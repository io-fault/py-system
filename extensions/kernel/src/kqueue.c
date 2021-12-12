/**
	// kqueue based KernelQueue implementation.
	// Provides supporting kernel event functionality for &.kernel.Scheduler.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#ifndef INITIAL_TASKS_ALLOCATED
	#define INITIAL_TASKS_ALLOCATED 4
#endif

#include "Scheduling.h"

int _kq_reference_update(KernelQueue, Link, PyObj *);

STATIC(int)
kernelq_kevent_delta(KernelQueue kq, int retry, kevent_t *event)
{
	struct timespec ts = {0,0};
	RETRY_STATE_INIT;
	int r = -1;

	/* Force receipt. */
	event->flags |= EV_RECEIPT;

	RETRY_SYSCALL:
	r = kevent(kq->kq_root, event, 1, event, 1, &ts);
	if (r < 0)
	{
		switch (errno)
		{
			case EINTR:
			{
				switch (retry)
				{
					case -1:
						UNLIMITED_RETRY();
					break;

					case 1:
						LIMITED_RETRY();
					break;

					case 0:
						PyErr_SetFromErrno(PyExc_OSError);
						return(-1);
					break;
				}
			}

			case EBADF:
				/**
					// Force -1 to avoid the possibility of acting on a kqueue
					// that was allocated by another part of the process after
					// an unexpected close occurred on this &kq->kq_root.
				*/
				kq->kq_root = -1;
			case ENOMEM:
			default:
			{
				PyErr_SetFromErrno(PyExc_OSError);
				return(-2);
			}
			break;
		}
	}
	else if (r != 1)
	{
		if (PyErr_WarnFormat(PyExc_RuntimeWarning, 0,
			"kevent processed unexpected number of events: %d", r) < 0)
			return(-9);
	}

	/* Check for filter error. */
	switch (KFILTER_ERROR(event))
	{
		case 0: break;

		case ENOENT:
			if (event->flags & EV_DELETE)
			{
				/* Don't raise already deleted errors. */
				return(0);
			}
			/* Intentionally pass through if not EV_DELETE */
		default:
		{
			errno = event->data;
			PyErr_SetFromErrno(PyExc_OSError);
			return(-3);
		}
		break;
	}

	return(0);
}

/**
	// Interpret event_t for use with a kqueue filter.
	// Duplicate file descriptors or open files as needed.
*/
CONCEAL(int)
kevent_identify(kevent_t *kev, event_t *evs)
{
	switch (evs->evs_type)
	{
		case EV_TYPE_ID(meta_actuate):
		{
			EV_USER_SETUP(kev);
			kev->ident = (uintptr_t) evs;
			kev->flags |= EV_ONESHOT;
			EV_USER_TRIGGER(kev);
		}
		break;

		case EV_TYPE_ID(never):
		case EV_TYPE_ID(meta_terminate):
		{
			EV_USER_SETUP(kev);
			kev->ident = (uintptr_t) evs;
			kev->flags |= EV_ONESHOT;
		}
		break;

		case EV_TYPE_ID(process_exit):
		{
			kev->fflags = NOTE_EXIT;
			kev->flags |= EV_ONESHOT;

			switch (evs->evs_resource_t)
			{
				case evr_kport:
					kev->filter = EVFILT_PROCDESC;
					kev->ident = evs->evs_resource.procref.procfd;
				break;

				case evr_identifier:
					kev->filter = EVFILT_PROC;
					kev->ident = evs->evs_resource.process;
				break;

				default:
					return(-1);
				break;
			}
		}
		break;

		case EV_TYPE_ID(process_signal):
		{
			kev->filter = EVFILT_SIGNAL;
			kev->ident = evs->evs_resource.signal_code;
		}
		break;

		case EV_TYPE_ID(time):
		{
			kev->filter = EVFILT_TIMER;
			kev->ident = (uintptr_t) evs;
			kev->fflags = NOTE_MSECONDS;
		}
		break;

		default:
		{
			/*
				// Common file descriptor case.
			*/
			kev->ident = evs->evs_resource.io[0];

			switch (evs->evs_type)
			{
				/* Level triggered I/O */
				case EV_TYPE_ID(io_transmit):
				{
					kev->filter = EVFILT_WRITE;
				}
				break;

				case EV_TYPE_ID(io_status):
				case EV_TYPE_ID(io_receive):
				{
					kev->filter = EVFILT_READ;
				}
				break;

				case EV_TYPE_ID(fs_status):
				{
					kev->filter = EVFILT_VNODE;
					kev->fflags = EVENT_FS_STATUS_FLAGS;
				}
				break;

				case EV_TYPE_ID(fs_delta):
				{
					kev->filter = EVFILT_VNODE;
					kev->fflags = EVENT_FS_DELTA_FLAGS;
				}
				break;

				case EV_TYPE_ID(fs_void):
				{
					kev->filter = EVFILT_VNODE;
					kev->fflags = EVENT_FS_VOID_FLAGS;
					kev->flags |= EV_ONESHOT;
				}
				break;

				default:
					return(-2);
				break;
			}
		}
	}

	return(0);
}

