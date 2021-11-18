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
#include <fault/internal.h>
#include <fault/python/environ.h>

#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
#elif CONFIG_SYSCALL_RETRY < 8
	#undef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
	#warning nope.
#endif

#ifndef INITIAL_TASKS_ALLOCATED
	#define INITIAL_TASKS_ALLOCATED 4
#endif

#ifndef MAX_TASKS_PER_SEGMENT
	#define MAX_TASKS_PER_SEGMENT 128
#endif

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
	// Signals that kernel.Events will listen for automatically.

	// SIGINT is handled fork.library.control() with signal.signal.
	// SIGUSR2 is *explicitly* used to trigger interjections.

	// *All* Kernel instances will receive signals.
*/
#define KQ_SIGNALS() \
	SIGNAME(SIGTERM) \
	SIGNAME(SIGCONT) \
	SIGNAME(SIGTSTP) \
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

#include "events.h"

static int
ev_kevent(
	Events ev,
	int retry, int *out,
	kevent_t *changes, int nchanges,
	kevent_t *events, int nevents,
	const struct timespec *timeout)
{
	RETRY_STATE_INIT;
	int r = -1;

	RETRY_SYSCALL:
	r = kevent(ev->ke_kqueue, changes, nchanges, events, nevents, timeout);
	if (r >= 0)
		*out = r;
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

/**
	// Set Python exception from kevent error.
*/
static int
ev_check_kevent(kevent_t *ev)
{
	if (ev->flags & EV_ERROR && ev->data != 0)
	{
		errno = ev->data;
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	return(1);
}

static inline int
interrupt_wait(Events ki)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	/**
		// Ignore interrupt if it's not waiting or has already been interrupted.
	*/
	if (ki->ke_waiting > 0)
	{
		kev.udata = (void *) ki;
		kev.ident = (uintptr_t) ki;
		kev.filter = EVFILT_USER;
		kev.fflags = NOTE_TRIGGER;
		kev.data = 0;
		kev.flags = EV_RECEIPT;

		if (!ev_kevent(ki, 1, &out, &kev, 1, NULL, 0, &ts))
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(-1);
		}

		ki->ke_waiting = -1;
		return(2);
	}
	else if (ki->ke_waiting < 0)
		return(1);
	else
		return(0);
}

/**
	// Append a memory allocation to the task queue.
*/
static int
ev_extend(Events ki, Tasks tail)
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
	ki->ke_tail->t_next = new;

	/* update position */
	ki->ke_tail = new;
	ki->ke_tailcursor = 0;

	return(0);
}

/**
	// Called to pop executing.
*/
static int
ev_queue_continue(Events ki)
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

	ki->ke_tail->t_allocated = ki->ke_tailcursor;
	ki->ke_executing = ki->ke_loading;

	ki->ke_tail = ki->ke_loading = n;
	ki->ke_loading->t_next = NULL;
	ki->ke_loading->t_allocated = INITIAL_TASKS_ALLOCATED;

	ki->ke_tailcursor = 0;

	return(0);
}

