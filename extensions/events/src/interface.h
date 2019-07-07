/**
	// Heterogeneous Kernel Events interface.
*/

#ifndef INITIAL_TASKS_ALLOCATED
	#define INITIAL_TASKS_ALLOCATED 4
#endif

#ifndef MAX_TASKS_PER_SEGMENT
	#define MAX_TASKS_PER_SEGMENT 128
#endif

typedef int kpoint_t; /* file descriptor */
typedef int kerror_t; /* kerror error identifier (errno) */

typedef struct Tasks *Tasks;
struct Tasks {
	Tasks t_next;
	size_t t_allocated;

	PyObj t_queue[0];
};

struct Interface {
	PyObject_HEAD

	/* storage for objects referenced by the kernel */
	PyObj kif_kset;

	/* cancel bucket */
	PyObj kif_cancellations;

	/*
		// Two separate queues. Executing gets drained, loading gets moved to executing; repeat.
		// &kif_tail is the pointer to the last linked list segment providing instant appends.
		// &kif_tailcursor is the append position in the tail segment.

		// When loading is rotated into executing at the end of &ki_execute_tasks,
		// &kif_tailcursor is reset after being used to identify the length of the segment
		// overwriting &kif_tail->t_allocated.

		// Worst case temporary memory waste is 129 pointers + sizeof(size_t),
		// a completely empty segment only to be freed on the next cycle.
	*/
	struct Tasks *kif_executing;
	struct Tasks *kif_loading;
	struct Tasks *kif_tail;
	int kif_tailcursor;

	/* kqueue(2) fd */
	kpoint_t kif_kqueue;
	int kif_waiting;

	kevent_t kif_events[8];
};

typedef struct Interface *Interface;

#define KI_LQUEUE_HAS_TASKS(I) (I->kif_tailcursor > 0 || I->kif_loading != I->kif_tail)
#define KI_XQUEUE_HAS_TASKS(I) (I->kif_executing != NULL && I->kif_executing->t_allocated > 0)
