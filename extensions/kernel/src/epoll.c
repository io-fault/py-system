/**
	// epoll based KernelQueue implementation.
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
#include <fault/python/injection.h>

#include "Scheduling.h"

/**
	// The source file is unconditionally compiled.
	// Guard against compiling when not targeting epoll-1.
*/
#if __EV_EPOLL__(1)

CONCEAL(int)
pidfd_open(pid_t pid, unsigned int flags)
{
	#ifndef __NR_pidfd_open
		#define __NR_pidfd_open 434
	#endif

	return syscall(__NR_pidfd_open, pid, flags);
	#undef __NR_pidfd_open
}

STATIC(uint32_t)
fs_inotify_events(enum EventType ev_type)
{
	uint32_t mask = 0;

	switch (ev_type)
	{
		case EV_TYPE_ID(fs_void):
			mask = IN_DELETE_SELF|IN_MOVE_SELF;
		break;

		case EV_TYPE_ID(fs_delta):
			mask = IN_MODIFY|IN_DELETE|IN_CREATE|IN_MOVED_FROM|IN_MOVED_TO;
		break;

		case EV_TYPE_ID(fs_status):
			mask = IN_ATTRIB;
			mask |= IN_DELETE_SELF|IN_MOVE_SELF;
			mask |= IN_MODIFY|IN_DELETE|IN_CREATE|IN_MOVED_FROM|IN_MOVED_TO;
		break;
	}

	return(mask);
}

CONCEAL(kport_t)
fs_event_open(const char *path, enum EventType ev_type)
{
	kport_t fd;

	fd = inotify_init1(IN_CLOEXEC);
	if (fd == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-1);
	}

	inotify_add_watch(fd, path, fs_inotify_events(ev_type));
	return(fd);
}

/**
	// Read a single inotify_event from &kp and return the mask field.
*/
CONCEAL(uint32_t)
fs_read_inotify_event(kport_t kp)
{
	char buf[64];
	struct inotify_event in_ev = {0,};
	int r, idx = 0;

	while (idx < sizeof(struct inotify_event))
	{
		r = read(kp, (&in_ev) + idx, sizeof(struct inotify_event) - idx);

		if (r < 0)
		{
			close(kp);
			return(0);
		}

		idx += r;
	}

	/* Align to edge for reading next event. */
	idx = 0;
	while (idx < in_ev.len)
	{
		r = read(kp, buf, 64);

		if (r < 0)
		{
			close(kp);
			return(0);
		}

		idx += r;
	}

	return(in_ev.mask);
}

CONCEAL(int)
kernelq_identify(kevent_t *kev, event_t *evs)
{
	kev->events |= EPOLL_FLAGS | EPOLLIN;

	switch (evs->evs_type)
	{
		case EV_TYPE_ID(never):
		case EV_TYPE_ID(meta_actuate):
		case EV_TYPE_ID(meta_terminate):
		{
			/* eventfd */
			AEV_CYCLIC_DISABLE(kev);
		}
		break;

		case EV_TYPE_ID(process_exit):
		{
			AEV_CYCLIC_DISABLE(kev);
		}
		break;

		case EV_TYPE_ID(process_signal):
		{
			AEV_CYCLIC_ENABLE(kev);
		}
		break;

		case EV_TYPE_ID(time):
		{
			/* timerfd */
			AEV_CYCLIC_ENABLE(kev);
		}
		break;

		default:
		{
			AEV_CYCLIC_ENABLE(kev);

			switch (evs->evs_type)
			{
				case EV_TYPE_ID(io_transmit):
				{
					kev->events |= EPOLLOUT | EPOLLET;
				}
				break;

				case EV_TYPE_ID(io_status):
				case EV_TYPE_ID(io_receive):
				{
					/* Defaults meet requirements. */;
				}
				break;

				case EV_TYPE_ID(fs_void):
					AEV_CYCLIC_DISABLE(kev);

				case EV_TYPE_ID(fs_status):
				case EV_TYPE_ID(fs_delta):
				{
					/* Defaults meet requirements. */;
				}
				break;

				default:
					/* Unrecognized event type. */
					return(-3);
				break;
			}
		}
	}

	return(0);
}

/**
	// Create an epoll file descriptor.
*/
STATIC(kport_t)
epoll_alloc(void)
{
	RETRY_STATE_INIT;
	int fd = -1;

	RETRY_SYSCALL:
	{
		ERRNO_RECEPTACLE(-1, &fd, epoll_create1, EPOLL_CLOEXEC);
	}

	if (fd < 0)
	{
		switch (errno)
		{
			case EINTR:
				LIMITED_RETRY()
				return(-2);
			default:
				return(-1);
			break;
		}
	}

	return((kport_t) fd);
}