static PyObj
ev_enqueue(Events ki, PyObj callable)
{
	Tasks tail = ki->ke_tail;

	if (ki->ke_tailcursor == tail->t_allocated)
	{
		/* bit of a latent error */
		PyErr_SetString(PyExc_MemoryError, "task queue could not be extended and must be flushed");
		return(NULL);
	}

	tail->t_queue[ki->ke_tailcursor++] = callable;
	Py_INCREF(callable);

	if (ki->ke_tailcursor == tail->t_allocated)
		ev_extend(ki, tail);

	/* XXX: redundant condition; refactor force into inline */
	if (interrupt_wait(ki) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ev_execute(Events ki, PyObj errctl)
{
	Tasks exec = ki->ke_executing;
	Tasks next = NULL;
	PyObj task, xo;
	int i, c, total = 0;

	if (exec == NULL)
	{
		PyErr_SetString(PyExc_RuntimeError, "concurrent task queue execution");
		return(NULL);
	}

	/* signals processing */
	ki->ke_executing = NULL;

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

	if (KE_LQUEUE_HAS_TASKS(ki))
	{
		if (ev_queue_continue(ki) == -1)
		{
			/* re-init executing somehow? force instance dropped? */
			return(NULL);
		}
	}
	else
	{
		/* loading queue is empty; create empty executing queue */
		ki->ke_executing = PyMem_Malloc(sizeof(struct Tasks));
		ki->ke_executing->t_allocated = 0;
		ki->ke_executing->t_next = NULL;
	}

	return(PyLong_FromLong((long) total));
}

/*
	// Relay to the generated transit alloc functions.
	// See the preproc blackmagic at the bottom of the file.
*/

static int
ev_init(Events ev)
{
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t kev;

	ev->ke_kqueue = kqueue();
	if (ev->ke_kqueue == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	/*
		// Init USER filter for wait() interruptions.
	*/
	kev.udata = (void *) ev;
	kev.ident = (uintptr_t) ev;
	kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR;
	kev.filter = EVFILT_USER;
	kev.fflags = 0;
	kev.data = 0;

	if (!ev_kevent(ev, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		close(ev->ke_kqueue);
		ev->ke_kqueue = -1;
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
		if (!ev_kevent(ev, 1, &nkevents, &kev, 1, &kev, 1, &ts)) \
		{ \
			PyErr_SetFromErrno(PyExc_OSError); \
			close(ev->ke_kqueue); \
			ev->ke_kqueue = -1; \
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
ev_close(PyObj self)
{
	Events ev = (Events) self;
	PyObj rob = NULL;

	if (ev->ke_kqueue >= 0)
	{
		close(ev->ke_kqueue);
		ev->ke_kqueue = -1;
		ev->ke_waiting = -3;
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
ev_void(PyObj self)
{
	Events ev = (Events) self;

	if (ev->ke_kqueue != -1)
	{
		close(ev->ke_kqueue);
		ev->ke_kqueue = -1;
	}

	Py_XDECREF(ev->ke_kset);
	ev->ke_kset = NULL;

	Py_XDECREF(ev->ke_cancellations);
	ev->ke_cancellations = NULL;

	Py_RETURN_NONE;
}

static int
acquire_kernel_ref(Events ev, PyObj link)
{
	switch (PySet_Contains(ev->ke_kset, link))
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

	return(PySet_Add(ev->ke_kset, link));
}

/**
	// Begin listening for the process exit event.
*/
static PyObj
ev_track(PyObj self, PyObj args)
{
	const static struct timespec ts = {0,0};

	Events ev = (Events) self;

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

	if (!ev_kevent(ev, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	else
	{
		if (!ev_check_kevent(&kev))
			return(NULL);
	}

	Py_RETURN_NONE;
}

static PyObj
ev_untrack(PyObj self, PyObj args)
{
	const static struct timespec ts = {0,0};
	Events ev = (Events) self;

	long l;
	int nkevents;
	kevent_t kev;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	kev.udata = NULL;
	kev.data = 0;
	kev.ident = (uintptr_t) l;
	kev.flags = EV_DELETE|EV_RECEIPT;
	kev.filter = EVFILT_PROC;
	kev.fflags = NOTE_EXIT;

	if (!ev_kevent(ev, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	else
	{
		if (!ev_check_kevent(&kev))
			return(NULL);
	}

	Py_RETURN_NONE;
}

static PyObj
ev_interrupt(PyObj self)
{
	Events ev = (Events) self;
	PyObj rob;

	switch (interrupt_wait(ev))
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
set_timer(Events ev, int recur, int note, unsigned long quantity, PyObj link)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev;

	/* Reference must be managed by caller. */
	kev.ident = (uintptr_t) link;
	kev.udata = link;

	kev.fflags = note;
	kev.data = quantity;

	kev.filter = EVFILT_TIMER;
	kev.flags = EV_ADD|EV_RECEIPT|EV_ENABLE;
	if (!recur)
		kev.flags |= EV_ONESHOT;

	if (!ev_kevent(ev, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}
	else
	{
		if (!ev_check_kevent(&kev))
			return(0);
	}

	if (PySet_Add(ev->ke_kset, link) < 0)
	{
		/* error */
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
ev_defer(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Events ev = (Events) self;
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

	if (!set_timer(ev, 0, note, l, link))
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ev_recur(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Events ev = (Events) self;
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

	if (!set_timer(ev, 1, note, l, link))
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ev_cancel(PyObj self, PyObj link)
{
	const static struct timespec ts = {0,0};
	Events ev = (Events) self;
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

		if (!ev_kevent(ev, 1, &nkevents, &kev, 1, &kev, 1, &ts))
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(NULL);
		}
		else if (kev.flags & EV_ERROR && kev.data != 0)
		{
			switch (kev.data)
			{
				case ENOENT:
					/*
						// Validate not in kset.
					*/
					Py_RETURN_NONE;
				break;

				default:
					errno = kev.data;
					PyErr_SetFromErrno(PyExc_OSError);
					return(NULL);
				break;
			}
		}

		/*
			// Add to cancellation *after* successful kevent()
		*/
		if (PySet_Add(ev->ke_cancellations, link) < 0)
			return(NULL);
	}

	Py_RETURN_NONE;
}

static const char *
signal_string(int sig)
{
	switch (sig)
	{
		#define SIGNAL(SID, SYM, ...) case SID: return SYM; break;
			#include <ksignal.h>
		#undef SIGNAL

		default:
			return "";
		break;
	}
}

static PyObj
ev__set_waiting(PyObj self)
{
	Events ev = (Events) self;
	ev->ke_waiting = 1;
	Py_RETURN_NONE;
}

/**
	// collect and process kqueue events
*/
static PyObj
ev_wait(PyObj self, PyObj args)
{
	struct timespec waittime = {0,0};
	struct timespec *ref = &waittime;

	Events ev = (Events) self;
	PyObj rob;
	int i, nkevents = 0;
	int error = 0;
	long sleeptime = -1024;

	if (!PyArg_ParseTuple(args, "|l", &sleeptime))
		return(NULL);

	/* Validate opened. */
	if (ev->ke_kqueue == -1)
	{
		return(PyTuple_New(0));
	}

	if (KE_HAS_TASKS(ev))
	{
		waittime.tv_sec = 0;
		ev->ke_waiting = 0;
	}
	else
	{
		if (sleeptime >= 0)
		{
			waittime.tv_sec = sleeptime;
			ev->ke_waiting = sleeptime ? 1 : 0;
		}
		else
		{
			/* indefinite */
			ref = NULL;
			ev->ke_waiting = 1;
		}
	}

	Py_BEGIN_ALLOW_THREADS
	{
		if (!ev_kevent(ev, 0, &nkevents, NULL, 0, ev->ke_events, CONFIG_STATIC_KEVENTS, ref))
			error = 1;
	}
	Py_END_ALLOW_THREADS

	ev->ke_waiting = 0;
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

		kev = &(ev->ke_events[i]);
		link = (PyObj) kev->udata;

		switch (kev->filter)
		{
			case EVFILT_PROC:
				ob = Py_BuildValue("(slO)", "process", (long) kev->ident, link ? link : Py_None);
				if (kev->fflags & NOTE_EXIT && link != NULL)
				{
					/* done with filter entry */

					if (PySet_Discard(ev->ke_kset, link) < 0)
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

				switch (PySet_Contains(ev->ke_cancellations, link))
				{
					case 1:
					{
						if (PySet_Discard(ev->ke_kset, link) < 0)
						{
							error = 1;
						}

						if (PySet_Discard(ev->ke_cancellations, link) < 0)
						{
							error = 1;
						}

						continue;
					}
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

					if (PySet_Discard(ev->ke_kset, link) < 0)
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
	if (PySet_GET_SIZE(ev->ke_cancellations) > 0)
	{
		PyObj r = PyObject_CallMethod(ev->ke_kset, "difference_update", "O", ev->ke_cancellations);
		if (r != NULL)
			Py_DECREF(r);

		if (PySet_Clear(ev->ke_cancellations))
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
ev_methods[] = {
	#define PyMethod_Id(N) ev_##N
		{"force", (PyCFunction) ev_interrupt, METH_NOARGS, NULL},
		{"alarm", (PyCFunction) ev_defer, METH_VARARGS|METH_KEYWORDS, NULL},

		PyMethod_None(void),
		PyMethod_None(close),

		PyMethod_Variable(wait),
		PyMethod_None(interrupt),
		PyMethod_Sole(execute),

		PyMethod_Variable(track),
		PyMethod_Variable(untrack),

		PyMethod_Sole(cancel),
		PyMethod_Keywords(defer),
		PyMethod_Keywords(recur),
		PyMethod_Sole(enqueue),

		PyMethod_None(_set_waiting),
	#undef PyMethod_Id
	{NULL,},
};

static PyMemberDef ev_members[] = {
	{"waiting", T_PYSSIZET, offsetof(struct Events, ke_waiting), READONLY,
		PyDoc_STR("Whether or not the Events object is with statement block.")},
	{NULL,},
};

static PyObj
ev_get_closed(PyObj self, void *closure)
{
	Events ki = (Events) self;
	PyObj rob = Py_False;

	if (ki->ke_kqueue == -1)
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyObj
ev_get_has_tasks(PyObj self, void *closure)
{
	Events ki = (Events) self;
	PyObj rob = Py_False;

	if (KE_HAS_TASKS(ki))
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyGetSetDef ev_getset[] = {
	{"closed", ev_get_closed, NULL, NULL},
	{"loaded", ev_get_has_tasks, NULL, NULL},
	{NULL,},
};

static void
ev_clear_tasks(PyObj self)
{
	Events ev = (Events) self;
	Tasks t, n;
	size_t i;

	Tasks kx = ev->ke_executing;
	Tasks kt = ev->ke_tail;
	Tasks kl = ev->ke_loading;

	ev->ke_executing = ev->ke_loading = ev->ke_tail = NULL;

	/*
		// Executing queue's t_allocated provides accurate count of the segment.
	*/
	t = kx;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_CLEAR(t->t_queue[i]);

		PyMem_Free(t);
		t = n;
	}

	/*
		// Special case for final segment in loading.
	*/
	t = kt;
	if (t != NULL)
	{
		for (i = 0; i < ev->ke_tailcursor; ++i)
			Py_CLEAR(t->t_queue[i]);

		kt->t_allocated = 0;
	}

	/*
		// Prior loop on tail sets allocated to zero maintaining the invariant.
	*/
	t = kl;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_CLEAR(t->t_queue[i]);

		PyMem_Free(t);
		t = n;
	}
}

static int
ev_clear(PyObj self)
{
	Events ev = (Events) self;
	Py_CLEAR(ev->ke_kset);
	Py_CLEAR(ev->ke_cancellations);
	ev_clear_tasks(self);
	return(0);
}

static void
ev_dealloc(PyObj self)
{
	Events ev = (Events) self;

	ev_clear(self);

	if (ev->ke_kqueue != -1)
	{
		close(ev->ke_kqueue);
		PyErr_WarnFormat(PyExc_ResourceWarning, 0,
			FACTOR_PATH("Events") " instance not closed before deallocation");
	}

	Py_TYPE(self)->tp_free(self);
}

static int
ev_traverse_tasks(PyObj self, visitproc visit, void *arg)
{
	Events ev = (Events) self;
	Tasks t, n;
	size_t i;

	t = ev->ke_executing;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_VISIT(t->t_queue[i]);
		t = n;
	}

	t = ev->ke_tail;
	if (t != NULL)
	{
		for (i = 0; i < ev->ke_tailcursor; ++i)
			Py_VISIT(t->t_queue[i]);
	}

	t = ev->ke_loading;
	while (t != NULL && t != ev->ke_tail)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_VISIT(t->t_queue[i]);
		t = n;
	}
}

static int
ev_traverse(PyObj self, visitproc visit, void *arg)
{
	Events ev = (Events) self;
	Py_VISIT(ev->ke_kset);
	Py_VISIT(ev->ke_cancellations);
	return(ev_traverse_tasks(self, visit, arg));
}

static int
init_queue(Events ev)
{
	ev->ke_loading = PyMem_Malloc(
		sizeof(struct Tasks) + (INITIAL_TASKS_ALLOCATED * sizeof(PyObject *))
	);
	if (ev->ke_loading == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	ev->ke_loading->t_next = NULL;
	ev->ke_loading->t_allocated = INITIAL_TASKS_ALLOCATED;

	ev->ke_executing = PyMem_Malloc(
		sizeof(struct Tasks) + (0 * sizeof(PyObject *))
	);
	if (ev->ke_executing == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	ev->ke_executing->t_next = NULL;
	ev->ke_executing->t_allocated = 0;

	ev->ke_tail = ev->ke_loading;
	ev->ke_tailcursor = 0;
	return(0);
}

static PyObj
ev_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	Events ev;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL);

	ev = (Events) subtype->tp_alloc(subtype, 0);
	if (ev == NULL)
		return(NULL);

	if (init_queue(ev) == -1)
	{
		Py_DECREF(ev);
		return(NULL);
	}

	ev->ke_kqueue = -1;
	ev->ke_waiting = 0;

	ev->ke_kset = PySet_New(0);
	if (ev->ke_kset == NULL)
	{
		Py_DECREF(ev);
		return(NULL);
	}

	ev->ke_cancellations = PySet_New(0);
	if (ev->ke_cancellations == NULL)
	{
		Py_DECREF(ev);
		return(NULL);
	}

	if (!ev_init(ev))
	{
		Py_DECREF(((PyObj) ev));
		return(NULL);
	}

	return((PyObj) ev);
}

/**
	// &.kernel.Events
*/
PyTypeObject
EventsType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	FACTOR_PATH("Events"),        /* tp_name */
	sizeof(struct Events),        /* tp_basicsize */
	0,                            /* tp_itemsize */
	ev_dealloc,                   /* tp_dealloc */
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
	NULL,                         /* tp_doc */
	ev_traverse,                  /* tp_traverse */
	ev_clear,                     /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	NULL,                         /* tp_iter */
	NULL,                         /* tp_iternext */
	ev_methods,                   /* tp_methods */
	ev_members,                   /* tp_members */
	ev_getset,                    /* tp_getset */
	NULL,                         /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	ev_new,                       /* tp_new */
};
