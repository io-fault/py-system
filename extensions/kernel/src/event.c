/**
	// &.kernel.Event implementation.
*/
#include <fcntl.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "Scheduling.h"

PyTypeObject EventType;
extern PyTypeObject KPortsType;

CONCEAL(kport_t)
fs_event_open(PyObj fsref)
{
	kport_t fd = -1;
	const char *path = PyBytes_AS_STRING(fsref);

	fd = open(path, O_EVTONLY);
	if (fd < 0)
		PyErr_SetFromErrno(PyExc_OSError);

	return(fd);
}

/**
	// Initialize an event for scheduling a timer.
*/
CONCEAL(int)
ev_nanoseconds(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {
		"nanosecond", "microsecond", "millisecond", "second", NULL
	};
	unsigned long ms = 0, s = 0;
	unsigned long long ns = 0, us = 0;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|KKkk", kwlist, &ns, &us, &ms, &s))
		return(-1);

	/* Convert to 64-bit nanosecond offset. */
	if (s > 0)
		us += (1000000ULL * s);
	if (ms > 0)
		ns += (1000000ULL * ms);
	if (us > 0)
		ns += (1000ULL * us);

	ev->ev_spec.evs_resource_t = evr_identifier;
	ev->ev_spec.evs_resource.time = ns;
	return(0);
}

/**
	// Initialize an event with a process identifier and optional port.
*/
CONCEAL(int)
ev_pid_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {"pid", "port", NULL};
	Py_ssize_t pid;
	kport_t port = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "n|i", kwlist, &pid, &port))
		return(-1);

	if (port < 0)
	{
		ev->ev_spec.evs_resource_t = evr_identifier;
		ev->ev_spec.evs_resource.process = pid;
	}
	else
	{
		ev->ev_spec.evs_resource_t = evr_kport;
		ev->ev_spec.evs_resource.procref.procfd = port;
		ev->ev_spec.evs_resource.procref.process = pid;
	}

	return(0);
}

/**
	// Initialize an event with a signal code and optional port.
*/
CONCEAL(int)
ev_signal_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {"signal", "port", NULL};
	int signo = 0;
	kport_t port = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "i|i", kwlist, &signo, &port))
		return(-1);

	if (port < 0)
	{
		ev->ev_spec.evs_resource_t = evr_identifier;
		ev->ev_spec.evs_resource.signal_code = signo;
	}
	else
	{
		ev->ev_spec.evs_resource_t = evr_kport;
		ev->ev_spec.evs_resource.sigref.sigfd = port;
		ev->ev_spec.evs_resource.sigref.signal_code = signo;
	}
	return(0);
}

/**
	// Initialize an event with an arbitrary reference.
*/
CONCEAL(int)
ev_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {
		"reference", NULL
	};
	PyObj ref = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O", kwlist, &ref))
		return(-1);

	Py_INCREF(ref);
	ev->ev_spec.evs_resource_t = evr_object;
	ev->ev_spec.evs_resource.ref_object = 0;
	return(0);
}

/**
	// Initialize an event with a filesystem Path.
	// Monitoring events(fs_delta, fs_void, fs_status).
*/
CONCEAL(int)
ev_filesystem_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {"path", "fileno", NULL};
	int fileno = -1;
	PyObj path = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", kwlist, &path, &fileno))
		return(-1);

	Py_INCREF(path);
	ev->ev_spec.evs_resource_t = evr_obkp_pair;
	ev->ev_spec.evs_resource.obkp.src = path;
	ev->ev_spec.evs_resource.obkp.kre = fileno;

	/* Open if fileno is not given. */
	if (fileno < 0)
	{
		PyObj bytespath = NULL;

		if (!PyUnicode_FSConverter(path, &bytespath))
			return(-1);
		ev->ev_spec.evs_resource.obkp.kre = fs_event_open(bytespath);
		Py_DECREF(bytespath);
	}

	return(0);
}

/**
	// Initialize an event with a pair of kport_t.
*/
CONCEAL(int)
ev_io_reference(Event ev, PyObj args, PyObj kw)
{
	static const char *kwlist[] = {
		"port", "correlation", NULL
	};
	int port = -1, rel = -1;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|ii", kwlist, &port, &rel))
		return(-1);

	ev->ev_spec.evs_resource_t = evr_kport;
	ev->ev_spec.evs_resource.io[0] = port;
	ev->ev_spec.evs_resource.io[1] = rel;
	return(0);
}

/* Constructors */
#define EV_TYPE(NAME, CONVERT) \
	STATIC(PyObj) \
	ev_##NAME(PyTypeObject *PYTYPE, PyObj args, PyObj kw) { \
		PyObj ROB; \
		Event ev; \
		ROB = PYTYPE->tp_alloc(PYTYPE, 0); \
		if (ROB == NULL) \
			return(NULL); \
		ev = (Event) ROB; \
		memset(&(ev->ev_spec.evs_resource), 0, sizeof(union EventResource)); \
		ev->ev_spec.evs_type = EV_TYPE_ID(NAME); \
		if (CONVERT(ev, args, kw) < 0) \
		{ \
			Py_DECREF(ROB); \
			return(NULL); \
		} \
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
		#define EV_TYPE(NAME, C) \
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
			#define EV_TYPE(NAME, C) PyMethod_Keywords(NAME),
				EVENT_TYPE_LIST()
			#undef EV_TYPE
		#undef PyMethod_TypeControl
	#undef PyMethod_Id
	{NULL,},
};

