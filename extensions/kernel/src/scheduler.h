/**
	// KernelQueue and TaskQueue based scheduler.
*/
#ifndef _SYSTEM_KERNEL_SCHEDULER_H_included_
#define _SYSTEM_KERNEL_SCHEDULER_H_included_

#define Scheduler_GetTaskQueue(EV) (&(EV->ks_tq))
#define Scheduler_GetKernelQueue(EV) (&(EV->ks_eq))

#define Scheduler_GetExceptionTrap(EV) ((EV->ks_exc))
#define Scheduler_SetExceptionTrap(EV, OB) (Scheduler_GetExceptionTrap(EV) = OB)
#define Scheduler_UpdateExceptionTrap(EV, OB) do { \
	Py_XDECREF(Scheduler_GetExceptionTrap(EV)); \
	Scheduler_SetExceptionTrap(EV, OB); \
	Py_XINCREF(OB); \
} while(0)

typedef struct Scheduler *Scheduler;
struct Scheduler {
	PyObject_HEAD
	PyObj weakreflist;

	int ks_waiting;
	PyObj ks_exc;
	struct TaskQueue ks_tq;
	struct KernelQueue ks_eq;
};
#endif
