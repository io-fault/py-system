/**
	// Kernel interfaces for process invocation and system signal management.

	// [ Engineering ]
	// Currently, this is lacking some aggressive testing and many error branches
	// are not exercised for coverage; considering how critical the task queue, this is not appropriate.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <sys/event.h>

#include <fault/libc.h>
#include <fault/python/environ.h>

typedef struct kevent kevent_t; /* kernel event description */
#include "interface.h"

#define KQ_FILTERS() \
	FILTER(EVFILT_USER) \
	FILTER(EVFILT_PROC) \
	FILTER(EVFILT_SIGNAL) \
	FILTER(EVFILT_VNODE) \
	FILTER(EVFILT_TIMER)

#define KQ_FLAGS() \
	FLAG(EV_ADD) \
	FLAG(EV_ENABLE) \
	FLAG(EV_DISABLE) \
	FLAG(EV_DELETE) \
	FLAG(EV_RECEIPT) \
	FLAG(EV_ONESHOT) \
	FLAG(EV_CLEAR) \
	FLAG(EV_EOF) \
	FLAG(EV_ERROR)

/**
	// Signals that kernel.Interface will listen for automatically.

	// SIGINT is handled fork.library.control() with signal.signal.
	// SIGUSR2 is *explicitly* used to trigger interjections.

	// *All* Kernel instances will receive signals.
*/
#define KQ_SIGNALS() \
	SIGNAME(SIGTERM) \
	SIGNAME(SIGCONT) \
	SIGNAME(SIGWINCH) \
	SIGNAME(SIGINFO) \
	SIGNAME(SIGHUP) \
	SIGNAME(SIGUSR1) \
	SIGNAME(SIGURG)

/**
	// SIGPOLL is POSIX+XSI and is apparently ignored on Darwin.
	// SIGURG is apparently widely available.
	// SIGIO doesn't exist on Linux.
	// SIGINFO is only available on Darwin and BSD.
*/

/**
	// SIGNAME(SIGUSR2)

	// SIGUSR2 is used to interrupt the main thread. This is used
	// to allow system.interject() to operate while in a system call.
*/

#ifndef HAVE_STDINT_H
	/* relying on Python's checks */
	#include <stdint.h>
#endif

#ifndef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
#elif CONFIG_STATIC_KEVENTS < 8
	#undef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
	#warning nope.
#endif

#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
#elif CONFIG_SYSCALL_RETRY < 8
	#undef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
	#warning nope.
#endif

/*
	// Limited retry state.
*/
#define _RETRY_STATE _avail_retries
#define RETRY_STATE_INIT int _RETRY_STATE = CONFIG_SYSCALL_RETRY
#define LIMITED_RETRY() do { \
	if (_RETRY_STATE > 0) { \
		errno = 0; \
		--_RETRY_STATE; \
		goto RETRY_SYSCALL; \
	} \
} while(0);
#define UNLIMITED_RETRY() errno = 0; goto RETRY_SYSCALL;

static int
ki_kevent(
	Interface kif,
	int retry, int *out,
	kevent_t *changes, int nchanges,
	kevent_t *events, int nevents,
	const struct timespec *timeout)
{
	RETRY_STATE_INIT;
	int r = -1;

	RETRY_SYSCALL:
	r = kevent(kif->kif_kqueue, changes, nchanges, events, nevents, timeout);
	if (r >= 0)
	{
		/*
			// EV_ERROR is used in cases where kevent(2) fails after it already processed
			// some events. In these cases, the EV_ERROR flag is used to note the case.
		*/
		if (r > 0 && events[r-1].flags & EV_ERROR)
		{
			--r;
			*out = r;
			/*
				// XXX: Set error from EV_ERROR?
			*/
		}
		else
			*out = r;
	}
	else
	{
		/*
			// Complete failure. Probably an interrupt or EINVAL.
		*/
		*out = 0;

		switch (errno)
		{
			case EINTR:
				/*
					// The caller can designate whether or not retry will occur.
				*/
				switch (retry)
				{
					case -1:
						UNLIMITED_RETRY();
					break;

					case 1:
						LIMITED_RETRY();
					break;

					case 0:
						/*
							// Purposefully allow it to fall through. Usually a signal occurred.
							// Falling through is appropriate as it usually means
							// processing an empty task queue.
						*/
						*out = 0;
						return(1);
					break;
				}
			case ENOMEM:
				LIMITED_RETRY();

			default:
				return(0);
			break;
		}
	}

	return(1);
}

