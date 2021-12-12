/**
	// Kernel event scheduling and Python task queue for main loop management.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "Scheduling.h"

STATIC(int) inline
interrupt_wait(Scheduler ks)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	/* Not waiting */
	switch (ks->ks_waiting)
	{
		case 0: /* Not waiting */
		case 2: /* Initial state */
			return(0);
		break;
	}

	/**
		// Ignore interrupt if it's not waiting or has already been interrupted.
	*/
	if (ks->ks_waiting > 0)
	{
		if (kernelq_interrupt(Scheduler_GetKernelQueue(ks)) < 0)
			return(-1);

		/* Interrupt issued. */
		ks->ks_waiting = -1;
		return(2);
	}

	/* Less than zero; already interrupted. */
	return(1);
}

STATIC(PyObj)
ks_enqueue(Scheduler ks, PyObj callable)
{
	TaskQueue tq = Scheduler_GetTaskQueue(ks);

	if (interrupt_wait(ks) < 0)
		return(NULL);

	if (taskq_enqueue(tq, callable) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

/**
	// Drain both sides of the task queue.
*/
STATIC(PyObj)
ks_execute(PyObj self, PyObj errctl)
{
	Scheduler ks = (Scheduler) self;
	TaskQueue tq = Scheduler_GetTaskQueue(ks);
	int total = 0, status = 0;

	status = taskq_execute(tq, errctl);
	if (status < 0)
		return(NULL);
	total += status;

	status = taskq_execute(tq, errctl);
	if (status < 0)
		return(NULL);
	total += status;

	Py_RETURN_INTEGER(total);
}

STATIC(int)
ks_termination(Scheduler ks, KernelQueue kq, TaskQueue tq)
{
	int r = 0;
	PyObj ops;
	Py_ssize_t i;

	ops = PyDict_Values(kq->kq_references);
	if (ops == NULL)
		return(-1);

	/* Collect and enqueue meta_terminate events */
	for (i = 0; i < PyList_GET_SIZE(ops); ++i)
	{
		PyObj op = PyList_GET_ITEM(ops, i);
		Link ln = (Link) op;
		Event ev = (Event) ln->ln_event;

		switch (Event_Type(ev))
		{
			case EV_TYPE_ID(meta_terminate):
			{
				if (taskq_enqueue(tq, ln) < 0)
				{
					r = -2;
					goto exit;
				}
			}
			break;

			default:
				;
			break;
		}
	}

	exit:
	{
		Py_CLEAR(ops);
		return(r);
	}
}

/**
	// Enqueue termination tasks and close the kernel queue resources.
*/
STATIC(PyObj)
ks_close(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	KernelQueue kq = Scheduler_GetKernelQueue(ks);
	TaskQueue tq = Scheduler_GetTaskQueue(ks);
	PyObj rob = NULL;

	switch (kernelq_close(kq))
	{
		case 0:
			/* Already closed. */
			ks->ks_waiting = 2;
			Py_RETURN_FALSE;
		break;

		case 1:
			/* Resources destroyed. */
			if (ks_termination(ks, kq, tq) < 0)
				return(NULL);
			ks->ks_waiting = 2;
			Py_RETURN_TRUE;
		break;

		case -1:
			PyErr_SetFromErrno(PyExc_OSError);
			return(NULL);
		break;
	}

	PyErr_SetString(PyExc_RuntimeError, "impossible switch case");
	return(NULL);
}

/**
	// Close the kqueue FD, and release references.
*/
STATIC(PyObj)
ks_void(PyObj self)
{
	Scheduler ks = (Scheduler) self;

	kernelq_close(Scheduler_GetKernelQueue(ks));
	Py_RETURN_NONE;
}

STATIC(PyObj)
ks_interrupt(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	PyObj rob;

	switch (interrupt_wait(ks))
	{
		case -1:
			return(NULL);
		break;

		case 0:
			rob = Py_None;
		break;

		case 1:
			rob = Py_False;
		break;

		case 2:
			rob = Py_True;
		break;
	}

	Py_INCREF(rob);
	return(rob);
}

/**
	// Replace the link in the reference dictionary, but return
	// the existing object if any.
*/
CONCEAL(int)
_kq_reference_update(KernelQueue kq, Link ln, PyObj *current)
{
	*current = PyDict_GetItem(kq->kq_references, ln->ln_event);

	if (*current != NULL)
	{
		/* Acquire the necessary reference to allow substitution. */
		if (PyList_Append(kq->kq_cancellations, *current) < 0)
			return(-1);
	}

	/* Insert or replace. */
	if (PyDict_SetItem(kq->kq_references, ln->ln_event, ln) < 0)
		return(-2);

	return(0);
}

CONCEAL(int)
_kq_reference_delete(KernelQueue kq, Event ev)
{
	PyObj current = PyDict_GetItem(kq->kq_references, (PyObj) ev);

	if (current != NULL)
	{
		/* Acquire the necessary reference to allow substitution. */
		if (PyList_Append(kq->kq_cancellations, current) < 0)
			return(-1);
	}

	if (PyDict_DelItem(kq->kq_references, ev) < 0)
		return(-2);

	return(0);
}

STATIC(PyObj)
ks_dispatch(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {"operation", "cyclic", NULL};

	Scheduler ks = (Scheduler) self;
	KernelQueue kq = Scheduler_GetKernelQueue(ks);
	Link ln;
	int cyclic = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O!|p", kwlist, &LinkType, &ln, &cyclic))
		return(NULL);

	switch (Event_Type(ln->ln_event))
	{
		case EV_TYPE_ID(meta_actuate):
		{
			if (ks->ks_waiting != 2 || kq->kq_root == -1)
			{
				PyErr_SetString(PyExc_ValueError, "scheduler already actuated");
				return(NULL);
			}

			/* fall default/kernelq_scheduler */
		}

		case EV_TYPE_ID(never):
		case EV_TYPE_ID(meta_terminate):
		default:
		{
			if (kernelq_schedule(kq, ln, cyclic) < 0)
				return(NULL);
		}
		break;
	}

	Py_INCREF(ln);
	return(ln);
}

STATIC(PyObj)
ks_cancel(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {"link", NULL};

	Scheduler ks = (Scheduler) self;
	Link ln = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O!", kwlist, &LinkType, &ln))
		return(NULL);

	switch (Event_Type(ln->ln_event))
	{
		default:
		{
			return(kernelq_cancel(Scheduler_GetKernelQueue(ks), ln));
		}
		break;
	}
}

STATIC(PyObj)
ks_operations(PyObj self)
{
	Scheduler ks = (Scheduler) self;

	return(PyDict_Values(Scheduler_GetKernelQueue(ks)->kq_references));
}

STATIC(PyObj)
ks__set_waiting(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	ks->ks_waiting = 1;
	Py_RETURN_NONE;
}

/**
	// collect and process kqueue events
*/
STATIC(PyObj)
ks_wait(PyObj self, PyObj args)
{
	Scheduler ks = (Scheduler) self;
	KernelQueue kq = Scheduler_GetKernelQueue(ks);
	int count = 0, error = 0;
	long secs = 16, ns = 0;

	if (!PyArg_ParseTuple(args, "|l", &secs))
		return(NULL);

	if (kq->kq_root != -1)
	{
		if (TQ_HAS_TASKS(Scheduler_GetTaskQueue(ks)))
		{
			secs = 0;
			ks->ks_waiting = 0;
		}
		else
		{
			if (secs > 0)
				ks->ks_waiting = 1;
			else
			{
				/* ms -> ns */
				ns = (-secs) * 1000000;
				secs = 0;
				ks->ks_waiting = 0;
			}
		}

		error = kernelq_receive(kq, secs, ns);
		ks->ks_waiting = 0;
		if (error < 0)
			return(NULL);
		count = kq->kq_event_count - kq->kq_event_position;

		if (kernelq_transition(kq, Scheduler_GetTaskQueue(ks)) < 0)
			return(NULL);
	}

	Py_RETURN_INTEGER(count);
}

STATIC(PyMethodDef)
ks_methods[] = {
	#define PyMethod_Id(N) ks_##N
		PyMethod_None(void),
		PyMethod_None(close),

		PyMethod_Variable(wait),
		PyMethod_None(interrupt),
		PyMethod_Sole(execute),

		PyMethod_Sole(enqueue),
		PyMethod_Keywords(dispatch),
		PyMethod_Keywords(cancel),
		PyMethod_None(operations),

		PyMethod_None(_set_waiting),
	#undef PyMethod_Id
	{NULL,},
};

STATIC(PyMemberDef)
ks_members[] = {
	{"waiting", T_PYSSIZET, offsetof(struct Scheduler, ks_waiting), READONLY, NULL},
	{NULL,},
};

STATIC(PyObj)
ks_get_closed(PyObj self, void *closure)
{
	Scheduler ks = (Scheduler) self;
	PyObj rob = Py_False;

	if (Scheduler_GetKernelQueue(ks)->kq_root == -1)
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

STATIC(PyObj)
ks_get_has_tasks(PyObj self, void *closure)
{
	Scheduler ks = (Scheduler) self;
	PyObj rob = Py_False;

	if (TQ_HAS_TASKS(Scheduler_GetTaskQueue(ks)))
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

STATIC(PyGetSetDef)
ks_getset[] = {
	{"closed", ks_get_closed, NULL, NULL},
	{"loaded", ks_get_has_tasks, NULL, NULL},
	{NULL,},
};

STATIC(int)
ks_clear(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	kernelq_clear(Scheduler_GetKernelQueue(ks));
	taskq_clear(Scheduler_GetTaskQueue(ks));
	return(0);
}

STATIC(void)
ks_dealloc(PyObj self)
{
	Scheduler ks = (Scheduler) self;

	taskq_clear(Scheduler_GetTaskQueue(ks));
	kernelq_clear(Scheduler_GetKernelQueue(ks));

	Py_TYPE(self)->tp_free(self);
}

STATIC(int)
ks_traverse(PyObj self, visitproc visit, void *arg)
{
	Scheduler ks = (Scheduler) self;

	if (kernelq_traverse(Scheduler_GetKernelQueue(ks), self, visit, arg) < 0)
		return(-1);

	if (taskq_traverse(Scheduler_GetTaskQueue(ks), self, visit, arg) < 0)
		return(-1);

	return(0);
}

STATIC(PyObj)
ks_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	PyObj rob;
	Scheduler ks;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL);

	rob = PyAllocate(subtype);
	if (rob == NULL)
		return(NULL);

	ks = (Scheduler) rob;
	ks->ks_waiting = 2;

	if (taskq_initialize(Scheduler_GetTaskQueue(ks)) < 0)
	{
		Py_DECREF(rob);
		return(NULL);
	}

	if (kernelq_initialize(Scheduler_GetKernelQueue(ks)) < 0)
	{
		Py_DECREF(rob);
		return(NULL);
	}

	return(rob);
}

/**
	// &.kernel.Scheduler
*/
PyTypeObject
SchedulerType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = FACTOR_PATH("Scheduler"),
	.tp_basicsize = sizeof(struct Scheduler),
	.tp_itemsize = 0,
	.tp_flags =
		Py_TPFLAGS_BASETYPE|
		Py_TPFLAGS_HAVE_GC|
		Py_TPFLAGS_DEFAULT,
	.tp_new = ks_new,
	.tp_dealloc = ks_dealloc,

	.tp_traverse = ks_traverse,
	.tp_clear = ks_clear,
	.tp_methods = ks_methods,
	.tp_members = ks_members,
	.tp_getset = ks_getset,
};
