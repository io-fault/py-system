/**
	// &.kernel.Event implementation.
*/
#include <fcntl.h>
#include <signal.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "Scheduling.h"

PyTypeObject EventType;
extern PyTypeObject KPortsType;

/**
	// Initialize an event for scheduling a timer.
*/
CONCEAL(int)
ev_time_units(Event ev, PyObj args, PyObj kw)
{
	const char *kwlist[] = {
		"units", "port", NULL
	};
	kport_t kp = -1;
	PyObj src = NULL;
	uint64_t ns = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", (char **) kwlist, &src, &kp))
		return(-1);

	ns = PyLong_AsUnsignedLongLong(src);
	if (ns == (uint64_t)-1 && PyErr_Occurred())
		return(NULL);

	#if __linux__
	{
		struct itimerspec old, its = {
			.it_interval = {
				.tv_sec  = ns / 1000000000ULL,
				.tv_nsec = (ns == 0 ? 1 : ns) % 1000000000ULL,
			},
		};
		its.it_value = its.it_interval;

		kp = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
		if (kp < 0)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(-2);
		}

		if (timerfd_settime(kp, 0, &its, &old) < 0)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			close(kp);
			errno = 0;

			return(-3);
		}
	}
	#else
		if (kp != -1)
		{
			PyErr_SetString(PyExc_ValueError, "port override not available");
			return(-4);
		}
	#endif

	Event_SetTime(ev, ns);
	Event_SetKPort(ev, kp);
	Event_SetSource(ev, src);
	return(0);
}

/**
	// Initialize an event with a process identifier and optional port.
*/
CONCEAL(int)
ev_pid_reference(Event ev, PyObj args, PyObj kw)
{
	const char *kwlist[] = {"pid", "port", NULL};
	PyObj src = NULL;
	long proc = 0;
	kport_t kp = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", (char **) kwlist, &src, &kp))
		return(-1);

	proc = PyLong_AsLong(src);
	if (proc == -1 && PyErr_Occurred())
		return(-2);

	#ifdef __linux__
	if (kp < 0)
	{
		int pidfd_open(pid_t, unsigned int);

		kp = pidfd_open((pid_t) proc, 0);
		if (kp < 0)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(-3);
		}
	}
	#endif

	Event_SetProcess(ev, ((pid_t) proc));
	Event_SetKPort(ev, kp);
	Event_SetSource(ev, src);
	return(0);
}

/**
	// Initialize an event with a signal code and optional port.
*/
CONCEAL(int)
ev_signal_reference(Event ev, PyObj args, PyObj kw)
{
	const char *kwlist[] = {"signal", "port", NULL};
	PyObj src = NULL;
	int signo = 0;
	kport_t kp = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", (char **) kwlist, &src, &kp))
		return(-1);

	signo = PyLong_AsLong(src);
	if (signo == -1 && PyErr_Occurred())
		return(-2);

	#ifdef __linux__
	if (kp < 0)
	{
		sigset_t mask, old;

		if (sigemptyset(&mask) < 0)
			goto return_errno;

		if (sigaddset(&mask, signo) < 0)
			goto return_errno;

		if (sigprocmask(SIG_BLOCK, &mask, &old) < 0)
			goto return_errno;

		kp = signalfd(-1, &mask, SFD_CLOEXEC);
		if (kp < 0)
			goto return_errno;
	}
	#endif

	Event_SetSignal(ev, signo);
	Event_SetKPort(ev, kp);
	Event_SetSource(ev, src);
	return(0);

	return_errno:
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(-1);
	}
}

/**
	// Initialize an event with an arbitrary reference.
*/
CONCEAL(int)
ev_reference(Event ev, PyObj args, PyObj kw)
{
	const char *kwlist[] = {
		"reference", "port", NULL
	};

	PyObj src = NULL;
	kport_t kp = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", (char **) kwlist, &src, &kp))
		return(-1);

	#ifdef __linux__
	if (kp < 0 && Event_Type(ev) != EV_TYPE_ID(meta_exception))
	{
		kp = eventfd(0, EFD_CLOEXEC);
		if (kp < 0)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(-1);
		}

		if (Event_Type(ev) == EV_TYPE_ID(meta_actuate))
		{
			uint64_t u = 1;
			if (write(kp, &u, sizeof(u)) < 0)
			{
				PyErr_SetFromErrno(PyExc_OSError);
				return(-1);
			}
		}
	}
	#endif

	Event_SetKPort(ev, kp);
	Event_SetSource(ev, src);
	return(0);
}