/**
	// Called by &.kernel.Scheduler.interrupt.
*/
CONCEAL(int)
kernelq_interrupt(KernelQueue kq)
{
	kevent_t kev = {
		.udata = (void *) NULL,
		.ident = (uintptr_t) kq,
	};
	EV_USER_TRIGGER(&kev);
	return(kernelq_kevent_delta(kq, 1, &kev));
}

STATIC(int)
kernelq_interrupt_setup(KernelQueue kq)
{
	kevent_t kev = {
		.udata = (void *) NULL,
		.ident = (uintptr_t) kq,
	};
	EV_USER_SETUP(&kev);
	return(kernelq_kevent_delta(kq, 1, &kev));
}

STATIC(void) inline
kevent_set_timer(kevent_t *kev, unsigned long long ns)
{
	#if SIZEOF_VOID_P <= 32
		/*
			// ! WARNING: SIZEOF_VOID_P != sizeof(kev->data)

			// Prioritize milliseconds when data field is *likely* 32-bit.
			// With a 64-bit field, there is substantial room for the near future,
			// but 32-bits of nanoseconds gives us about 4.2 seconds.
		*/
		uint32_t s = 0;

		if (ns > (0xFFFFFFFFULL * 1000000))
		{
			/* Exceeds what 32-bit milliseconds can refer to. */

			kev->fflags = NOTE_SECONDS;
			kev->data = ns / 1000000000;
		}
		else
		{
			#if defined(NOTE_MSECONDS)
				kev->fflags = NOTE_MSECONDS;
				kev->data = ns / 1000000;
			#elif defined(NOTE_USECONDS)
				kev->fflags = NOTE_USECONDS;
				kev->data = ns / 1000000000;
			#elif defined(NOTE_NSECONDS)
				kev->fflags = NOTE_NSECONDS;
				kev->data = ns;
			#endif
		}
	#else
		/*
			// Simple 64-bit case as &.kernel.Event is configured to
			// hold 64-bits of nanoseconds. Prioritize nanoseconds and convert
			// to more course units when not available.
		*/
		#if defined(NOTE_NSECONDS)
			kev->fflags = NOTE_NSECONDS;
			kev->data = ns;
		#elif defined(NOTE_USECONDS)
			kev->fflags = NOTE_USECONDS;
			kev->data = ns / 1000;
		#elif defined(NOTE_MSECONDS)
			kev->fflags = NOTE_MSECONDS;
			kev->data = ns / 1000000;
		#else
			kev->fflags = NOTE_SECONDS;
			kev->data = ns / 1000000000;
		#endif
	#endif
}

/**
	// Establish the link with the kernel event.
*/
CONCEAL(int)
kernelq_schedule(KernelQueue kq, Link ln, int cyclic)
{
	kevent_t kev = {
		.flags = EV_ADD|EV_RECEIPT,
		.fflags = 0,
		.udata = ln,
	};
	Event ev = ln->ln_event;
	PyObj current = NULL;

	if (kevent_identify(&kev, Event_Specification(ln->ln_event)) < 0)
	{
		/* Unrecognized EV_TYPE */
		PyErr_SetString(PyExc_ValueError, "unrecognized event type");
		return(-1);
	}

	/**
		// Configure cyclic flag.
	*/
	switch (cyclic)
	{
		case -1:
		{
			/* Inherit from &kevent_identify. */
			if (kev.flags & EV_ONESHOT)
				Link_Clear(ln, cyclic);
			else
				Link_Set(ln, cyclic);
		}
		break;

		case 0:
		{
			/* All filters support one shot. */
			Link_Clear(ln, cyclic);
			kev.flags |= EV_ONESHOT;
		}
		break;

		case 1:
		{
			Link_Set(ln, cyclic);

			if (kev.flags & EV_ONESHOT)
			{
				/* kevent_identify designates this restriction via EV_ONESHOT. */
				PyErr_SetString(PyExc_ValueError, "cyclic behavior not supported on event");
				return(-1);
			}
		}
		break;
	}

	switch (kev.filter)
	{
		case EVFILT_TIMER:
		{
			kevent_set_timer(&kev, ev->ev_spec.evs_resource.time);

			switch (ev->ev_spec.evs_type)
			{
				case EV_TYPE_ID(never):
					kev.flags |= EV_DISABLE;
				break;

				default:
					;
				break;
			}
		}
		break;

		case EVFILT_VNODE:
			kev.flags |= EV_CLEAR;
		break;

		default:
			kev.data = 0;
		break;
	}

	/*
		// Update prior to kevent to make clean up easier on failure.
	*/
	if (_kq_reference_update(kq, ln, &current) < 0)
		return(-1);

	if (kernelq_kevent_delta(kq, 1, &kev) < 0)
	{
		if (current != NULL && current != ln)
		{
			PyObj old;

			if (_kq_reference_update(kq, ln, &old) < 0)
				PyErr_WriteUnraisable(ln);
		}

		return(-2);
	}

	/*
		// Release old record (current), if any.
		// Scheduler is now holding the reference to &record via kq_references.
	*/
	Py_XDECREF(current);
	Link_Set(ln, dispatched);
	return(0);
}

