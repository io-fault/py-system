/**
	// Heterogeneous Kernel Events interface.
*/
#ifndef _SYSTEM_KERNEL_EVENTS_H_included_
#define _SYSTEM_KERNEL_EVENTS_H_included_
typedef struct kevent kevent_t; /* kernel event description */

#ifndef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
#elif CONFIG_STATIC_KEVENTS < 8
	#undef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
	#warning nope.
#endif

typedef int kport_t; /* file descriptor */
typedef int kerror_t; /* kerror error identifier (errno) */

typedef struct Tasks *Tasks;
struct Tasks {
	Tasks t_next;
	size_t t_allocated;

	PyObj t_queue[0];
};

struct Events {
	PyObject_HEAD

	/* storage for objects referenced by the kernel */
	PyObj ke_kset;

	/* cancel bucket */
	PyObj ke_cancellations;

	/*
		// Two separate queues. Executing gets drained, loading gets moved to executing; repeat.
		// &ke_tail is the pointer to the last linked list segment providing instant appends.
		// &ke_tailcursor is the append position in the tail segment.

		// When loading is rotated into executing at the end of &ki_execute_tasks,
		// &ke_tailcursor is reset after being used to identify the length of the segment
		// overwriting &ke_tail->t_allocated.

		// Worst case temporary memory waste is 129 pointers + sizeof(size_t),
		// a completely empty segment only to be freed on the next cycle.
	*/
	struct Tasks *ke_executing;
	struct Tasks *ke_loading;
	struct Tasks *ke_tail;
	int ke_tailcursor;

	/* kqueue(2) fd */
	kport_t ke_kqueue;
	int ke_waiting;

	kevent_t ke_events[CONFIG_STATIC_KEVENTS];
};

typedef struct Events *Events;

#define KE_LQUEUE_HAS_TASKS(I) (I->ke_tailcursor > 0 || I->ke_loading != I->ke_tail)
#define KE_XQUEUE_HAS_TASKS(I) (I->ke_executing != NULL && I->ke_executing->t_allocated > 0)
#define KE_HAS_TASKS(I) (KE_LQUEUE_HAS_TASKS(I) || KE_XQUEUE_HAS_TASKS(I))
#endif
