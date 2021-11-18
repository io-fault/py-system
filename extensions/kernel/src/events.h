/**
	// Heterogeneous Kernel Events interface.
*/
#ifndef _SYSTEM_KERNEL_EVENTS_H_included_
#define _SYSTEM_KERNEL_EVENTS_H_included_

#ifndef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
#elif CONFIG_STATIC_KEVENTS < 8
	#undef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
	#warning nope.
#endif

typedef struct kevent kevent_t; /* kernel event description */
typedef int kport_t; /* file descriptor */
typedef int kerror_t; /* kerror error identifier (errno) */

#define Events_GetTaskQueue(EV) (&(EV->ke_taskq))
typedef struct Events *Events;
struct Events {
	PyObject_HEAD

	/* storage for objects referenced by the kernel */
	PyObj ke_kset;

	/* cancel bucket */
	PyObj ke_cancellations;

	struct TaskQueue ke_taskq;

	/* kqueue(2) fd */
	kport_t ke_kqueue;
	int ke_waiting;

	kevent_t ke_events[CONFIG_STATIC_KEVENTS];
};
#endif