CONCEAL(PyObj)
kernelq_cancel(KernelQueue kq, Link ln)
{
	kevent_t kev = {
		.flags = EV_DELETE|EV_RECEIPT,
		.data = 0,
		.udata = 0,
	};
	PyObj original;

	if (kevent_identify(&kev, Event_Specification(ln->ln_event)) < 0)
	{
		/* Unrecognized EV_TYPE */
		PyErr_SetString(PyExc_TypeError, "unrecognized event type");
		return(NULL);
	}

	/*
		// Prepare for cancellation by adding the existing
		// scheduled &ln to the cancellation list. This
		// allows any concurrently collected event to be
		// safely enqueued using &ln.
	*/

	original = PyDict_GetItem(kq->kq_references, ln->ln_event); /* borrowed */
	if (original == NULL)
	{
		/* No event in table. No cancellation necessary. */
		if (PyErr_Occurred())
			return(NULL);
		else
			Py_RETURN_NONE;
	}

	/*
		// Maintain reference count in case of concurrent use(collected event).
	*/
	if (PyList_Append(kq->kq_cancellations, original) < 0)
		return(NULL);

	/* Delete link from references */
	if (PyDict_DelItem(kq->kq_references, ln->ln_event) < 0)
	{
		/* Nothing has actually changed here, so just return the error. */
		/* Cancellation list will be cleared by &.kernel.Scheduler.wait */
		return(NULL);
	}

	if (kernelq_kevent_delta(kq, 1, &kev) < 0)
	{
		Link prior = (Link) original;

		/*
			// Cancellation failed.
			// Likely a critical error, but try to recover.
		*/
		if (PyDict_SetItem(kq->kq_references, prior->ln_event, original) < 0)
		{
			/*
				// Could not restore the reference position.
				// Prefer leaking the link over risking a SIGSEGV
				// after the kq_cancellations list has been cleared.
			*/
			Py_INCREF(original);
			PyErr_WarnFormat(PyExc_RuntimeWarning, 0,
				"event link leaked due to cancellation failure");
		}

		return(NULL);
	}

	Py_RETURN_NONE;
}

CONCEAL(int)
kernelq_initialize(KernelQueue kq)
{
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t kev;

	kq->kq_references = PyDict_New();
	if (kq->kq_references == NULL)
		return(-1);

	kq->kq_cancellations = PyList_New(0);
	if (kq->kq_cancellations == NULL)
		return(-2);

	kq->kq_root = kqueue();
	if (kq->kq_root == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-3);
	}

	if (kernelq_interrupt_setup(kq) < 0)
	{
		close(kq->kq_root);
		kq->kq_root = -1;
		return(-4);
	}

	return(0);
}

