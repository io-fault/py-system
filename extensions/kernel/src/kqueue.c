/**
	// kqueue based KernelQueue implementation.
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

#ifndef HAVE_STDINT_H
	/* relying on Python's checks */
	#include <stdint.h>
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

#include "taskq.h"
#include "kernelq.h"
#include "events.h"
#include "signals.h"

STATIC(int)
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

STATIC(int)
kernelq_kevent(KernelQueue kq,
	int retry, int *out,
	kevent_t *changes, int nchanges,
	kevent_t *events, int nevents,
	const struct timespec *timeout)
{
	RETRY_STATE_INIT;
	int r = -1;

	RETRY_SYSCALL:
	r = kevent(kq->kq_root, changes, nchanges, events, nevents, timeout);
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
						return(-1);
					break;
				}
			case ENOMEM:
				LIMITED_RETRY();

			default:
				return(-1);
			break;
		}
	}

	return(0);
}

/**
	// Set Python exception from kevent error.
*/
STATIC(int)
check_kevent(kevent_t *ev)
{
	if (ev->flags & EV_ERROR && ev->data != 0)
	{
		errno = ev->data;
		PyErr_SetFromErrno(PyExc_OSError);
		return(-1);
	}

	return(0);
}

CONCEAL(int)
kernelq_interrupt(KernelQueue kq)
{
	const struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.udata = (void *) kq,
		.ident = (uintptr_t) kq,
		.filter = EVFILT_USER,
		.fflags = NOTE_TRIGGER,
		.data = 0,
		.flags = EV_RECEIPT,
	};

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, NULL, 0, &ts) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-1);
	}

	return(0);
}

CONCEAL(int)
kernelq_setup_interrupt(KernelQueue kq)
{
	const struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.udata = (void *) kq,
		.ident = (uintptr_t) kq,
		.flags = EV_ADD|EV_RECEIPT|EV_CLEAR,
		.filter = EVFILT_USER,
		.fflags = 0,
		.data = 0,
	};

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, &kev, 1, &ts) < 0)
		return(-1);

	return(0);
}

#define SIGNALT \
	.flags = 0, \
	.data = 0, \
	.flags = EV_ADD|EV_RECEIPT|EV_CLEAR, \
	.filter = EVFILT_SIGNAL

CONCEAL(int)
kernelq_setup_signals(KernelQueue kq, void *ref)
{
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t keva[] = {
		#define SIGNAME(SN) {SIGNALT, .ident = SN, .udata = ref},
			SIGNALS()
		#undef SIGNAME
	};
	nkevents = sizeof(keva) / sizeof(kevent_t);

	if (kernelq_kevent(kq, 1, &nkevents, keva, nkevents, keva, nkevents, &ts) < 0)
		return(-1);

	if (nkevents != (sizeof(keva) / sizeof(kevent_t)))
	{
		printf("inconsistency\n");
	}

	return(0);
}

CONCEAL(int)
kernelq_initialize(KernelQueue kq)
{
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t kev;

	kq->kq_references = PySet_New(0);
	if (kq->kq_references == NULL)
		return(-1);

	kq->kq_cancellations = PySet_New(0);
	if (kq->kq_cancellations == NULL)
		return(-2);

	kq->kq_root = kqueue();
	if (kq->kq_root == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-3);
	}

	if (kernelq_setup_interrupt(kq) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);

		/* Avoid the resource warning. */
		close(kq->kq_root);
		kq->kq_root = -1;
		return(-4);
	}

	if (kernelq_setup_signals(kq, NULL) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);

		/* Avoid the resource warning. */
		close(kq->kq_root);
		kq->kq_root = -1;
		return(-5);
	}

	return(0);
}

CONCEAL(void)
kernelq_clear(KernelQueue kq)
{
	Py_CLEAR(kq->kq_references);
	Py_CLEAR(kq->kq_cancellations);

	if (kq->kq_root != -1)
	{
		close(kq->kq_root);
		PyErr_WarnFormat(PyExc_ResourceWarning, 0,
			FACTOR_PATH("Events") " instance not closed before deallocation");
	}
}

CONCEAL(int)
kernelq_traverse(KernelQueue kq, PyObj self, visitproc visit, void *arg)
{
	Py_VISIT(kq->kq_references);
	Py_VISIT(kq->kq_cancellations);
	return(0);
}

/**
	// Close the event queue kernel resources.

	// [ Returns ]
	// Zero if already closed, one if resources were destroyed.
	// Negative one if `errno` was set.
*/
CONCEAL(int)
kernelq_close(KernelQueue kq)
{
	if (kq->kq_root < 0)
		return(0);

	if (close(kq->kq_root) < 0)
		return(-1);

	kq->kq_root = -1;
	return(1);
}

STATIC(int)
acquire_kernel_ref(KernelQueue kq, PyObj link)
{
	switch (PySet_Contains(kq->kq_references, link))
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

	return(PySet_Add(kq->kq_references, link));
}

/**
	// Begin listening for the process exit event.
*/
CONCEAL(PyObj)
kernelq_process_watch(KernelQueue kq, pid_t target, void *ref)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.filter = EVFILT_PROC,
		.fflags = NOTE_EXIT,
		.flags = EV_ADD|EV_RECEIPT|EV_CLEAR,
		.data = 0,
		.ident = target,
		.udata = ref,
	};

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, &kev, 1, &ts) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	else
	{
		if (check_kevent(&kev) < 0)
			return(NULL);
	}

	Py_RETURN_NONE;
}

