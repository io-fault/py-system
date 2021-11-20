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
#include "events.h"

/**
	// Set Python exception from kevent error.
*/
static int
ev_check_kevent(kevent_t *ev)
{
	if (ev->flags & EV_ERROR && ev->data != 0)
	{
		errno = ev->data;
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	return(1);
}

static inline int
interrupt_wait(Events ev)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	/**
		// Ignore interrupt if it's not waiting or has already been interrupted.
	*/
	if (ev->ke_waiting > 0)
	{
		if (kernelq_interrupt(Events_GetKernelQueue(ev)) < 0)
			return(-1);

		ev->ke_waiting = -1;
		return(2);
	}
	else if (ev->ke_waiting < 0)
		return(1);
	else
		return(0);
}

static PyObj
ev_enqueue(Events ev, PyObj callable)
{
	TaskQueue tq = Events_GetTaskQueue(ev);

	if (interrupt_wait(ev) < 0)
		return(NULL);

	if (taskq_enqueue(tq, callable) != 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ev_execute(Events ev, PyObj errctl)
{
	TaskQueue tq = Events_GetTaskQueue(ev);
	return(taskq_execute(tq, errctl));
}

/**
	// Close the kqueue FD.
*/
static PyObj
ev_close(PyObj self)
{
	Events ev = (Events) self;
	PyObj rob = NULL;

	switch (kernelq_close(Events_GetKernelQueue(ev)))
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
ev_void(PyObj self)
{
	Events ev = (Events) self;

	kernelq_close(Events_GetKernelQueue(ev));
	Py_RETURN_NONE;
}

/**
	// Begin listening for the process exit event.
*/
static PyObj
ev_track(PyObj self, PyObj args)
{
	Events ev = (Events) self;
	KernelQueue kq = Events_GetKernelQueue(ev);
	long l;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	return(kernelq_process_watch(kq, (pid_t) l, NULL));
}

static PyObj
ev_untrack(PyObj self, PyObj args)
{
	Events ev = (Events) self;
	KernelQueue kq = Events_GetKernelQueue(ev);
	long l;

	if (!PyArg_ParseTuple(args, "l", &l))
		return(NULL);

	return(kernelq_process_ignore(kq, (pid_t) l, NULL));
}

static PyObj
ev_interrupt(PyObj self)
{
	Events ev = (Events) self;
	PyObj rob;

	switch (interrupt_wait(ev))
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
ev_defer(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Events ev = (Events) self;
	PyObj link = NULL;
	unsigned long l = 0;
	int unit = 's';

	/*
		// (link_object, period, unit)
	*/
	if (!PyArg_ParseTupleAndKeywords(args, kw, "Ok|C", (char **) kwlist, &link, &l, &unit))
		return(NULL);

	if (kernelq_defer(Events_GetKernelQueue(ev), unit, l, link) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ev_recur(PyObj self, PyObj args, PyObj kw)
{
	const static char *kwlist[] = {
		"link", "quantity", "unitcode", NULL,
	};

	Events ev = (Events) self;
	PyObj link = NULL;
	unsigned long l = 0;
	int unit = 's';

	/*
		// (link_object, period, unit)
	*/
	if (!PyArg_ParseTupleAndKeywords(args, kw, "Ok|C", (char **) kwlist, &link, &l, &unit))
		return(NULL);

	if (kernelq_recur(Events_GetKernelQueue(ev), unit, l, link) < 0)
		return(NULL);

	Py_RETURN_NONE;
}

static PyObj
ev_cancel(PyObj self, PyObj link)
{
	Events ev = (Events) self;
	return(kernelq_cancel(Events_GetKernelQueue(ev), link));
}

static PyObj
ev__set_waiting(PyObj self)
{
	Events ev = (Events) self;
	ev->ke_waiting = 1;
	Py_RETURN_NONE;
}

/**
	// collect and process kqueue events
*/
static PyObj
ev_wait(PyObj self, PyObj args)
{
	Events ev = (Events) self;
	KernelQueue kq = Events_GetKernelQueue(ev);
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

	if (TQ_HAS_TASKS(Events_GetTaskQueue(ev)))
	{
		secs = 0;
		ev->ke_waiting = 0;
	}
	else
	{
		if (secs > 0)
			ev->ke_waiting = 1;
		else
		{
			/* ms -> ns */
			ns = (-secs) * 1000000;
			secs = 0;
			ev->ke_waiting = 0;
		}
	}

	error = kernelq_enqueue(kq, secs, ns);
	ev->ke_waiting = 0;
	if (error < 0)
		return(NULL);

	return(kernelq_transition(kq));
}

static PyMethodDef
ev_methods[] = {
	#define PyMethod_Id(N) ev_##N
		{"force", (PyCFunction) ev_interrupt, METH_NOARGS, NULL},
		{"alarm", (PyCFunction) ev_defer, METH_VARARGS|METH_KEYWORDS, NULL},

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

static PyMemberDef ev_members[] = {
	{"waiting", T_PYSSIZET, offsetof(struct Events, ke_waiting), READONLY,
		PyDoc_STR("Whether or not the Events object is with statement block.")},
	{NULL,},
};

static PyObj
ev_get_closed(PyObj self, void *closure)
{
	Events ev = (Events) self;
	PyObj rob = Py_False;

	if (Events_GetKernelQueue(ev)->kq_root == -1)
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyObj
ev_get_has_tasks(PyObj self, void *closure)
{
	Events ev = (Events) self;
	PyObj rob = Py_False;

	if (TQ_HAS_TASKS(Events_GetTaskQueue(ev)))
		rob = Py_True;

	Py_INCREF(rob);
	return(rob);
}

static PyGetSetDef ev_getset[] = {
	{"closed", ev_get_closed, NULL, NULL},
	{"loaded", ev_get_has_tasks, NULL, NULL},
	{NULL,},
};

static int
ev_clear(PyObj self)
{
	Events ev = (Events) self;
	kernelq_clear(Events_GetKernelQueue(ev));
	taskq_clear(Events_GetTaskQueue(ev));
	return(0);
}

static void
ev_dealloc(PyObj self)
{
	Events ev = (Events) self;

	taskq_clear(Events_GetTaskQueue(ev));
	kernelq_clear(Events_GetKernelQueue(ev));

	Py_TYPE(self)->tp_free(self);
}

static int
ev_traverse(PyObj self, visitproc visit, void *arg)
{
	Events ev = (Events) self;

	if (kernelq_traverse(Events_GetKernelQueue(ev), self, visit, arg) < 0)
		return(-1);

	if (taskq_traverse(Events_GetTaskQueue(ev), self, visit, arg) < 0)
		return(-1);

	return(0);
}

static PyObj
ev_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	PyObj rob;
	Events ev;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL);

	rob = PyAllocate(subtype);
	if (rob == NULL)
		return(NULL);

	ev = (Events) rob;
	ev->ke_waiting = 0;

	if (taskq_initialize(Events_GetTaskQueue(ev)) < 0)
	{
		Py_DECREF(rob);
		return(NULL);
	}

	if (kernelq_initialize(Events_GetKernelQueue(ev)) < 0)
	{
		Py_DECREF(rob);
		return(NULL);
	}

	return(rob);
}

/**
	// &.kernel.Events
*/
PyTypeObject
EventsType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	FACTOR_PATH("Events"),        /* tp_name */
	sizeof(struct Events),        /* tp_basicsize */
	0,                            /* tp_itemsize */
	ev_dealloc,                   /* tp_dealloc */
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
	ev_traverse,                  /* tp_traverse */
	ev_clear,                     /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	NULL,                         /* tp_iter */
	NULL,                         /* tp_iternext */
	ev_methods,                   /* tp_methods */
	ev_members,                   /* tp_members */
	ev_getset,                    /* tp_getset */
	NULL,                         /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	ev_new,                       /* tp_new */
};
