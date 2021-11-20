/**
	// Heterogeneous Kernel Events interface.
*/
#ifndef _SYSTEM_KERNEL_EVENTS_H_included_
#define _SYSTEM_KERNEL_EVENTS_H_included_

#define Events_GetTaskQueue(EV) (&(EV->ke_tq))
#define Events_GetKernelQueue(EV) (&(EV->ke_eq))

typedef struct Events *Events;
struct Events {
	PyObject_HEAD
	int ke_waiting;
	struct TaskQueue ke_tq;
	struct KernelQueue ke_eq;
};
#endif