STATIC(PyObj)
ev_get_type(Event ev, void *closure)
{
	const char *typname = ev_type_name(ev->ev_spec.evs_type);
	return(PyUnicode_FromString(typname));
}

STATIC(PyObj)
ev_get_port(Event ev, void *closure)
{
	kport_t kp = -1;

	switch (Event_ResourceType(ev))
	{
		case evr_obkp_pair:
			kp = Event_Resource(ev)->obkp.kre;
		break;

		case evr_kport:
		{
			switch (Event_Type(ev))
			{
				case EV_TYPE_ID(process_signal):
					kp = Event_Resource(ev)->sigref.sigfd;
				break;

				case EV_TYPE_ID(process_exit):
					kp = Event_Resource(ev)->procref.procfd;
				break;

				default:
					kp = Event_Resource(ev)->io[0];
				break;
			}
		}
		break;

		default:
			Py_RETURN_NONE;
		break;
	}

	Py_RETURN_INTEGER(kp);
}

STATIC(PyObj)
ev_get_correlation(Event ev, void *closure)
{
	kport_t kp = -1;

	switch (Event_Type(ev))
	{
		case EV_TYPE_ID(io_status):
		case EV_TYPE_ID(io_receive):
		case EV_TYPE_ID(io_transmit):
			kp = Event_Resource(ev)->io[1];
		break;

		case EV_TYPE_ID(fs_status):
		case EV_TYPE_ID(fs_delta):
		case EV_TYPE_ID(fs_void):
		{
			PyObj path = Event_Resource(ev)->obkp.src;
			Py_INCREF(path);
			return(path);
		}
		break;

		default:
			Py_RETURN_NONE;
		break;
	}

	Py_RETURN_INTEGER(kp);
}

STATIC(PyObj)
ev_get_input(Event ev, void *closure)
{
	kport_t kp = -1;

	switch (Event_Type(ev))
	{
		case EV_TYPE_ID(io_receive):
			kp = Event_Resource(ev)->io[0];
		break;

		case EV_TYPE_ID(io_transmit):
			kp = Event_Resource(ev)->io[1];
		break;

		default:
			Py_RETURN_NONE;
		break;
	}

	Py_RETURN_INTEGER(kp);
}

STATIC(PyObj)
ev_get_output(Event ev, void *closure)
{
	kport_t kp = -1;

	switch (Event_Type(ev))
	{
		case EV_TYPE_ID(io_receive):
			kp = Event_Resource(ev)->io[1];
		break;

		case EV_TYPE_ID(io_transmit):
			kp = Event_Resource(ev)->io[0];
		break;

		default:
			Py_RETURN_NONE;
		break;
	}

	Py_RETURN_INTEGER(kp);
}

STATIC(PyGetSetDef)
ev_getset[] = {
	{"type", ev_get_type, NULL, NULL},

	{"port", ev_get_port, NULL, NULL},
	{"correlation", ev_get_correlation, NULL, NULL},

	/* Position independent access to kport's. */
	{"input", ev_get_input, NULL, NULL},
	{"output", ev_get_output, NULL, NULL},

	{NULL,},
};

STATIC(Py_hash_t)
ev_hash(Event ev)
{
	event_t *evs = &ev->ev_spec;
	Py_hash_t r = 0;

	switch (evs->evs_resource_t)
	{
		case evr_kport:
			r = evs->evs_resource.io[0];
		break;

		case evr_identifier:
		{
			switch (evs->evs_type)
			{
				case EV_TYPE_ID(process_signal):
					r = evs->evs_resource.signal_code;
				break;

				case EV_TYPE_ID(process_exit):
					r = evs->evs_resource.process;
				break;

				case EV_TYPE_ID(time):
				default:
					/* Pointer identity */
					r = (Py_hash_t) evs;
				break;
			}
		}
		break;
	}

	r |= (evs->evs_type << (sizeof(Py_hash_t) / 2));
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
			union EventResource *r1, *r2;
			r1 = Event_Resource(ev);
			r2 = Event_Resource(op);

			switch (ev->ev_spec.evs_type)
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
					if (memcmp(r1, r2, sizeof(union EventResource)) == 0)
						Py_RETURN_TRUE;
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
		event_t *evs = &(ev->ev_spec);

		memcpy(evs, Event_Specification(src), sizeof(event_t));

		switch (evs->evs_resource_t)
		{
			case evr_obkp_pair:
				Py_INCREF(evs->evs_resource.obkp.src);
			break;

			case evr_object:
				Py_INCREF(evs->evs_resource.ref_object);
			break;

			default:
				/* Nothing to do for other types. */
				;
			break;
		}
	}

	return(rob);
}

STATIC(void)
ev_clear(Event ev)
{
	switch (ev->ev_spec.evs_resource_t)
	{
		case evr_obkp_pair:
			Py_CLEAR(ev->ev_spec.evs_resource.obkp.src);
		break;

		case evr_object:
			Py_CLEAR(ev->ev_spec.evs_resource.ref_object);
		break;

		default:
			/* Nothing to do for other types. */
			;
		break;
	}

	ev->ev_spec.evs_resource_t = evr_none;
}

STATIC(int)
ev_traverse(PyObj self, visitproc visit, void *arg)
{
	Event ev = (Event) self;
	event_t *evs = Event_Specification(ev);

	switch (ev->ev_spec.evs_resource_t)
	{
		case evr_obkp_pair:
			Py_VISIT(evs->evs_resource.obkp.src);
		break;

		case evr_object:
			Py_VISIT(evs->evs_resource.ref_object);
		break;
	}

	return(0);
}

STATIC(void)
ev_dealloc(Event ev)
{
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