CONCEAL(int)
kernelq_delta(KernelQueue kq, int ctl, kport_t kp, kevent_t *kev)
{
	RETRY_STATE_INIT;
	int r = -1;
	Link ln = kev->data.ptr;

	RETRY_SYSCALL:
	{
		ERRNO_RECEPTACLE(-1, &r, epoll_ctl, kq->kq_root, ctl, kp, kev);
	}

	if (r != 0)
	{
		switch (errno)
		{
			case EINTR:
				LIMITED_RETRY()
				return(-1);
			break;

			default:
				return(-2);
			break;
		}
	}

	return(0);
}

/**
	// Receive events from the kernel using epoll_wait.
	// Retry logic is not desired here as the event loop will naturally try again.
*/
CONCEAL(int)
kernelq_receive(KernelQueue kq, long seconds, long ns)
{
	int r = -1;
	int timeout;

	if (seconds != -1)
	{
		timeout = seconds * 1000;
		timeout += (ns / 1000000);
	}
	else
		timeout = -1;

	Py_BEGIN_ALLOW_THREADS
	{
		int nevents = CONFIG_STATIC_KEVENTS - kq->kq_event_position;
		kevent_t *kq_offset = &(kq->kq_array[kq->kq_event_position]);

		ERRNO_RECEPTACLE(-1, &r, epoll_wait, kq->kq_root, kq_offset, nevents, timeout);
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
				close(kq->kq_eventfd_interrupt);
				kq->kq_eventfd_interrupt = -1;
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

CONCEAL(int)
kernelq_schedule(KernelQueue kq, int cyclic, Link ln)
{
	PyObj current = NULL;
	Event ev = ln->ln_event;
	kport_t kp = Event_GetKPort(ev);
	kevent_t kev = {
		.events = 0,
		.data = {
			.ptr = ln,
		},
	};

	if (kernelq_identify(&kev, Event_Specification(ln->ln_event)) < 0)
	{
		PyErr_SetString(PyExc_TypeError, "could not recognize event file descriptor");
		return(-1);
	}

	if (kernelq_cyclic_event(kq, cyclic, ln, &kev) < 0)
		return(-2);

	if (kernelq_reference_update(kq, ln, &current) < 0)
		return(-3);

	if (kernelq_delta(kq, AEV_CREATE, kp, &kev) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);

		if (current != NULL && current != ln)
		{
			PyObj old;

			if (kernelq_reference_update(kq, ln, &old) < 0)
				PyErr_WriteUnraisable(ln);
		}

		return(-4);
	}

	Link_Set(ln, dispatched);
	return(0);
}

CONCEAL(int)
kernelq_interrupt(KernelQueue kq)
{
	uint64_t sig = 1;
	return((int) write(kq->kq_eventfd_interrupt, &sig, sizeof(sig)));
}

CONCEAL(int)
kernelq_interrupt_accept(KernelQueue kq)
{
	uint64_t sig = 0;
	read(kq->kq_eventfd_interrupt, &sig, sizeof(sig));
	return(0);
}

STATIC(int)
kernelq_interrupt_setup(KernelQueue kq)
{
	kevent_t kev = {
		.events = EPOLLIN,
		.data = {
			.ptr = NULL,
		},
	};
	kport_t kp;

	kp = eventfd(0, EFD_CLOEXEC);
	if (kp < 0)
		return(-1);

	kq->kq_eventfd_interrupt = kp;
	kernelq_delta(kq, AEV_CREATE, kp, &kev);
	return(0);
}

CONCEAL(int)
kernelq_initialize(KernelQueue kq)
{
	kq->kq_references = PyDict_New();
	if (kq->kq_references == NULL)
		return(-1);

	kq->kq_cancellations = PyList_New(0);
	if (kq->kq_cancellations == NULL)
		return(-2);

	kq->kq_root = epoll_alloc();
	if (kq->kq_root == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-3);
	}

	if (kernelq_interrupt_setup(kq) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);

		close(kq->kq_root);
		kq->kq_root = -1;

		return(-4);
	}

	return(0);
}

CONCEAL(int)
kernelq_close(KernelQueue kq)
{
	if (kq->kq_root < 0)
		return(0);

	close(kq->kq_eventfd_interrupt);
	errno = 0;

	if (close(kq->kq_root) < 0)
		return(-1);

	/* Resources destroyed */
	kq->kq_root = -1;
	kq->kq_eventfd_interrupt = -1;
	return(1);
}
#endif /* epoll exclusive */