/**
	// Initialize an event with a filesystem Path.
	// Monitoring events(fs_delta, fs_void, fs_status).
*/
CONCEAL(int)
ev_filesystem_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {"path", "port", NULL};
	int kp = -1;
	PyObj path = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", (char **) kwlist, &path, &kp))
		return(-1);

	/* Open if kp is not given. */
	if (kp < 0)
	{
		PyObj bytespath = NULL;

		if (!PyUnicode_FSConverter(path, &bytespath))
			return(-2);

		kp = fs_event_open(PyBytes_AS_STRING(bytespath), Event_Type(ev));
		Py_DECREF(bytespath);
		if (kp < 0)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			return(-3);
		}
	}

	Event_SetKPort(ev, kp);
	Event_SetSource(ev, path);
	return(0);
}

/**
	// Initialize an event with a pair of kport_t.
*/
CONCEAL(int)
ev_io_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {
		"source", "port", "correlation", NULL
	};
	PyObj src = NULL;
	kport_t kp = -1;
	kport_t cor = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|ii", (char **) kwlist, &src, &kp, &cor))
		return(-1);

	Event_SetCorrelation(ev, cor);
	Event_SetKPort(ev, kp);
	Event_SetSource(ev, src);
	return(0);
}

/* Constructors */
#define EV_TYPE(NAME, CONVERT, CYCLIC) \
	STATIC(PyObj) \
	ev_##NAME(PyTypeObject *PYTYPE, PyObj args, PyObj kw) { \
		PyObj ROB; \
		Event ev; \
		ROB = PYTYPE->tp_alloc(PYTYPE, 0); \
		if (ROB == NULL) \
			return(NULL); \
		ev = (Event) ROB; \
		Event_Type(ev) = EV_TYPE_ID(NAME); \
		Event_SetTime(ev, 0); \
		Event_SetSource(ev, NULL); \
		Event_SetKPort(ev, -1); \
		if (CONVERT(ev, args, kw) < 0) \
		{ \
			Py_DECREF(ROB); \
			return(NULL); \
		} \
		Py_INCREF(Event_GetSource(ev)); \
		return(ROB); \
	}

	/* Excludes invalid and filesystem. */
	EVENT_TYPE_LIST()
#undef EV_TYPE

STATIC(PyObj)
ev_constructor(PyObj typ, PyObj typstring)
{
	PyObj rob;
	const char *typname = NULL, *normal;
	enum EventType ev_type = EV_TYPE_ID(invalid);

	if (PyUnicode_Check(typstring))
		typname = PyUnicode_AsUTF8(typstring);
	else if (PyBytes_Check(typstring))
		typname = PyBytes_AS_STRING(typstring);
	else
	{
		PyErr_SetString(PyExc_TypeError, "event type name must be a str or bytes object");
		return(NULL);
	}

	ev_type = ev_type_code(typname);

	switch (ev_type)
	{
		#define EV_TYPE(NAME, C, CYCLIC) \
			case EV_TYPE_ID(NAME): \
				normal = #NAME; \
			break;

			EVENT_TYPE_LIST()
		#undef EV_TYPE

		default:
			PyErr_SetString(PyExc_ValueError, "unrecognized event type identifier");
			return(NULL);
		break;
	}

	return(PyObject_GetAttrString(typ, normal));
}

STATIC(PyMethodDef)
ev_methods[] = {
	#define PyMethod_Id(N) ev_##N
		/* Class methods */
		#undef PyMethod_TypeControl
		#define PyMethod_TypeControl PyMethod_ClassType
			/**
			// Event.type(i) constructors. */
			PyMethod_Sole(constructor),
			#define EV_TYPE(NAME, C, CYCLIC) PyMethod_Keywords(NAME),
				EVENT_TYPE_LIST()
			#undef EV_TYPE
		#undef PyMethod_TypeControl
	#undef PyMethod_Id
	{NULL,},
};

STATIC(PyObj)
ev_get_type(Event ev, void *closure)
{
	const char *typname = ev_type_name(Event_Type(ev));
	return(PyUnicode_FromString(typname));
}

STATIC(PyObj)
ev_get_port(Event ev, void *closure)
{
	Py_RETURN_INTEGER(Event_GetKPort(ev));
}

STATIC(PyObj)
ev_get_source(Event ev, void *closure)
{
	PyObj rob = Event_GetSource(ev);
	Py_INCREF(rob);
	return(rob);
}

STATIC(PyGetSetDef)
ev_getset[] = {
	{"type", ev_get_type, NULL, NULL},
	{"source", ev_get_source, NULL, NULL},
	{"port", ev_get_port, NULL, NULL},

	{NULL,},
};

