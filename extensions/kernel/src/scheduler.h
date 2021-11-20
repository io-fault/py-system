/**
	// KernelQueue and TaskQueue based scheduler.
*/
#ifndef _SYSTEM_KERNEL_SCHEDULER_H_included_
#define _SYSTEM_KERNEL_SCHEDULER_H_included_

#define Scheduler_GetTaskQueue(EV) (&(EV->ks_tq))
#define Scheduler_GetKernelQueue(EV) (&(EV->ks_eq))

typedef struct Scheduler *Scheduler;
struct Scheduler {
	PyObject_HEAD
	int ks_waiting;
	struct TaskQueue ks_tq;
	struct KernelQueue ks_eq;
};
#endif