CONCEAL(void)
kernelq_clear(KernelQueue kq)
{
	Py_CLEAR(kq->kq_references);
	Py_CLEAR(kq->kq_cancellations);
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

/**
	// Receive events from the kernel.
	// Retry logic is not desired here as the event loop will naturally try again.
*/
CONCEAL(int)
kernelq_receive(KernelQueue kq, long seconds, long ns)
{
	const struct timespec waittime = {seconds, ns};
	int r = 0;

	kq->kq_event_position = 0;
	kq->kq_event_count = 0;

	Py_BEGIN_ALLOW_THREADS
	{
		int nevents = CONFIG_STATIC_KEVENTS - kq->kq_event_position;
		kevent_t *kq_offset = &(kq->kq_array[kq->kq_event_position]);

		r = kevent(kq->kq_root, NULL, 0, kq_offset, nevents, &waittime);
	}
	Py_END_ALLOW_THREADS

	if (r < 0)
	{
		switch (errno)
		{
			case EINTR:
				errno = 0;
			break;

			case EBADF:
				/**
					// Force -1 to avoid the possibility of acting on a kqueue
					// that was allocated by another part of the process after
					// an unexpected close occurred on this &kq.kq_root.
				*/
				kq->kq_root = -1;
			default:
				PyErr_SetFromErrno(PyExc_OSError);
				return(-1);
			break;
		}
	}
	else
		kq->kq_event_count = r;

	return(0);
}

STATIC(void)
pkevent(kevent_t *kev)
{
	const char *fname;

	switch (kev->filter)
	{
		#define KFILTER(B, TYP) case B: fname = #B; break;
			KQ_FILTER_LIST();
		#undef KFILTER

		default:
			fname = "unknown";
		break;
	}

	fprintf(stderr,
		"%s (%d), fflags: %d,"
		" ident: %p, data: %p, udata: %p, flags:"
		" "
		#define FLAG(FLG) "%s"
			KQ_FLAG_LIST()
		#undef FLAG
		"\n",
		fname, kev->filter, kev->fflags,
		(void *) kev->ident,
		(void *) kev->data,
		(void *) kev->udata,
		#define FLAG(FLG) (kev->flags & FLG) ? (#FLG "|") : "",
			KQ_FLAG_LIST()
		#undef FLAG
		""
	);

	return;
}

/**
	// Transition the received kernel events to enqueued tasks.

	// [ Returns ]
	// The count of processed events or negative value on error.
*/
CONCEAL(int)
kernelq_transition(KernelQueue kq, TaskQueue tq)
{
	PyObj refset = kq->kq_references;
	PyObj cancelset = kq->kq_cancellations;

	for (; kq->kq_event_position < kq->kq_event_count; ++(kq->kq_event_position))
	{
		PyObj task; /* tuple attached to event data */
		Link ln;
		kevent_t *kev;

		kev = &(kq->kq_array[kq->kq_event_position]);
		ln = (Link) kev->udata;

		switch (kev->filter)
		{
			case EVFILT_SIGNAL:
			{
				int signo = kev->ident;
			}
			break;

			case EVFILT_PROC:
			case EVFILT_PROCDESC:
			{
				assert(kev->fflags & NOTE_EXIT);
			}
			break;

			case EVFILT_VNODE:
			{
				kport_t fd = kev->ident;
			}
			break;

			case EVFILT_TIMER:
			{
				uintptr_t id = kev->ident;
			}
			break;

			case EVFILT_USER:
			{
				/* &.kernel.Scheduler.interrupt, nothing to do. */
				if (ln == NULL)
					continue;
			}
			break;

			case EVFILT_WRITE:
			case EVFILT_READ:
			{
				kport_t fd = kev->ident;
			}
			break;

			default:
			{
				/*
					// unknown event, throw warning
				*/
				PyErr_WarnFormat(PyExc_Warning, 0, "unrecognized event (%d) received", kev->filter);
				continue;
			}
			break;
		}

		assert(Py_TYPE(ln) == &LinkType);

		if (taskq_enqueue(tq, ln) < 0)
			return(-1);

		if (!Link_Get(ln, cyclic))
		{
			if (kev->flags & EV_ONESHOT)
				Link_Set(ln, cancelled);
			else
			{
				kev->flags = EV_DELETE|EV_RECEIPT;

				if (kernelq_kevent_delta(kq, 1, kev) < 0)
				{
					PyErr_WriteUnraisable(ln);
					PyErr_Clear();
				}
				else
					Link_Set(ln, cancelled);
			}

			if (PyDict_DelItem(refset, ln->ln_event) < 0)
			{
				PyErr_WriteUnraisable(ln);
				PyErr_Clear();
			}
		}
	}

	/*
		// Clear cancellation references.
	*/
	if (PyList_GET_SIZE(cancelset) > 0)
	{
		PyObj r = PyObject_CallMethod(cancelset, "clear", "", NULL);
		if (r == NULL)
		{
			PyErr_WriteUnraisable(NULL);
			PyErr_Clear();
		}
		else
			Py_DECREF(r);
	}

	return(0);
}
