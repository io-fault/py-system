/**
	// Kernel event scheduling and Python task queue for main loop management.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/event.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#ifndef HAVE_STDINT_H
	/* relying on Python's checks */
	#include <stdint.h>
#endif

#include "taskq.h"
#include "kernelq.h"
#include "scheduler.h"

static inline int
interrupt_wait(Scheduler ks)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	/**
		// Ignore interrupt if it's not waiting or has already been interrupted.
	*/
	if (ks->ks_waiting > 0)
	{
		if (kernelq_interrupt(Scheduler_GetKernelQueue(ks)) < 0)
			return(-1);

		ks->ks_waiting = -1;
		return(2);
	}
	else if (ks->ks_waiting < 0)
		return(1);
	else
		return(0);
}

static PyObj
ks_enqueue(Scheduler ks, PyObj callable)
{
	TaskQueue tq = Scheduler_GetTaskQueue(ks);

	if (interrupt_wait(ks) < 0)
		return(NULL);

	if (taskq_enqueue(tq, callable) != 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ks_execute(Scheduler ks, PyObj errctl)
{
	TaskQueue tq = Scheduler_GetTaskQueue(ks);
	return(taskq_execute(tq, errctl));
}

/**
	// Close the kqueue FD.
*/
static PyObj
ks_close(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	PyObj rob = NULL;

	switch (kernelq_close(Scheduler_GetKernelQueue(ks)))
	{
		case 0:
			/* Already closed. */
			Py_RETURN_FALSE;
		break;

		case 1:
			/* Resources destroyed. */
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
static PyObj
ks_void(PyObj self)
{
	Scheduler ks = (Scheduler) self;

	kernelq_close(Scheduler_GetKernelQueue(ks));
	Py_RETURN_NONE;
}

/**
	// Begin listening for the process exit event.
*/
static PyObj
ks_track(PyObj self, PyObj args)
{
	Scheduler ks = (Scheduler) self;
	KernelQueue kq = Scheduler_GetKernelQueue(ks);
	long l;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	return(kernelq_process_watch(kq, (pid_t) l, NULL));
}

static PyObj
ks_untrack(PyObj self, PyObj args)
{
	Scheduler ks = (Scheduler) self;
	KernelQueue kq = Scheduler_GetKernelQueue(ks);
	long l;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	return(kernelq_process_ignore(kq, (pid_t) l, NULL));
}

static PyObj
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

static PyObj
ks_defer(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Scheduler ks = (Scheduler) self;
	PyObj link = NULL;
	unsigned long l = 0;
	int unit = 's';

	/*
		// (link_object, period, unit)
	*/
	if (!PyArg_ParseTupleAndKeywords(args, kw, "Ok|C", (char **) kwlist, &link, &l, &unit))
		return(NULL);

	if (kernelq_defer(Scheduler_GetKernelQueue(ks), unit, l, link) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ks_recur(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Scheduler ks = (Scheduler) self;
	PyObj link = NULL;
	unsigned long l = 0;
	int unit = 's';

	/*
		// (link_object, period, unit)
	*/
	if (!PyArg_ParseTupleAndKeywords(args, kw, "Ok|C", (char **) kwlist, &link, &l, &unit))
		return(NULL);

	if (kernelq_recur(Scheduler_GetKernelQueue(ks), unit, l, link) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ks_cancel(PyObj self, PyObj link)
{
	Scheduler ks = (Scheduler) self;
	return(kernelq_cancel(Scheduler_GetKernelQueue(ks), link));
}

static PyObj
ks__set_waiting(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	ks->ks_waiting = 1;
	Py_RETURN_NONE;
}

/**
	// collect and process kqueue events
*/
static PyObj
ks_wait(PyObj self, PyObj args)
{
	Scheduler ks = (Scheduler) self;
	KernelQueue kq = Scheduler_GetKernelQueue(ks);
	int error = 0;
	long secs = 16, ns = 0;

	if (!PyArg_ParseTuple(args, "|l", &secs))
		return(NULL);

	/* Validate opened. */
	if (kq->kq_root == -1)
	{
		/* Closed kernelq, no events to read. */
		return(PyList_New(0));
	}

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

	error = kernelq_enqueue(kq, secs, ns);
	ks->ks_waiting = 0;
	if (error < 0)
		return(NULL);

	return(kernelq_transition(kq));
}

static PyMethodDef
ks_methods[] = {
	#define PyMethod_Id(N) ks_##N
		{"force", (PyCFunction) ks_interrupt, METH_NOARGS, NULL},
		{"alarm", (PyCFunction) ks_defer, METH_VARARGS|METH_KEYWORDS, NULL},

		PyMethod_None(void),
		PyMethod_None(close),

		PyMethod_Variable(wait),
		PyMethod_None(interrupt),
		PyMethod_Sole(execute),

		PyMethod_Variable(track),
		PyMethod_Variable(untrack),

		PyMethod_Keywords(defer),
		PyMethod_Sole(cancel),
		PyMethod_Keywords(recur),
		PyMethod_Sole(enqueue),

		PyMethod_None(_set_waiting),
	#undef PyMethod_Id
	{NULL,},
};

static PyMemberDef ks_members[] = {
	{"waiting", T_PYSSIZET, offsetof(struct Scheduler, ks_waiting), READONLY,
		PyDoc_STR("Whether or not the Scheduler object is with statement block.")},
	{NULL,},
};

static PyObj
ks_get_closed(PyObj self, void *closure)
{
	Scheduler ks = (Scheduler) self;
	PyObj rob = Py_False;

	if (Scheduler_GetKernelQueue(ks)->kq_root == -1)
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyObj
ks_get_has_tasks(PyObj self, void *closure)
{
	Scheduler ks = (Scheduler) self;
	PyObj rob = Py_False;

	if (TQ_HAS_TASKS(Scheduler_GetTaskQueue(ks)))
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyGetSetDef ks_getset[] = {
	{"closed", ks_get_closed, NULL, NULL},
	{"loaded", ks_get_has_tasks, NULL, NULL},
	{NULL,},
};

static int
ks_clear(PyObj self)
{
	Scheduler ks = (Scheduler) self;
	kernelq_clear(Scheduler_GetKernelQueue(ks));
	taskq_clear(Scheduler_GetTaskQueue(ks));
	return(0);
}

static void
ks_dealloc(PyObj self)
{
	Scheduler ks = (Scheduler) self;

	taskq_clear(Scheduler_GetTaskQueue(ks));
	kernelq_clear(Scheduler_GetKernelQueue(ks));

	Py_TYPE(self)->tp_free(self);
}

static int
ks_traverse(PyObj self, visitproc visit, void *arg)
{
	Scheduler ks = (Scheduler) self;

	if (kernelq_traverse(Scheduler_GetKernelQueue(ks), self, visit, arg) < 0)
		return(-1);

	if (taskq_traverse(Scheduler_GetTaskQueue(ks), self, visit, arg) < 0)
		return(-1);

	return(0);
}

static PyObj
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
	ks->ks_waiting = 0;

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
	FACTOR_PATH("Scheduler"),        /* tp_name */
	sizeof(struct Scheduler),        /* tp_basicsize */
	0,                            /* tp_itemsize */
	ks_dealloc,                   /* tp_dealloc */
	NULL,                         /* tp_print */
	NULL,                         /* tp_getattr */
	NULL,                         /* tp_setattr */
	NULL,                         /* tp_compare */
	NULL,                         /* tp_repr */
	NULL,                         /* tp_as_number */
	NULL,                         /* tp_as_sequence */
	NULL,                         /* tp_as_mapping */
	NULL,                         /* tp_hash */
	NULL,                         /* tp_call */
	NULL,                         /* tp_str */
	NULL,                         /* tp_getattro */
	NULL,                         /* tp_setattro */
	NULL,                         /* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_HAVE_GC|
	Py_TPFLAGS_DEFAULT,           /* tp_flags */
	NULL,                         /* tp_doc */
	ks_traverse,                  /* tp_traverse */
	ks_clear,                     /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	NULL,                         /* tp_iter */
	NULL,                         /* tp_iternext */
	ks_methods,                   /* tp_methods */
	ks_members,                   /* tp_members */
	ks_getset,                    /* tp_getset */
	NULL,                         /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	ks_new,                       /* tp_new */
};
