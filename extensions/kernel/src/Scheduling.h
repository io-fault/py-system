/**
	// Scheduler compilation environment.

	// /Event/
		// A reference to a point in time.

		// Either an actual point in time, timer, or a reference to an observable
		// operation such as process exit, signal, or filesystem events.

		// Instances are merely identifiers for when.
		// Scheduled events are &Link instances.
	// /Scheduler/
		// The join of a kernel event queue and a task queue providing
		// access to the scheduling of &Event instances.
	// /KernelQueue/
		// The abstraction for epoll or kqueue.
	// /TaskQueue/
		// The queue implementation for enqueueing tasks to be executed.
	// /Link/
		// The join of a scheduled &Event and a task.
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

#include "kqueue.h"

/* Requires kernelq.h implementation (kqueue.h/epoll.h) */
#include "link.h"

#include "taskq.h"
#include "kernelq.h"
#include "scheduler.h"
