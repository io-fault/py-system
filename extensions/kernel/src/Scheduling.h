/**
	// Scheduler compilation environment.

	// /Event/
		// A reference to a point in time.

		// Either an actual point in time, timer, or a reference to an observable
		// operation such as process exit, signal, filesystem events, or I/O.

		// Instances are containers for when and what. When the operation should
		// be executed and what resource generated the event.
	// /Scheduler/
		// The join of a kernel event queue and a task queue providing
		// access to the scheduling of &Event instances.
	// /KernelQueue/
		// The abstraction for the underlying event system.
		// Currently, epoll or kqueue.
	// /TaskQueue/
		// The queue implementation for enqueueing tasks to be executed.
	// /Link/
		// The join of a scheduled &Event and a task.
		// When dispatched, represents a Scheduled Operation.
*/
#ifndef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
#endif

#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
#endif

#include <kcore.h>
#include <kports.h>

#include "event.h"
#define __EV_KQUEUE__(X) 0
#define __EV_EPOLL__(X) 0

#ifdef __linux__
	#undef __EV_EPOLL__
	#define __EV_EPOLL__(X) 1
	#include "epoll.h"
#else
	#undef __EV_KQUEUE__
	#define __EV_KQUEUE__(X) 1
	#include "kqueue.h"
#endif

/* Requires kernelq.h implementation (kqueue.h/epoll.h) */
#include "link.h"

#include "taskq.h"
#include "kernelq.h"
#include "scheduler.h"