STATIC(Py_hash_t)
ev_hash(Event ev)
{
	int typshift = sizeof(Py_hash_t) / 2;
	event_t *evs = Event_Specification(ev);
	kport_t kp = Event_GetKPort(ev);
	Py_hash_t r = 0;

	if (kp == -1)
	{
		switch (evs->evs_type)
		{
			case EV_TYPE_ID(process_signal):
				r = Event_GetSignal(ev);
			break;

			case EV_TYPE_ID(process_exit):
				r = Event_GetProcess(ev);
			break;

			case EV_TYPE_ID(time):
			default:
				/* Pointer identity if not a kport. */
				r = (Py_hash_t) evs;
			break;
		}
	}
	else
	{
		r = (Py_hash_t) kp;
		typshift += 2;
	}

	r |= (evs->evs_type << typshift);

	if (r == -1)
		r = -2;
	return(r);
}

STATIC(PyObj)
ev_richcompare(Event ev, PyObj operand, int cmpop)
{
	if (cmpop != Py_EQ)
		Py_RETURN_NOTIMPLEMENTED;

	/* Exact; covers the timer case. */
	if (ev == operand)
		Py_RETURN_TRUE;

	/* Requires &.kernel.Event instance. */
	if (Py_TYPE(operand) == (&EventType))
	{
		Event op = (Event) operand;

		if (Event_Type(ev) == Event_Type(op))
		{
			event_t *r1 = Event_Specification(ev);
			event_t *r2 = Event_Specification(op);

			switch (Event_Type(ev))
			{
				case EV_TYPE_ID(time):
				{
					/*
						// Only identical (timer) instances should be seen as equal.
					*/
					Py_RETURN_FALSE;
				}
				break;

				default:
				{
					if (Event_GetKPort(ev) != -1)
					{
						if (Event_GetKPort(ev) == Event_GetKPort(op))
							Py_RETURN_TRUE;
					}
					else if (memcmp(&r1->evs_field, &r2->evs_field, sizeof(r1->evs_field)) == 0)
						Py_RETURN_TRUE;
				}
				break;
			}
		}
	}

	Py_RETURN_FALSE;
}

/**
	// Copy Event. Type specific constructors must be used to create new instances.
*/
STATIC(PyObj)
ev_new(PyTypeObject *typ, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {"source", NULL};
	PyObj rob;
	Event src;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O!", kwlist, &EventType, &src))
		return(NULL);

	rob = typ->tp_alloc(typ, 0);
	if (rob != NULL)
	{
		Event ev = (Event) rob;
		event_t *evs = Event_Specification(ev);

		memcpy(evs, Event_Specification(src), sizeof(event_t));
		Py_XINCREF(Event_GetSource(ev));

		if (evs->evs_kresource != -1)
		{
			evs->evs_kresource = dup(evs->evs_kresource);
		}

	}

	return(rob);
}

STATIC(void)
ev_release(Event ev)
{
	int errsnap = errno;
	kport_t kp;

	kp = Event_GetKPort(ev);
	if (kp < 0)
		return;

	if (close(kp) < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		PyErr_WriteUnraisable(ev);
		errno = errsnap;
	}

	return;
}

STATIC(void)
ev_clear(Event ev)
{
	Py_CLEAR(Event_GetSource(ev));
}

STATIC(int)
ev_traverse(PyObj self, visitproc visit, void *arg)
{
	Event ev = (Event) self;
	Py_VISIT(Event_GetSource(ev));
	return(0);
}

STATIC(void)
ev_dealloc(Event ev)
{
	PyObject_GC_UnTrack(ev);
	ev_release(ev);
	ev_clear(ev);
	ev->ev_spec.evs_type = EV_TYPE_ID(invalid);
	Py_TYPE(ev)->tp_free(ev);
}

/**
	// &.kernel.Event
*/
CONCEAL(PyTypeObject)
EventType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = FACTOR_PATH("Event"),
	.tp_basicsize = sizeof(struct Event),
	.tp_itemsize = 0,
	.tp_flags =
		Py_TPFLAGS_DEFAULT|
		Py_TPFLAGS_HAVE_GC,
	.tp_clear = ev_clear,
	.tp_traverse = ev_traverse,
	.tp_methods = ev_methods,
	.tp_getset = ev_getset,
	.tp_hash = ev_hash,
	.tp_richcompare = ev_richcompare,
	.tp_new = ev_new,
	.tp_dealloc = ev_dealloc,
};
