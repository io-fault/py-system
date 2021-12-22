/**
	// A linked list of vectors used to manage the queue's storage.
*/
typedef struct Tasks *Tasks;
struct Tasks {
	Tasks t_next;
	size_t t_allocated;

	PyObj t_vector[0];
};

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
typedef struct TaskQueue *TaskQueue;
struct TaskQueue {
	struct Tasks *q_executing;
	struct Tasks *q_loading;

	struct Tasks *q_tail;
	int q_tailcursor;
};

#define TQ_LQUEUE_HAS_TASKS(I) (I->q_tailcursor > 0 || I->q_loading != I->q_tail)
#define TQ_XQUEUE_HAS_TASKS(I) (I->q_executing != NULL && I->q_executing->t_allocated > 0)
#define TQ_HAS_TASKS(I) (TQ_LQUEUE_HAS_TASKS(I) || TQ_XQUEUE_HAS_TASKS(I))

int taskq_initialize(TaskQueue);
void taskq_clear(TaskQueue);
int taskq_traverse(TaskQueue, visitproc, void *);
int taskq_extend(TaskQueue);
int taskq_enqueue(TaskQueue, PyObj);
int taskq_execute(TaskQueue, PyObj, PyObj);