static inline int
ki_force_event(Interface ki)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	/**
		// Ignore force if it's not waiting or has already forced.
	*/
	if (ki->kif_waiting > 0)
	{
		kev.udata = (void *) ki;
		kev.ident = (uintptr_t) ki;
		kev.filter = EVFILT_USER;
		kev.fflags = NOTE_TRIGGER;
		kev.data = 0;
		kev.flags = EV_RECEIPT;

		if (!ki_kevent(ki, 1, &out, &kev, 1, NULL, 0, &ts))
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(-1);
		}

		ki->kif_waiting = -1;
		return(2);
	}
	else if (ki->kif_waiting < 0)
		return(1);
	else
		return(0);
}

/**
	// Append a memory allocation to the task queue.
*/
static int
ki_extend(Interface ki, Tasks tail)
{
	Tasks new;
	size_t count = tail->t_allocated;

	if (count < MAX_TASKS_PER_SEGMENT)
		count *= 2;

	new = PyMem_Malloc(sizeof(struct Tasks) + (sizeof(PyObject *) * count));
	if (new == NULL)
		return(-1);

	new->t_next = NULL;
	new->t_allocated = count;
	ki->kif_tail->t_next = new;

	/* update position */
	ki->kif_tail = new;
	ki->kif_tailcursor = 0;

	return(0);
}

/**
	// Called to pop executing.
*/
static int
ki_queue_continue(Interface ki)
{
	Tasks n = NULL;

	n = PyMem_Malloc(
		sizeof(struct Tasks) + (INITIAL_TASKS_ALLOCATED * sizeof(PyObject *))
	);
	if (n == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue continuation");
		return(-1);
	}

	ki->kif_tail->t_allocated = ki->kif_tailcursor;
	ki->kif_executing = ki->kif_loading;

	ki->kif_tail = ki->kif_loading = n;
	ki->kif_loading->t_next = NULL;
	ki->kif_loading->t_allocated = INITIAL_TASKS_ALLOCATED;

	ki->kif_tailcursor = 0;

	return(0);
}

