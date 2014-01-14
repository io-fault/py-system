#if 0
csource = """
#endif
/*
 * kernel.py.c - kernel interfaces for process exit signalling
 */
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>

/* file descriptor transfers */
#include <sys/param.h>

#include <sys/event.h>
typedef struct kevent kevent_t; /* kernel event description */
#define KQ_FILTERS() \
	FILTER(EVFILT_USER) \
	FILTER(EVFILT_PROC) \
	FILTER(EVFILT_SIGNAL) \
	FILTER(EVFILT_VNODE) \
	FILTER(EVFILT_TIMER)

#define KQ_FLAGS() \
	FLAG(EV_ADD) \
	FLAG(EV_ENABLE) \
	FLAG(EV_DISABLE) \
	FLAG(EV_DELETE) \
	FLAG(EV_RECEIPT) \
	FLAG(EV_ONESHOT) \
	FLAG(EV_CLEAR) \
	FLAG(EV_EOF) \
	FLAG(EV_ERROR)

/*
 * Signals that kernel.Interface will listen for automatically.
 *
 * SIGINT is handled lib.control() with signal.signal.
 * SIGUSR2 is *explicitly* used to trigger interjections.
 *
 * Important to note that *all* Context instances will receive signals.
 */
#define KQ_SIGNALS() \
	SIGNAME(SIGTERM) \
	SIGNAME(SIGHUP) \
	SIGNAME(SIGCONT) \
	SIGNAME(SIGINFO) \
	SIGNAME(SIGWINCH) \
	SIGNAME(SIGUSR1)

/*
 *	SIGNAME(SIGUSR2)
 *
 *	SIGUSR2 is used to interrupt the main thread. This is used
 *	to allow system.interject() to operate.
 */

#ifndef HAVE_STDINT_H
/* relying on Python's checks */
#include <stdint.h>
#endif

#ifndef CONFIG_STATIC_KEVENTS
#define CONFIG_STATIC_KEVENTS 16
#elif CONFIG_STATIC_KEVENTS < 8
#undef CONFIG_STATIC_KEVENTS
#define CONFIG_STATIC_KEVENTS 16
#warning nope.
#endif

#ifndef CONFIG_SYSCALL_RETRY
#define CONFIG_SYSCALL_RETRY 64
#elif CONFIG_SYSCALL_RETRY < 8
#undef CONFIG_SYSCALL_RETRY
#define CONFIG_SYSCALL_RETRY 16
#warning nope.
#endif

/*
 * Manage retry state for limiting the number of times we'll accept EINTR.
 */
#define _RETRY_STATE _avail_retries
#define RETRY_STATE_INIT int _RETRY_STATE = CONFIG_SYSCALL_RETRY
#define LIMITED_RETRY() do { \
	if (_RETRY_STATE > 0) { \
		errno = 0; \
		--_RETRY_STATE; \
		goto RETRY_SYSCALL; \
	} \
} while(0);
#define UNLIMITED_RETRY() errno = 0; goto RETRY_SYSCALL;

typedef int kpoint_t; /* file descriptor */
typedef int kerror_t; /* kerror error identifier (errno) */

struct Interface {
	PyObject_HEAD

	kpoint_t kif_kqueue; /* kqueue(2) fd */
	kevent_t kif_events[8];
	int kif_waiting;
};
typedef struct Interface *Interface;

static int
interface_kevent(Interface kif, int retry, int *out, kevent_t *changes, int nchanges, kevent_t *events, int nevents, const struct timespec *timeout)
{
	RETRY_STATE_INIT;
	int r = -1;

RETRY_SYSCALL:
	r = kevent(kif->kif_kqueue, changes, nchanges, events, nevents, timeout);
	if (r >= 0)
	{
		/*
		 * EV_ERROR is used in cases where kevent(2) fails after it already processed
		 * some events. In these cases, the EV_ERROR flag is used to note the case.
		 */
		if (r > 0 && events[r-1].flags & EV_ERROR)
		{
			--r;
			*out = r;
			/*
			 * XXX: Set error from EV_ERROR?
			 */
		}
		else
			*out = r;
	}
	else
	{
		/*
		 * Complete failure. Probably an interrupt or EINVAL.
		 */
		*out = 0;

		switch (errno)
		{
			case EINTR:
				/*
				 * The caller can designate whether or not retry will occur.
				 */
				switch (retry)
				{
					case -1:
						UNLIMITED_RETRY();
					break;

					case 1:
						LIMITED_RETRY();
					break;

					case 0:
						/*
						 * Purposefully allow it to fall through. Usually a signal occurred.
						 * For nucleus, falling through is appropriate as it usually means
						 * processing an empty task queue.
						 */
						*out = 0;
						return(1);
					break;
				}
			case ENOMEM:
				LIMITED_RETRY();

			default:
				return(0);
			break;
		}
	}

	return(1);
}

static PyObj
interface_repr(PyObj self)
{
	Interface kif = (Interface) self;
	PyObj rob;

	rob = PyUnicode_FromFormat("");

	return(rob);
}

/*
 * Relay to the generated transit alloc functions.
 * See the preproc blackmagic at the bottom of the file.
 */

static int
interface_init(Interface kif)
{
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t kev;

	kif->kif_kqueue = kqueue();
	if (kif->kif_kqueue == -1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	/*
	 * Init USER filter for wait() interruptions.
	 */
	kev.udata = (void *) kif;
	kev.ident = (uintptr_t) kif;
	kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR;
	kev.filter = EVFILT_USER;
	kev.fflags = 0;
	kev.data = 0;

	if (!interface_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

#define SIGNAME(SN) \
	kev.udata = NULL; \
	kev.data = 0; \
	kev.ident = (uintptr_t) SN; \
	kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR; \
	kev.filter = EVFILT_SIGNAL; \
	kev.fflags = 0; \
\
	if (!interface_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts)) { \
		PyErr_SetFromErrno(PyExc_OSError); \
		return(0); }
	KQ_SIGNALS()
#undef SIGNAME

	return(1);
}

/*
 * Interface.void()
 *
 * Close down the kqueue and create a new one.
 */
static PyObj
interface_void(PyObj self)
{
	Interface kif = (Interface) self;

	if (kif->kif_kqueue != -1)
		close(kif->kif_kqueue);

	kif->kif_kqueue = -1;

	if (!interface_init(kif))
	{
		Py_DECREF(((PyObj) kif));
		return(NULL);
	}

	Py_RETURN_NONE;
}

/*
 * Interface.track()
 */
static PyObj
interface_track(PyObj self, PyObj ob)
{
	Interface kif = (Interface) self;
	long l;
	const struct timespec ts = {0,0};
	int nkevents;
	kevent_t kev;

	l = PyLong_AsLong(ob);
	if (PyErr_Occurred())
		return(NULL);

	kev.udata = NULL;
	kev.data = 0;
	kev.ident = (uintptr_t) l;
	kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR;
	kev.filter = EVFILT_PROC;
	kev.fflags = NOTE_EXIT|NOTE_SIGNAL;

	if (!interface_kevent(kif, 1, &nkevents, &kev, 1, NULL, 0, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	Py_RETURN_NONE;
}

static PyObj
interface_force(PyObj self)
{
	Interface kif = (Interface) self;
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	/*
	 * Ignore force if we're not waiting.
	 */
	if (kif->kif_waiting == 0)
	{
		Py_RETURN_NONE;
	}

	kev.udata = (void *) kif;
	kev.ident = (uintptr_t) kif;
	kev.filter = EVFILT_USER;
	kev.fflags = NOTE_TRIGGER;
	kev.data = 0;
	kev.flags = EV_RECEIPT;

	if (!interface_kevent(kif, 1, &out, &kev, 1, NULL, 0, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	Py_RETURN_NONE;
}

static PyObj
interface_alarm(PyObj self, PyObj ob)
{
	Interface kif = (Interface) self;
	struct timespec ts = {0,0};
	kevent_t kev;
	long l = 0;
	int nkevents = 0;

	l = PyLong_AsLong(ob);

	/*
	 * Init USER filter for wait() interruptions.
	 */
	kev.udata = NULL;
	kev.ident = (uintptr_t) 1;
	kev.data = l;
	kev.flags = EV_ADD|EV_RECEIPT|EV_ONESHOT|EV_ENABLE;
	kev.filter = EVFILT_TIMER;
	kev.fflags = NOTE_USECONDS;

	if (!interface_kevent(kif, 1, &nkevents, &kev, 1, &kev, 1, &ts))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	Py_RETURN_NONE;
}

static const char *
signal_string(int sig)
{
	switch (sig)
	{
		case SIGCONT:
			return "continue";
		break;

		case SIGTERM:
			return "terminate";
		break;

		case SIGHUP:
			return "delta";
		break;

#ifdef SIGINFO
		case SIGINFO:
			return "terminal.query";
		break;
#endif

		case SIGWINCH:
			return "terminal.delta";
		break;

		case SIGUSR1:
			return "tunnel";
		break;

		case SIGUSR2:
			return "trip";
		break;

		default:
			return "";
		break;
	}
}

/*
 * interface_wait() - collect and process traffic events
 */
static PyObj
interface_wait(PyObj self)
{
	Interface kif = (Interface) self;
	PyObj rob;
	int i, nkevents = 0;
	int error = 0;

	const static struct timespec nowait = {0,0};
	const static struct timespec waitfor = {9,0};

	//struct timespec *wait = (struct timespec *) (waiting ? &waitfor : &nowait);
	kif->kif_waiting = 1;

	Py_BEGIN_ALLOW_THREADS

	if (!interface_kevent(kif, 0, &nkevents, NULL, 0, kif->kif_events, CONFIG_STATIC_KEVENTS, NULL))
	{
		error = 1;
	}

	Py_END_ALLOW_THREADS

	kif->kif_waiting = 0;

	if (error)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	/*
	 * Process the collected events.
	 */
	rob = PyList_New(0);
	if (rob == NULL)
		return(NULL);

	for (i = 0; i < nkevents; ++i)
	{
		kevent_t *kev;
		PyObj ob;

		kev = &(kif->kif_events[i]);

		switch (kev->filter)
		{
			case EVFILT_PROC:
				ob = Py_BuildValue("(si)", "process-delta", (int) kev->ident);
			break;

			case EVFILT_SIGNAL:
				ob = Py_BuildValue("(ss)", "signal", signal_string(kev->ident));
			break;

			case EVFILT_TIMER:
				ob = Py_BuildValue("(si)", "timer", (PyObj) kev->udata);
			break;

			case EVFILT_USER:
				continue;
			break;

			default:
				continue;
			break;
		}

		PyList_Append(rob, ob);
	}

	return(rob);
}

static PyMethodDef
interface_methods[] = {
	{"void", (PyCFunction) interface_void,
		METH_NOARGS, PyDoc_STR(
"void()\n\n"
"\n"
"\n"
)},

	{"track", (PyCFunction) interface_track, METH_O,
		PyDoc_STR(
":returns: None\n"
":rtype: NoneType\n"
"\n"
"Watch a process so that an event will be generated when it exits.\n"
)},

	{"alarm", (PyCFunction) interface_alarm, METH_O,
		PyDoc_STR(
":returns: None\n"
":rtype: NoneType\n"
"\n"
"\n"
)},

	{"force", (PyCFunction) interface_force, METH_NOARGS,
		PyDoc_STR(
":returns: None\n"
":rtype: NoneType\n"
"\n"
"\n"
)},

	{"wait",
		(PyCFunction) interface_wait, METH_NOARGS,
		PyDoc_STR(
":returns: Sequence of events that occurred while waiting.\n"
":rtype: list\n"
"\n"
)},
	{NULL,},
};

static PyMemberDef interface_members[] = {
	{"waiting", T_PYSSIZET, offsetof(struct Interface, kif_waiting), READONLY,
		PyDoc_STR("Whether or not the Interface object has a running wait() call.")},
	{NULL,},
};

static void
interface_dealloc(PyObj self)
{
	Interface kif = (Interface) self;

	if (kif->kif_kqueue != -1)
	{
		close(kif->kif_kqueue);
	}
}

static PyObj
interface_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	Interface kif;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL); XCOVERAGE

	kif = (Interface) subtype->tp_alloc(subtype, 0);
	if (kif == NULL)
		return(NULL); XCOVERAGE

	kif->kif_kqueue = -1;
	kif->kif_waiting = 0;

	if (!interface_init(kif))
	{
		Py_DECREF(((PyObj) kif));
		return(NULL);
	}

	return((PyObj) kif);
}

PyDoc_STRVAR(interface_doc,
"The kernel Interface implementation providing event driven signalling for control signals and subprocess exits.");

PyTypeObject
InterfaceType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	QPATH("Interface"),			/* tp_name */
	sizeof(struct Interface),	/* tp_basicsize */
	0,									/* tp_itemsize */
	interface_dealloc,			/* tp_dealloc */
	NULL,								/* tp_print */
	NULL,								/* tp_getattr */
	NULL,								/* tp_setattr */
	NULL,								/* tp_compare */
	NULL,								/* tp_repr */
	NULL,								/* tp_as_number */
	NULL,								/* tp_as_sequence */
	NULL,								/* tp_as_mapping */
	NULL,								/* tp_hash */
	NULL,								/* tp_call */
	NULL,								/* tp_str */
	NULL,								/* tp_getattro */
	NULL,								/* tp_setattro */
	NULL,								/* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_DEFAULT,			/* tp_flags */
	interface_doc,					/* tp_doc */
	NULL,								/* tp_traverse */
	NULL,								/* tp_clear */
	NULL,								/* tp_richcompare */
	0,									/* tp_weaklistoffset */
	NULL,								/* tp_iter */
	NULL,								/* tp_iternext */
	interface_methods,			/* tp_methods */
	interface_members,			/* tp_members */
	NULL,								/* tp_getset */
	NULL,								/* tp_base */
	NULL,								/* tp_dict */
	NULL,								/* tp_descr_get */
	NULL,								/* tp_descr_set */
	0,									/* tp_dictoffset */
	NULL,								/* tp_init */
	NULL,								/* tp_alloc */
	interface_new,					/* tp_new */
};

static PyObj
set_process_title(PyObj mod, PyObj title)
{
	PyObj bytes;

#ifndef __MACH__
	/*
	 * no support on darwin
	 */
	bytes = PyUnicode_AsUTF8String(title);

	if (bytes == NULL)
		return(NULL);

	setproctitle("%s", PyBytes_AS_STRING(bytes));
	Py_DECREF(bytes);
#endif

	Py_RETURN_NONE;
}

METHODS() = {
	{"set_process_title",
		(PyCFunction) set_process_title, METH_O,
		PyDoc_STR(
":returns: None\n"
"\n"
"Set the process title on supporting platforms."
)},
	{NULL,}
};

#define PYTHON_TYPES() \
	ID(Interface)

INIT(PyDoc_STR("Kernel interfaces for supporting nucleus based process management.\n"))
{
	PyObj mod = NULL;

#if TEST()
	Py_XDECREF(__EOVERRIDE__);
	__EOVERRIDE__ = PyDict_New();
	if (__EOVERRIDE__ == NULL)
		return(NULL); XCOVERAGE

	Py_XDECREF(__POVERRIDE__);
	__POVERRIDE__ = PyDict_New();
	if (__POVERRIDE__ == NULL)
		return(NULL); XCOVERAGE
#endif

	CREATE_MODULE(&mod);
	if (mod == NULL)
		return(NULL); XCOVERAGE

#if TEST()
	PyModule_AddObject(mod, "EOVERRIDE", __EOVERRIDE__);
	PyModule_AddObject(mod, "POVERRIDE", __POVERRIDE__);
#endif

	/*
	 * Initialize Transit types.
	 */
#define ID(NAME) \
	if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
		goto error; \
	if (PyModule_AddObject(mod, #NAME, (PyObj) &( NAME##Type )) < 0) \
		goto error;
	PYTHON_TYPES()
#undef ID

	return(mod);
error:
	DROP_MODULE(mod);
	return(NULL);
}
/*
 * vim: ts=3:sw=3:noet:
 */
#if 0
"""
#endif