CONCEAL(PyObj)
kernelq_process_ignore(KernelQueue kq, pid_t target, void *ref)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.filter = EVFILT_PROC,
		.fflags = NOTE_EXIT,
		.flags = EV_DELETE|EV_RECEIPT,
		.data = 0,
		.ident = target,
		.udata = ref,
	};

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, &kev, 1, &ts) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	else
	{
		if (check_kevent(&kev) < 0)
			return(NULL);
	}

	Py_RETURN_NONE;
}

CONCEAL(int)
kernelq_recur(KernelQueue kq, int unit, unsigned long quantity, PyObj ref)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.filter = EVFILT_TIMER,
		.flags = EV_ADD|EV_RECEIPT|EV_ENABLE,
		.ident = (uintptr_t) ref,
		.udata = ref,
		.fflags = -1,
		.data = quantity,
	};

	kev.fflags = note_unit(unit, &kev.data);
	if (kev.fflags < 0)
		return(-1);

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, &kev, 1, &ts) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-2);
	}
	else
	{
		if (check_kevent(&kev) < 0)
			return(-3);
	}

	if (PySet_Add(kq->kq_references, (PyObj) ref) < 0)
	{
		/* error */
	}

	return(0);
}

CONCEAL(int)
kernelq_defer(KernelQueue kq, int unit, unsigned long quantity, PyObj ref)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.filter = EVFILT_TIMER,
		.flags = EV_ADD|EV_RECEIPT|EV_ENABLE|EV_ONESHOT,
		.ident = (uintptr_t) ref,
		.udata = ref,
		.fflags = -1,
		.data = quantity,
	};

	kev.fflags = note_unit(unit, &kev.data);
	if (kev.fflags < 0)
		return(-1);

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, &kev, 1, &ts) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-2);
	}
	else
	{
		if (check_kevent(&kev) < 0)
			return(-3);
	}

	if (PySet_Add(kq->kq_references, ref) < 0)
	{
		/* error */
	}

	return(0);
}

CONCEAL(PyObj)
kernelq_cancel(KernelQueue kq, void *ref)
{
	const static struct timespec ts = {0,0};
	int nkevents = 0;
	kevent_t kev = {
		.ident = ref,
		.udata = 0,
		.filter = EVFILT_TIMER,
		.fflags = 0,
		.data = 0,
		.flags = EV_DELETE|EV_RECEIPT,
	};

	if (kernelq_kevent(kq, 1, &nkevents, &kev, 1, &kev, 1, &ts) < 0)
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
					// Validate not in reference set.
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
	if (PySet_Add(kq->kq_cancellations, link) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

/**
	// Receive events from the kernel.
*/
CONCEAL(int)
kernelq_enqueue(KernelQueue kq, long seconds, long ns)
{
	struct timespec waittime = {seconds, ns};
	PyObj rob;
	int error = 0;

	kq->kq_event_position = 0;
	kq->kq_event_count = 0;

	Py_BEGIN_ALLOW_THREADS
	{
		error = kernelq_kevent(kq, 0,
			&(kq->kq_event_count),
			NULL, 0,
			kq->kq_array, CONFIG_STATIC_KEVENTS,
			&waittime
		);
	}
	Py_END_ALLOW_THREADS

	if (error < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-1);
	}

	return(0);
}

/**
	// Transition the received kernel events.
*/
CONCEAL(PyObj)
kernelq_transition(KernelQueue kq)
{
	PyObj rob;
	int i, error = 0;
	PyObj refset = kq->kq_references;
	PyObj cancelset = kq->kq_cancellations;

	rob = PyList_New(0);
	if (rob == NULL)
		return(NULL);

	for (i = kq->kq_event_position; i < kq->kq_event_count; ++i)
	{
		PyObj link;
		kevent_t *kev;
		PyObj ob;

		kev = &(kq->kq_array[i]);
		link = (PyObj) kev->udata;

		switch (kev->filter)
		{
			case EVFILT_PROC:
				ob = Py_BuildValue("(slO)", "process", (long) kev->ident, link ? link : Py_None);
				if (kev->fflags & NOTE_EXIT && link != NULL)
				{
					/* done with filter entry */

					if (PySet_Discard(refset, link) < 0)
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

				switch (PySet_Contains(cancelset, link))
				{
					case 1:
					{
						if (PySet_Discard(refset, link) < 0)
						{
							error = 1;
						}

						if (PySet_Discard(cancelset, link) < 0)
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
					/* defer */
					ob = Py_BuildValue("(sO)", "alarm", link);

					if (PySet_Discard(refset, link) < 0)
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
					// Currently, only used to interrupt.
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

	kq->kq_event_position = i;

	/*
		// Complete timer cancellations.
	*/
	if (PySet_GET_SIZE(cancelset) > 0)
	{
		PyObj r = PyObject_CallMethod(refset, "difference_update", "O", cancelset);
		if (r != NULL)
			Py_DECREF(r);

		if (PySet_Clear(cancelset))
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