static PyObj
ki_enqueue_task(Interface ki, PyObj callable)
{
	Tasks tail = ki->kif_tail;

	if (ki->kif_tailcursor == tail->t_allocated)
	{
		/* bit of a latent error */
		PyErr_SetString(PyExc_MemoryError, "task queue could not be extended and must be flushed");
		return(NULL);
	}

	tail->t_queue[ki->kif_tailcursor++] = callable;
	Py_INCREF(callable);

	if (ki->kif_tailcursor == tail->t_allocated)
		ki_extend(ki, tail);

	/* XXX: redundant condition; refactor force into inline */
	if (ki_force_event(ki) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ki_execute_tasks(Interface ki, PyObj errctl)
{
	Tasks exec = ki->kif_executing;
	Tasks next = NULL;
	PyObj task, xo;
	int i, c, total = 0;

	if (exec == NULL)
	{
		PyErr_SetString(PyExc_RuntimeError, "concurrent task queue execution");
		return(NULL);
	}

	/* signals processing */
	ki->kif_executing = NULL;

	do {
		for (i = 0, c = exec->t_allocated; i < c; ++i)
		{
			task = exec->t_queue[i];
			xo = PyObject_CallObject(task, NULL);
			total += 1;

			if (xo == NULL)
			{
				PyObj exc, val, tb;
				PyErr_Fetch(&exc, &val, &tb);

				if (errctl != Py_None)
				{
					PyObj ereturn;

					PyErr_NormalizeException(&exc, &val, &tb);
					if (PyErr_Occurred())
					{
						/* normalization failed? */
						PyErr_WriteUnraisable(task);
						PyErr_Clear();
					}
					else
					{
						if (tb != NULL)
						{
							PyException_SetTraceback(val, tb);
							Py_DECREF(tb);
						}

						ereturn = PyObject_CallFunctionObjArgs(errctl, task, val, NULL);
						if (ereturn)
							Py_DECREF(ereturn);
						else
						{
							/* errctl raised exception */
							PyErr_WriteUnraisable(task);
							PyErr_Clear();
						}
					}
				}
				else
				{
					/* explicitly discarded */
					Py_XDECREF(tb);
				}

				Py_XDECREF(exc);
				Py_XDECREF(val);
			}
			else
			{
				Py_DECREF(xo);
			}

			Py_DECREF(task);
		}

		next = exec->t_next;
		PyMem_Free(exec);
		exec = next;
	}
	while (exec != NULL);

	if (KI_LQUEUE_HAS_TASKS(ki))
	{
		if (ki_queue_continue(ki) == -1)
		{
			/* re-init executing somehow? force instance dropped? */
			return(NULL);
		}
	}
	else
	{
		/* loading queue is empty; create empty executing queue */
		ki->kif_executing = PyMem_Malloc(sizeof(struct Tasks));
		ki->kif_executing->t_allocated = 0;
		ki->kif_executing->t_next = NULL;
	}

	return(PyLong_FromLong((long) total));
}

/*
	// Relay to the generated transit alloc functions.
	// See the preproc blackmagic at the bottom of the file.
*/

static int
ki_init(Interface kif)
{
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t kev;

	kif->kif_kqueue = kqueue();
	if (kif->kif_kqueue == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	/*
		// Init USER filter for wait() interruptions.
	*/
	kev.udata = (void *) kif;
	kev.ident = (uintptr_t) kif;
	kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR;
	kev.filter = EVFILT_USER;
	kev.fflags = 0;
	kev.data = 0;

	if (!ki_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		close(kif->kif_kqueue);
		kif->kif_kqueue = -1;
		return(0);
	}

	#define SIGNAME(SN) \
		kev.udata = NULL; \
		kev.data = 0; \
		kev.ident = (uintptr_t) SN; \
		kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR; \
		kev.filter = EVFILT_SIGNAL; \
		kev.fflags = 0; \
		\
		if (!ki_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts)) \
		{ \
			PyErr_SetFromErrno(PyExc_OSError); \
			close(kif->kif_kqueue); \
			kif->kif_kqueue = -1; \
			return(0); \
		}

	KQ_SIGNALS()
	#undef SIGNAME

	return(1);
}

/**
	// Close the kqueue FD.
*/
static PyObj
ki_close(PyObj self)
{
	Interface kif = (Interface) self;
	PyObj rob = NULL;

	if (kif->kif_kqueue >= 0)
	{
		close(kif->kif_kqueue);
		kif->kif_kqueue = -1;
		kif->kif_waiting = -3;
		rob = Py_True;
	}
	else
		rob = Py_False;

	Py_INCREF(rob);
	return(rob);
}

/**
	// Close the kqueue FD, and release references.
*/
static PyObj
ki_void(PyObj self)
{
	Interface kif = (Interface) self;

	if (kif->kif_kqueue != -1)
	{
		close(kif->kif_kqueue);
		kif->kif_kqueue = -1;
	}

	Py_XDECREF(kif->kif_kset);
	kif->kif_kset = NULL;

	Py_XDECREF(kif->kif_cancellations);
	kif->kif_cancellations = NULL;

	Py_RETURN_NONE;
}

static int
acquire_kernel_ref(Interface kif, PyObj link)
{
	switch (PySet_Contains(kif->kif_kset, link))
	{
		case 1:
			PyErr_SetString(PyExc_RuntimeError, "link already referenced by kernel");
			return(-1);
		break;

		case 0:
			/* not present */
		break;

		default:
			return(-1);
		break;
	}

	return(PySet_Add(kif->kif_kset, link));
}

/**
	// Begin listening for the process exit event.
*/
static PyObj
ki_track(PyObj self, PyObj args)
{
	const static struct timespec ts = {0,0};

	Interface kif = (Interface) self;

	long l;
	int nkevents;
	kevent_t kev;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	kev.udata = NULL;
	kev.data = 0;
	kev.ident = (uintptr_t) l;
	kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR;
	kev.filter = EVFILT_PROC;
	kev.fflags = NOTE_EXIT;

	if (!ki_kevent(kif, 1, &nkevents, &kev, 1, &kev, 0, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	else
	{
		if (kev.filter == EV_ERROR)
		{
			errno = (int) kev.data;
			PyErr_SetFromErrno(PyExc_OSError);
			return(NULL);
		}
	}

	Py_RETURN_NONE;
}

static PyObj
ki_untrack(PyObj self, PyObj args)
{
	const static struct timespec ts = {0,0};
	Interface kif = (Interface) self;

	long l;
	int nkevents;
	kevent_t kev;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	kev.udata = NULL;
	kev.data = 0;
	kev.ident = (uintptr_t) l;
	kev.flags = EV_DELETE;
	kev.filter = EVFILT_PROC;
	kev.fflags = NOTE_EXIT;

	if (!ki_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	else
	{
		if (kev.filter == EV_ERROR)
		{
			errno = (int) kev.data;
			PyErr_SetFromErrno(PyExc_OSError);
			return(NULL);
		}
	}

	Py_RETURN_NONE;
}

static PyObj
ki_force(PyObj self)
{
	Interface kif = (Interface) self;
	PyObj rob;

	switch (ki_force_event(kif))
	{
		case -1:
			return(NULL);
		break;

		case 0:
			rob = Py_None;
		break;

		case 1:
			rob = Py_False;
		break;

		case 2:
			rob = Py_True;
		break;
	}

	Py_INCREF(rob);
	return(rob);
}

static int
set_timer(Interface kif, int recur, int note, unsigned long quantity, PyObj link)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev;

	kev.ident = (uintptr_t) link;
	kev.udata = link;

	kev.fflags = note;
	kev.data = quantity;

	kev.filter = EVFILT_TIMER;
	kev.flags = EV_ADD|EV_RECEIPT|EV_ENABLE;
	if (!recur)
		kev.flags |= EV_ONESHOT;

	if (!ki_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	return(1);
}

static int
note_unit(int unit, unsigned long *l)
{
	const int ms = (0xCE << 2) | 0xBC;
	int note;

	switch (unit)
	{
		#ifdef NOTE_NSECONDS
			case 'n':
				note = NOTE_NSECONDS;
			break;
		#else
			#warning Converting nanoseconds to milliseconds when necessary.
			case 'n':
				/* nanoseconds to milliseconds */
				note = 0;
				*l = *l / 1000000;
			break;
		#endif

		#ifdef NOTE_USECONDS
			case 'u':
			case ms:
				note = NOTE_USECONDS; /* microseconds */
			break;
		#else
			#warning Converting microseconds to milliseconds when necessary.
			case 'u':
				/* microseconds to milliseconds */
				note = 0;
				*l = *l / 1000;
			break;
		#endif

		#ifdef NOTE_SECONDS
			case 's':
				note = NOTE_SECONDS;
			break;
		#else
			#warning Converting seconds to milliseconds when necessary.
			case 's':
				/* microseconds to milliseconds */
				note = 0;
				*l = (*l) * 1000;
			break;
		#endif

		case 'm':
			note = 0; /* milliseconds */
		break;

		default:
			PyErr_Format(PyExc_ValueError, "invalid unit code '%c' for timer", unit);
			note = -1;
		break;
	}

	return(note);
}

static PyObj
ki_alarm(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Interface kif = (Interface) self;
	PyObj link = NULL;
	unsigned long l = 0;
	int unit = 's';
	int note;

	/*
		// (link_object, period, unit)
	*/
	if (!PyArg_ParseTupleAndKeywords(args, kw, "Ok|C", (char **) kwlist, &link, &l, &unit))
		return(NULL);

	note = note_unit(unit, &l);
	if (note < 0)
		return(NULL);

	if (!set_timer(kif, 0, note, l, link))
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ki_recur(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Interface kif = (Interface) self;
	PyObj link = NULL;
	unsigned long l = 0;
	int unit = 's';
	int note;

	/*
		// (link_object, period, unit)
	*/
	if (!PyArg_ParseTupleAndKeywords(args, kw, "Ok|C", (char **) kwlist, &link, &l, &unit))
		return(NULL);

	note = note_unit(unit, &l);
	if (note < 0)
		return(NULL);

	if (!set_timer(kif, 1, note, l, link))
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ki_cancel(PyObj self, PyObj link)
{
	const static struct timespec ts = {0,0};
	Interface kif = (Interface) self;
	int nkevents = 0;
	kevent_t kev;

	if (link != Py_None)
	{
		kev.ident = (uintptr_t) link;
		kev.udata = 0;

		kev.fflags = 0;
		kev.data = 0;

		kev.filter = EVFILT_TIMER;
		kev.flags = EV_DELETE|EV_RECEIPT;

		if (!ki_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts))
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(NULL);
		}

		/*
			// Add to cancellation *after* successful kevent()
		*/
		if (PySet_Add(kif->kif_cancellations, link) < 0)
			return(NULL);
	}

	Py_RETURN_NONE;
}

static const char *
signal_string(int sig)
{
	switch (sig)
	{
		case SIGCONT:
			return "continue";
		break;

		case SIGTERM:
			return "terminate";
		break;

		case SIGHUP:
			return "delta";
		break;

		case SIGURG:
			return "urgent";
		break;

		#ifdef SIGINFO
			case SIGINFO:
				return "terminal.query";
			break;
		#endif

		#ifdef SIGWINCH
			case SIGWINCH:
				return "terminal.delta";
			break;
		#endif

		case SIGUSR1:
			return "tunnel";
		break;

		case SIGUSR2:
			/* Should be ignored; used to interrupt blocking system calls in the main thread. */
			return "trip";
		break;

		default:
			return "";
		break;
	}
}

static PyObj
ki_set_waiting(PyObj self)
{
	Interface kif = (Interface) self;
	kif->kif_waiting = 1;
	Py_RETURN_NONE;
}

/**
	// collect and process kqueue events
*/
static PyObj
ki_wait(PyObj self, PyObj args)
{
	struct timespec waittime = {0,0};
	struct timespec *ref = &waittime;

	Interface kif = (Interface) self;
	PyObj rob;
	int i, nkevents = 0;
	int error = 0;
	long sleeptime = -1024;

	if (!PyArg_ParseTuple(args, "|l", &sleeptime))
		return(NULL);

	/* Validate opened. */
	if (kif->kif_kqueue == -1)
	{
		return(PyTuple_New(0));
	}

	if (KI_HAS_TASKS(kif))
	{
		waittime.tv_sec = 0;
		kif->kif_waiting = 0;
	}
	else
	{
		if (sleeptime >= 0)
		{
			waittime.tv_sec = sleeptime;
			kif->kif_waiting = sleeptime ? 1 : 0;
		}
		else
		{
			/* indefinite */
			ref = NULL;
			kif->kif_waiting = 1;
		}
	}

	Py_BEGIN_ALLOW_THREADS
	if (!ki_kevent(kif, 0, &nkevents, NULL, 0, kif->kif_events, CONFIG_STATIC_KEVENTS, ref))
	{
		error = 1;
	}
	Py_END_ALLOW_THREADS

	kif->kif_waiting = 0;
	if (error)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	/*
		// Process the collected events.
	*/
	rob = PyList_New(0);
	if (rob == NULL)
		return(NULL);

	if (nkevents == 0 && waittime.tv_sec > 0)
	{
		/* No events and definite wait time */
		error = PyList_Append(rob, Py_BuildValue("sl", "timeout", waittime.tv_sec));

		if (error == -1 || PyList_GET_ITEM(rob, 0) == NULL)
		{
			Py_DECREF(rob);
			return(NULL);
		}
	}

	for (i = 0; i < nkevents; ++i)
	{
		PyObj link;
		kevent_t *kev;
		PyObj ob;

		kev = &(kif->kif_events[i]);
		link = (PyObj) kev->udata;

		switch (kev->filter)
		{
			case EVFILT_PROC:
				ob = Py_BuildValue("(slO)", "process", (long) kev->ident, link ? link : Py_None);
				if (kev->fflags & NOTE_EXIT && link != NULL)
				{
					/* done with filter entry */

					if (PySet_Discard(kif->kif_kset, link) < 0)
					{
						/* note internal error */
						error = 1;
					}
				}
			break;

			case EVFILT_SIGNAL:
				ob = Py_BuildValue("(ss)", "signal", signal_string(kev->ident));
			break;

			case EVFILT_TIMER:
			{
				if (link == NULL)
					/* core status? */
					continue;

				switch (PySet_Contains(kif->kif_cancellations, link))
				{
					case 1:
						if (PySet_Discard(kif->kif_kset, link) < 0)
						{
							/* error */
							error = 1;
						}
						else if (PySet_Discard(kif->kif_cancellations, link) < 0)
						{
							/* error */
							error = 1;
						}

						continue;
					break;
					case 0:
						/* not cancelled */
					break;
					default:
						/* -1 error */
						error = 1;
					break;
				}

				if (kev->flags & EV_ONESHOT)
				{
					ob = Py_BuildValue("(sO)", "alarm", link);

					if (PySet_Discard(kif->kif_kset, link) < 0)
					{
						/* error */
						error = 1;
					}
				}
				else
				{
					ob = Py_BuildValue("(sO)", "recur", link);
				}
			}
			break;

			case EVFILT_USER:
				/*
					// event from force() method
				*/
				continue;
			break;

			default:
				/*
					// unknown event, throw warning
				*/
				continue;
			break;
		}

		if (ob == NULL || error != 0)
		{
			Py_DECREF(rob);
			return(NULL);
		}
		PyList_Append(rob, ob);
	}

	/*
		// Complete timer cancellations.
	*/
	if (PySet_GET_SIZE(kif->kif_cancellations) > 0)
	{
		PyObj r = PyObject_CallMethod(kif->kif_kset, "difference_update", "O", kif->kif_cancellations);
		if (r != NULL)
			Py_DECREF(r);

		if (PySet_Clear(kif->kif_cancellations))
		{
			/* error? Documentation (3.5) doesn't state error code. */
			/*
				// XXX: Exception needs to be communicated on a side channel.
					// Destroying the collected events is dangerous.
			*/
			Py_DECREF(rob);
			return(NULL);
		}
	}

	return(rob);
}

static PyMethodDef
ki_methods[] = {
	{"close",
		(PyCFunction) ki_close,
		METH_NOARGS, PyDoc_STR(
			"Close the interface eliminating the possibility of receiving events from the kernel.\n"
		)
	},

	{"void",
		(PyCFunction) ki_void,
		METH_NOARGS, PyDoc_STR(
			"Destroy the Interface instance, closing any file descriptors managed by the object.\n"
			"Also destroy the internal set-object for holding kernel references.\n"
		)
	},

	{"track",
		(PyCFunction) ki_track, METH_VARARGS,
		PyDoc_STR(
			"Listen for the process exit event."
		)
	},

	{"untrack",
		(PyCFunction) ki_untrack, METH_VARARGS,
		PyDoc_STR(
			"Stop listening for the process exit event."
		)
	},

	{"alarm",
		(PyCFunction) ki_alarm, METH_VARARGS|METH_KEYWORDS,
		PyDoc_STR(
			"Allocate a one-time timer that will cause an event after the designed period.\n"
		)
	},

	{"recur",
		(PyCFunction) ki_recur, METH_VARARGS|METH_KEYWORDS,
		PyDoc_STR(
			"Allocate a recurring timer that will cause an event at the designed frequency.\n"
		)
	},

	{"cancel",
		(PyCFunction) ki_cancel, METH_O,
		PyDoc_STR(
			"Cancel a timer, recurring or once, using the link that the timer was allocated with.\n"
		)
	},

	{"force",
		(PyCFunction) ki_force, METH_NOARGS,
		PyDoc_STR(
			"Cause a corresponding &wait call to stop waiting **if** the Interface\n"
			"instance is inside a with-statement block::\n"
		)
	},

	{"wait",
		(PyCFunction) ki_wait, METH_VARARGS,
		PyDoc_STR(
			"Executed after entering a with-statement block to collect queued events or timeout.\n"
		)
	},

	{"enqueue",
		(PyCFunction) ki_enqueue_task, METH_O,
		PyDoc_STR(
			"Enqueue a task for execution.\n"
		)
	},
	{"execute",
		(PyCFunction) ki_execute_tasks, METH_O,
		PyDoc_STR(
			"Execute recently enqueued, FIFO, tasks and prepare for the next cycle.\n"
		)
	},

	{"_set_waiting",
		(PyCFunction) ki_set_waiting, METH_NOARGS,
		PyDoc_STR(
			"Set waiting state for testing."
		)
	},

	{NULL,},
};

static PyMemberDef ki_members[] = {
	{"waiting", T_PYSSIZET, offsetof(struct Interface, kif_waiting), READONLY,
		PyDoc_STR("Whether or not the Interface object is with statement block.")},
	{NULL,},
};

static PyObj
ki_get_closed(PyObj self, void *closure)
{
	Interface ki = (Interface) self;
	PyObj rob = Py_False;

	if (ki->kif_kqueue == -1)
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyObj
ki_get_has_tasks(PyObj self, void *closure)
{
	Interface ki = (Interface) self;
	PyObj rob = Py_False;

	if (KI_HAS_TASKS(ki))
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyGetSetDef ki_getset[] = {
	{"closed", ki_get_closed, NULL,
		PyDoc_STR("whether the interface's kernel connection is closed")},
	{"loaded", ki_get_has_tasks, NULL,
		PyDoc_STR("whether the queue has been loaded with any number of tasks")},
	{NULL,},
};

static void
ki_dealloc(PyObj self)
{
	Interface kif = (Interface) self;
	Tasks t, n;
	size_t i;

	Py_XDECREF(kif->kif_kset);
	kif->kif_kset = NULL;
	Py_XDECREF(kif->kif_cancellations);
	kif->kif_cancellations = NULL;

	if (kif->kif_kqueue != -1)
	{
		close(kif->kif_kqueue);
		PyErr_WarnFormat(PyExc_ResourceWarning, 0,
			FACTOR_PATH("Interface") " instance not closed before deallocation");
	}

	/*
		// Executing queue's t_allocated provides accurate count of the segment.
	*/
	t = kif->kif_executing;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_DECREF(t->t_queue[i]);

		PyMem_Free(t);
		t = n;
	}

	/*
		// Special case for final segment in loading.
	*/
	t = kif->kif_tail;
	if (t != NULL)
	{
		for (i = 0; i < kif->kif_tailcursor; ++i)
			Py_DECREF(t->t_queue[i]);

		kif->kif_tail->t_allocated = 0;
	}

	/*
		// Prior loop on tail sets allocated to zero maintaining the invariant.
	*/
	t = kif->kif_loading;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_DECREF(t->t_queue[i]);

		PyMem_Free(t);
		t = n;
	}

	kif->kif_executing = kif->kif_loading = kif->kif_tail = NULL;
}

static int
ki_clear(PyObj self)
{
	Interface kif = (Interface) self;
	Py_CLEAR(kif->kif_kset);
	Py_CLEAR(kif->kif_cancellations);
	return(0);
}

static int
ki_traverse(PyObj self, visitproc visit, void *arg)
{
	Interface kif = (Interface) self;
	Py_VISIT(kif->kif_kset);
	Py_VISIT(kif->kif_cancellations);
	return(0);
}

static int
init_queue(Interface kif)
{
	kif->kif_loading = PyMem_Malloc(
		sizeof(struct Tasks) + (INITIAL_TASKS_ALLOCATED * sizeof(PyObject *))
	);
	if (kif->kif_loading == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	kif->kif_loading->t_next = NULL;
	kif->kif_loading->t_allocated = INITIAL_TASKS_ALLOCATED;

	kif->kif_executing = PyMem_Malloc(
		sizeof(struct Tasks) + (0 * sizeof(PyObject *))
	);
	if (kif->kif_executing == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	kif->kif_executing->t_next = NULL;
	kif->kif_executing->t_allocated = 0;

	kif->kif_tail = kif->kif_loading;
	kif->kif_tailcursor = 0;
	return(0);
}

static PyObj
ki_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	Interface kif;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL);

	kif = (Interface) subtype->tp_alloc(subtype, 0);
	if (kif == NULL)
		return(NULL);

	if (init_queue(kif) == -1)
	{
		Py_DECREF(kif);
		return(NULL);
	}

	kif->kif_kqueue = -1;
	kif->kif_waiting = 0;

	kif->kif_kset = PySet_New(0);
	if (kif->kif_kset == NULL)
	{
		Py_DECREF(kif);
		return(NULL);
	}

	kif->kif_cancellations = PySet_New(0);
	if (kif->kif_cancellations == NULL)
	{
		Py_DECREF(kif);
		return(NULL);
	}

	if (!ki_init(kif))
	{
		Py_DECREF(((PyObj) kif));
		return(NULL);
	}

	return((PyObj) kif);
}

PyDoc_STRVAR(ki_doc,
"The kernel Interface implementation providing event driven signalling "
"for control signals, subprocess exits, timers, and a task queue.");

PyTypeObject
InterfaceType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	FACTOR_PATH("Interface"),     /* tp_name */
	sizeof(struct Interface),     /* tp_basicsize */
	0,                            /* tp_itemsize */
	ki_dealloc,                   /* tp_dealloc */
	NULL,                         /* tp_print */
	NULL,                         /* tp_getattr */
	NULL,                         /* tp_setattr */
	NULL,                         /* tp_compare */
	NULL,                         /* tp_repr */
	NULL,                         /* tp_as_number */
	NULL,                         /* tp_as_sequence */
	NULL,                         /* tp_as_mapping */
	NULL,                         /* tp_hash */
	NULL,                         /* tp_call */
	NULL,                         /* tp_str */
	NULL,                         /* tp_getattro */
	NULL,                         /* tp_setattro */
	NULL,                         /* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_HAVE_GC|
	Py_TPFLAGS_DEFAULT,           /* tp_flags */
	ki_doc,                       /* tp_doc */

	ki_traverse,                  /* tp_traverse */
	ki_clear,                     /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	NULL,                         /* tp_iter */
	NULL,                         /* tp_iternext */
	ki_methods,                   /* tp_methods */
	ki_members,                   /* tp_members */
	ki_getset,                    /* tp_getset */
	NULL,                         /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	ki_new,                       /* tp_new */
};

#define PYTHON_TYPES() \
	ID(Interface)

#include <fault/python/module.h>
INIT(PyDoc_STR("Kernel interfaces for supporting nucleus based process management.\n"))
{
	PyObj mod = NULL;

	CREATE_MODULE(&mod);
	if (mod == NULL)
		return(NULL);

	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(mod, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;

		PYTHON_TYPES()
	#undef ID

	return(mod);

	error:
		DROP_MODULE(mod);
		return(NULL);
}
