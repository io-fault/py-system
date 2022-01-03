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

/**
	// The source file is unconditionally compiled.
	// Guard against compiling when not targeting kqueue-1.
*/
#if __EV_KQUEUE__(1)

#ifndef O_EVTONLY
	/* macos uses O_EVTONLY so just abstract on it for BSD's */
	#define O_EVTONLY O_RDONLY
#endif

/**
	// ev_type is ignored here as the subevent set is determined by kernelq_identify.
*/
CONCEAL(kport_t)
fs_event_open(const char *path, enum EventType ev_type)
{
	kport_t fd = -1;

	fd = open(path, O_EVTONLY);
	if (fd < 0)
		PyErr_SetFromErrno(PyExc_OSError);

	return(fd);
}

CONCEAL(int)
kernelq_delta(KernelQueue kq, int ctl, kport_t kp, kevent_t *event)
{
	struct timespec ts = {0,0};
	RETRY_STATE_INIT;
	int r = -1;

	/* Force receipt. */
	event->flags |= ctl|EV_RECEIPT;

	RETRY_SYSCALL:
	r = kevent(kq->kq_root, event, 1, event, 1, &ts);
	if (r < 0)
	{
		switch (errno)
		{
			case EBADF:
				/**
					// Force -1 to avoid the possibility of acting on a kqueue
					// that was allocated by another part of the process after
					// an unexpected close occurred on this &kq->kq_root.
				*/
				kq->kq_root = -1;

			case EINTR:
				LIMITED_RETRY();
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
*/
CONCEAL(int)
kernelq_identify(kevent_t *kev, event_t *evs)
{
	enum EventType evt = evs->evs_type;
	kport_t kre = evs->evs_kresource;

	switch (evt)
	{
		case EV_TYPE_ID(meta_actuate):
		{
			EV_USER_SETUP(kev);
			kev->ident = (uintptr_t) evs;
			EV_USER_TRIGGER(kev);
			AEV_CYCLIC_DISABLE(kev);
		}
		break;

		case EV_TYPE_ID(never):
		case EV_TYPE_ID(meta_terminate):
		{
			EV_USER_SETUP(kev);
			kev->ident = (uintptr_t) evs;
			AEV_CYCLIC_DISABLE(kev);
		}
		break;

		case EV_TYPE_ID(process_exit):
		{
			kev->fflags = NOTE_EXIT;
			AEV_CYCLIC_DISABLE(kev);

			if (evs->evs_kresource != -1)
			{
				kev->filter = EVFILT_PROCDESC;
				kev->ident = kre;
			}
			else
			{
				kev->filter = EVFILT_PROC;
				kev->ident = evs->evs_field.process;
			}
		}
		break;

		case EV_TYPE_ID(process_signal):
		{
			kev->filter = EVFILT_SIGNAL;
			kev->ident = evs->evs_field.signal_code;
			AEV_CYCLIC_ENABLE(kev);
		}
		break;

		case EV_TYPE_ID(time):
		{
			kev->filter = EVFILT_TIMER;
			kev->ident = (uintptr_t) evs;
			kev->fflags = NOTE_MSECONDS;
			AEV_CYCLIC_ENABLE(kev);
		}
		break;

		default:
		{
			/*
				// Common file descriptor case.
			*/
			kev->ident = kre;
			AEV_CYCLIC_ENABLE(kev);

			switch (evt)
			{
				case EV_TYPE_ID(io_transmit):
				{
					kev->filter = EVFILT_WRITE;
					kev->flags |= EV_CLEAR;
				}
				break;

				case EV_TYPE_ID(io_status):
				case EV_TYPE_ID(io_receive):
				{
					kev->filter = EVFILT_READ;
					kev->flags &= ~EV_CLEAR;
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
					AEV_CYCLIC_DISABLE(kev);
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
	return(kernelq_delta(kq, 0, -1, &kev));
}

/**
	// Nothing to do for kqueue.
*/
CONCEAL(int)
kernelq_interrupt_accept(KernelQueue kq)
{
	return(0);
}

STATIC(int)
kernelq_interrupt_setup(KernelQueue kq)
{
	kevent_t kev = {
		.udata = (void *) NULL,
		.ident = (uintptr_t) kq,
	};
	EV_USER_SETUP(&kev);
	return(kernelq_delta(kq, AEV_CREATE, -1, &kev));
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
kernelq_schedule(KernelQueue kq, int cyclic, Link ln)
{
	PyObj current = NULL;
	kevent_t kev = {
		.flags = EV_ADD|EV_RECEIPT,
		.fflags = 0,
		.udata = ln,
	};
	Event ev = ln->ln_event;

	if (kernelq_identify(&kev, Event_Specification(ev)) < 0)
	{
		/* Unrecognized EV_TYPE */
		PyErr_SetString(PyExc_ValueError, "unrecognized event type");
		return(-1);
	}

	if (kernelq_cyclic_event(kq, cyclic, ln, &kev) < 0)
		return(-2);

	switch (kev.filter)
	{
		case EVFILT_TIMER:
		{
			kevent_set_timer(&kev, Event_GetTime(ev));

			switch (Event_Type(ev))
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

		case EVFILT_WRITE:
		case EVFILT_VNODE:
			kev.flags |= EV_CLEAR;
		break;

		default:
			kev.data = 0;
		break;
	}

	/*
		// Update prior to kevent to make clean up easier on failure.
		// If no error and current is not NULL, the reference is held by
		// by kq_cancellations.
	*/
	if (kernelq_reference_update(kq, ln, &current) < 0)
		return(-1);

	if (kernelq_delta(kq, AEV_CREATE, -1, &kev) < 0)
	{
		if (current != NULL && current != ln)
		{
			PyObj old = NULL;

			if (kernelq_reference_update(kq, ln, &old) < 0)
				PyErr_WriteUnraisable(ln);
		}

		return(-2);
	}

	/*
		// Release old record (current), if any.
		// Scheduler is now holding the reference to &record via kq_references.
	*/
	Link_Set(ln, dispatched);
	return(0);
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
	int r = -1;

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
				/* Concurrent close. */
				kq->kq_root = -1;
				kq->kq_event_count = 0;
				errno = 0;
				return(0);
			break;

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
#endif /* kqueue exclusive */
