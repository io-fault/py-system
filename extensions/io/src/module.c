/**
	// IO implementation using kqueue or epoll.
	// See &.documentation.io.mechanics for more information.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

/* file descriptor transfers */
#include <sys/param.h>

#include <fault/libc.h>
#include <fault/python/environ.h>
#include <fault/python/injection.h>

#include <kcore.h>
#include <kports.h>

#include <endpoint.h>

#include "module.h"
#include "port.h"

struct KPortsAPI *KP = NULL;
struct EndpointAPI *EP = NULL;

/* Number of kevent structs to allocate when working with kevent(). */
#ifndef CONFIG_DEFAULT_ARRAY_SIZE
	#define CONFIG_DEFAULT_ARRAY_SIZE 16
#endif

#define errpf(...) fprintf(stderr, __VA_ARGS__)

#ifndef HAVE_STDINT_H
	/* relying on Python checks */
	#include <stdint.h>
#endif

/* posix errno macro detection */
#include <fault/posix/errno.h>

PyObj PyExc_TransitionViolation = NULL;

/**
	// Get the name of the errno.
*/
static const char *
errname(int err)
{
	switch(err)
	{
		#define XDEF(D, S) case D: return( #D );
			FAULT_POSIX_ERRNO_TABLE()
		#undef XDEF
		default:
			return "ENOTDEFINED";
		break;
	}

	return("<broken switch statement>");
}

#ifndef MIN
	#define MIN(x1,x2) ((x1) < (x2) ? (x1) : (x2))
#endif

enum polarity_t {
	p_output = -1,
	p_neutral = 0,
	p_input = 1
};
typedef enum polarity_t polarity_t;

char
freight_charcode(freight_t f)
{
	switch (f)
	{
		case f_void:
			return 'v';
		case f_events:
			return 'e'; /* Array */
		case f_octets:
			return 'o';
		case f_datagrams:
			return 'G';
		case f_sockets:
			return 'S';
		case f_ports:
			return 'P';
	}
	return '_';
}

const char *
freight_identifier(freight_t f)
{
	switch (f)
	{
		case f_void:
			return "void";
		case f_events:
			return "events";
		case f_octets:
			return "octets";
		case f_datagrams:
			return "datagrams";
		case f_sockets:
			return "sockets";
		case f_ports:
			return "ports";
	}
	return "unknown";
}

char *
ktype_string(ktype_t kt)
{
	switch (kt)
	{
		case kt_bad:
			return("bad");
		case kt_pipe:
			return("pipe");
		case kt_fifo:
			return("fifo");
		case kt_device:
			return("device");
		case kt_tty:
			return("tty");
		case kt_socket:
			return("socket");
		case kt_file:
			return("file");
		case kt_kqueue:
			return("kqueue");
		default:
			return("unknown");
	}
}

static int
inet6_from_pyint(void *out, PyObj ob)
{
	int r = -1;

	if (ob == Py_None)
		return(INADDR_ANY);

	if (Py_TYPE(ob) != &PyLong_Type)
	{
		PyObj lo = PyNumber_Long(ob);
		if (lo)
		{
			r = _PyLong_AsByteArray((PyLongObject *) lo, out, 128 / 8, 0, 0);
			Py_DECREF(lo);
		}
	}
	else
	{
		r = _PyLong_AsByteArray((PyLongObject *) ob, out, 128 / 8, 0, 0);
	}

	return(r);
}

static int
inet4_from_pyint(void *out, PyObj ob)
{
	int r = -1;

	if (ob == Py_None)
		return(INADDR_ANY);

	if (Py_TYPE(ob) != &PyLong_Type)
	{
		PyObj lo = PyNumber_Long(ob);
		if (lo)
		{
			r = _PyLong_AsByteArray((PyLongObject *) lo, out, 32 / 8, 0, 0);
			Py_DECREF(lo);
		}
	}
	else
	{
		r = _PyLong_AsByteArray((PyLongObject *) ob, out, 32 / 8, 0, 0);
	}

	return(r);
}

static aport_kind_t
sockaddr_port(any_addr_t *ss, struct aport_t *dst, size_t dstlen)
{
	switch (ss->ss_family)
	{
		#define A(AF) \
			case AF##_pf: { \
				AF##_casted(afdata, ss); \
				AF##_port(dst, dstlen, afdata); } \
				return(AF##_port_kind);
			ADDRESSING()
		#undef A
	}

	return(aport_kind_none);
}

/**
	// [ Parameters ]
	// /ss/
		// Source address structure.
	// /dst/
		// Destination memory buffer for the interface string
		// to be written to.
	// /dstlen/
		// Length of &dst string.

	// [ Return ]
	// The &ss parameter is the destination of the interface
	// described in &dst.
*/
static void
sockaddr_interface(any_addr_t *ss, char *dst, size_t dstlen)
{
	switch (ss->ss_family)
	{
		#define A(AF) \
			case AF##_pf: { \
				AF##_casted(afdata, ss); \
				AF##_str(dst, dstlen, afdata); \
			} break;
			ADDRESSING()
		#undef A
	}
}

#if ! FV_OPTIMAL() || F_TRACE()
	static void pchannel(Channel);
	static void pkevent(kevent_t *);
#endif

static kcall_t
kcall_id(char *str)
{
	/*
		// Naturally a hash is better, but
		// this is only used during Port.__new__,
		// which is rarely used.
	*/
	#define KC(x) if (strcmp(#x, str) == 0) return (kc_##x); else
		KCALLS()
		return(kc_INVALID);
	#undef KC
}

static const char *
kcall_identifier(kcall_t kc)
{
	switch (kc)
	{
		#define KC(x) case kc_##x: return(#x);
		KCALLS()
		#undef KC
	}

	return("INVALID");
}

#define PyErr_SetChannelTerminatedError(t) \
	PyErr_SetString(PyExc_TransitionViolation, "already terminated")
#define PyErr_SetChannelResourceError(t) \
	PyErr_SetString(PyExc_TransitionViolation, "resource already present")

static int
socket_receive_buffer(kport_t kp)
{
	int size = -1;
	socklen_t ssize = sizeof(size);
	getsockopt(kp, SOL_SOCKET, SO_RCVBUF, &size, &ssize);
	return(size);
}

static int
socket_send_buffer(kport_t kp)
{
	int size = -1;
	socklen_t ssize = sizeof(size);
	getsockopt(kp, SOL_SOCKET, SO_SNDBUF, &size, &ssize);
	return(size);
}

static PyObj
path(kport_t kp)
{
	#ifdef F_GETPATH
		char fp[PATH_MAX];

		if (fcntl(kp, F_GETPATH, fp) != -1)
		{
			return(PyBytes_FromString(fp));
		}
		else
		{
			/*
				// Ignore error; file path not available.
			*/
			errno = 0;
			Py_RETURN_NONE;
		}
	#else
		Py_RETURN_NONE;
	#endif
}

static PyObj
port_raised(PyObj self)
{
	Port p = (Port) self;

	if (p->error == 0)
	{
		Py_RETURN_NONE;
	}

	errno = p->error;
	PyErr_SetFromErrno(PyExc_OSError);
	errno = 0;

	return(NULL);
}

static PyObj
port_exception(PyObj self)
{
	Port p = (Port) self;
	PyObj exc, val, tb;

	if (p->error == 0)
	{
		Py_RETURN_NONE;
	}

	errno = p->error;
	PyErr_SetFromErrno(PyExc_OSError);
	errno = 0;

	PyErr_Fetch(&exc, &val, &tb);
	Py_XDECREF(exc);
	Py_XDECREF(tb);

	return(val);
}

static PyObj
port_leak(PyObj self)
{
	Port p = (Port) self;
	PyObj rob;

	rob = p->latches ? Py_True : Py_False;
	p->latches = 0;

	p->cause = kc_leak;

	Py_INCREF(rob);
	return(rob);
}

static PyObj
port_shatter(PyObj self)
{
	Port p = (Port) self;
	PyObj rob;

	rob = p->latches ? Py_True : Py_False;
	port_unlatch(p, 0);

	p->cause = kc_shatter;

	Py_INCREF(rob);
	return(rob);
}

/* METH_O, METH_VARARGS, METH_VARKEYWORDS, METH_NOARGS */
static PyMethodDef port_methods[] = {
	{"shatter",
		(PyCFunction) port_shatter, METH_NOARGS,
		PyDoc_STR(
			"Destroy the resource reference without triggering representation shutdowns such as (/unix/man/2)`shutdown` on sockets. "
			"Ports with Array attached Channels should never be shattered as it causes the event subscription to be lost. "
			"Subsequently, the Channel will remain in the Array ring until terminated by user code.\n\n"
	)},

	{"leak",
		(PyCFunction) port_leak, METH_NOARGS,
		PyDoc_STR(
			"Leak the kernel resource reference. Allows use of the file descriptor "
			"without fear of a subsequent shutdown or close from a Channel.\n\n"
	)},

	{"raised",
		(PyCFunction) port_raised, METH_NOARGS,
		PyDoc_STR(
			"Raise the &OSError corresponding to the noted error."
	)},

	{"exception",
		(PyCFunction) port_exception, METH_NOARGS,
		PyDoc_STR(
			"Return the &OSError corresponding to the operating system error.\n"
			"\n[Effects]\n"
			"/(&Exception)`Return`/\n"
			"\tThe Python exception that would be raised by &raised.\n"
			"\n"
	)},

	{NULL,}
};

static PyMemberDef port_members[] = {
	{"id", T_KPORT, offsetof(struct Port, point), READONLY,
		PyDoc_STR(
			"The identifier of the port used to communicate with the kernel."
	)},
	{"error_code", T_KERROR, offsetof(struct Port, error), READONLY,
		PyDoc_STR(
			"The error code associated with the Port."
	)},

	/*
		// Some aliases to lend toward convention.
	*/

	{"fileno", T_KPORT, offsetof(struct Port, point), READONLY,
		PyDoc_STR("Alias to &id. Included for convention.")},
	{"errno", T_KERROR, offsetof(struct Port, error), READONLY,
		PyDoc_STR("Alias to &error_code. Included for convention.")},

	{"_call_id", T_UBYTE, offsetof(struct Port, cause), READONLY,
		PyDoc_STR(
		"The internal identifier for the &call string."
	)},
	{"_freight_id", T_INT, offsetof(struct Port, freight), READONLY,
		PyDoc_STR(
		"The internal identifier for the &freight string."
	)},

	{NULL,},
};

static PyObj
port_get_freight(PyObj self, void *_)
{
	Port p = (Port) self;
	return(PyUnicode_FromString(freight_identifier(p->freight)));
}

static PyObj
port_get_call(PyObj self, void *_)
{
	Port p = (Port) self;

	if (!p->error)
		Py_RETURN_NONE;

	return(PyUnicode_FromString(kcall_identifier(p->cause)));
}

static PyObj
port_get_error_name(PyObj self, void *_)
{
	Port p = (Port) self;
	return(PyUnicode_FromString(errname(p->error)));
}

static PyObj
port_get_error_description(PyObj self, void *_)
{
	Port p = (Port) self;
	const char *errstr;

	if (p->error == ENONE)
		return PyUnicode_FromString("No error occurred.");

	errstr = strerror((int) p->error);

	if (errstr == NULL)
	{
		Py_RETURN_NONE;
	}

	return(PyUnicode_FromString(errstr));
}

static PyObj
port_get_posix_description(PyObj self, void *_)
{
	Port p = (Port) self;
	const char *str;

	#define XDEF(sym, estr) case sym: str = estr; break;
		switch (p->error)
		{
			FAULT_POSIX_ERRNO_TABLE()

			default:
				str = "Error code not recognized.";
			break;
		}
	#undef XDEF

	return(PyUnicode_FromString(str));
}

static PyGetSetDef port_getset[] = {
	{"call", port_get_call, NULL,
		PyDoc_STR(
			"The system library call or system.io call performed that caused the error associated with the Port.\n"
	)},

	{"error_name", port_get_error_name, NULL,
		PyDoc_STR(
			"The macro name of the errno. Equivalent to `errno.errorcode[port.errno]`.\n"
	)},

	{"freight", port_get_freight, NULL,
		PyDoc_STR(
			"What was being transferred by the Channel.\n"
	)},

	{"error_description", port_get_error_description, NULL,
		PyDoc_STR(
			"A string describing the errno using the (/unix/man/2)`strerror` function.\n"
			"This may be equivalent to the &strposix attribute."
	)},

	{"_posix_description", port_get_posix_description, NULL,
		PyDoc_STR(
			"A string describing the errno using the POSIX descriptions built into Traffic.\n"
	)},

	{NULL,},
};

/**
	// String representation suitable for text based displays.
*/
static PyObj
port_str(PyObj self)
{
	Port p = (Port) self;
	char *errstr;
	PyObj rob;

	if (p->error)
	{
		errstr = strerror(p->error);
		rob = PyUnicode_FromFormat(
			"Port (%d) transferring %s performed \"%s\" resulting in %s(%d) [%s]",
			p->point,
			freight_identifier(p->freight),
			kcall_identifier(p->cause),
			errname(p->error), p->error,
			errstr ? errstr : ""
		);
	}
	else
	{
		rob = PyUnicode_FromFormat(
			"Port %d (%s) transferring %s",
			p->point, "", freight_identifier(p->freight)
		);
	}

	return(rob);
}

static PyObj
port_repr(PyObj self)
{
	Port p = (Port) self;
	PyObj rob;
	const char *kcid = kcall_identifier(p->cause);
	const char *frid = freight_identifier(p->freight);

	rob = PyUnicode_FromFormat(
		"%s(id = %d, error_code = %d, cause = '%s', freight = '%s')",
		Py_TYPE(self)->tp_name, p->point, p->error, kcid, frid
	);

	return(rob);
}

static PyObj
port_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"id", "call", "error_code", "freight", NULL};
	PyObj rob;
	Port p;
	int err = -1;
	kport_t kid = -1;
	char *freight = "unknown";
	char *kcstr = "none";

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|isis", kwlist,
		&kid, &kcstr, &err, &freight))
		return(NULL);

	rob = PyAllocate(subtype);
	if (rob == NULL)
		return(NULL);

	p = (Port) rob;

	p->error = err;
	p->point = kid;
	p->cause = kcall_id(kcstr);
	p->freight = f_void;
	p->latches = 0;

	return(rob);
}

PyDoc_STRVAR(port_doc,
"Port(id = -1, error_code = 0, call = 'none', freight = 'void')\n\n");

static void
port_dealloc(PyObj self)
{
	Port p = (Port) self;

	/**
		// Array instances hold a reference to a point until it is
		// explicitly closed. At that point, it is detached from the ring,
		// and Array's reference is released.
	*/
	if (p->latches && p->point != kp_invalid && p->cause != kc_leak)
	{
		#if PY_MAJOR_VERSION > 2
		PyErr_WarnFormat(PyExc_ResourceWarning, 0, "port was latched at deallocation");
		#endif
	}

	Py_TYPE(self)->tp_free(self);
}

PyTypeObject
PortType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Port"),   /* tp_name */
	sizeof(struct Port),          /* tp_basicsize */
	0,                            /* tp_itemsize */
	port_dealloc,                 /* tp_dealloc */
	NULL,                         /* tp_print */
	NULL,                         /* tp_getattr */
	NULL,                         /* tp_setattr */
	NULL,                         /* tp_compare */
	port_repr,                    /* tp_repr */
	NULL,                         /* tp_as_number */
	NULL,                         /* tp_as_sequence */
	NULL,                         /* tp_as_mapping */
	NULL,                         /* tp_hash */
	NULL,                         /* tp_call */
	port_str,                     /* tp_str */
	NULL,                         /* tp_getattro */
	NULL,                         /* tp_setattro */
	NULL,                         /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,           /* tp_flags */
	port_doc,                     /* tp_doc */
	NULL,                         /* tp_traverse */
	NULL,                         /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	NULL,                         /* tp_iter */
	NULL,                         /* tp_iternext */
	port_methods,                 /* tp_methods */
	port_members,                 /* tp_members */
	port_getset,                  /* tp_getset */
	NULL,                         /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	port_new,                     /* tp_new */
};

static PyMethodDef endpoint_methods[] = {
	{NULL,}
};

#include "channel.h"

/**
	// Datagrams (struct)&Channel Structure.
*/
struct Datagrams {
	Channel_HEAD
};

#define INIT_CHANNEL(t, J) do { \
	Channel_SetArray(t, J); \
	Channel_SetNextTransfer(t, NULL); \
	Channel_SetResource(t, NULL); \
	Channel_SetLink(t, NULL); \
	Channel_ClearWindow(t); \
	Channel_State(t) = 0; \
	Channel_SetDelta(t, 0); \
	Channel_SetEvents(t, 0); \
} while(0)

#define INIT_INPUT_CHANNEL(t, J) do { \
	INIT_CHANNEL(t, J); \
	Channel_SetControl(t, ctl_polarity); \
} while(0)

#define INIT_OUTPUT_CHANNEL(t, J) do { \
	INIT_CHANNEL(t, J); \
	Channel_NulControl(t, ctl_polarity); \
} while(0)

#define Array_Cycling(J)               (J->lltransfer != NULL)
#define Array_GetChannelCount(J)       ((J->choice.array.nchannels))
#define Array_ResetChannelCount(J)     ((J->choice.array.nchannels) = 0)
#define Array_IncrementChannelCount(J) (++ Array_GetChannelCount(J))
#define Array_DecrementChannelCount(t) (-- Array_GetChannelCount(t))

static int array_fall(Array, int);

/* Append to the end of the doubly linked list; requires GIL. */
#define Channel_EnqueueDelta(t) do { \
	Array J = (Channel_GetArray(t)); \
	if (Channel_GetDelta(t) != 0 && ((Array)(t)) != J) { \
		CHANNEL_RELOCATE_SEGMENT_BEFORE(J, (t), (t)); \
		array_fall(J, 0); \
	} \
} while(0)

/**
	// Transfer Linked List management.

	// The transfer linked list is the list that manages the channels
	// that have significant state changes: transfer occurred--with exhaustion--or termination.
*/
#define Channel_GetNextTransfer(t)       ((t)->lltransfer)
#define Channel_IsTransfer(t)            (Channel_GetNextTransfer(t) != NULL)
#define Channel_SetNextTransfer(t, sett) (Channel_GetNextTransfer(t) = sett)

#define Array_AddTransfer(J, t) \
	do { \
		if (Channel_GetNextTransfer(t) == NULL) { \
			Channel_SetNextTransfer(t, Channel_GetNextTransfer(J)); \
			Channel_SetNextTransfer(J, t); \
		} \
	} while(0)

/**
	// Array only holds one reference to channels no matter
	// how often they're referenced.
*/
#define CHANNEL_JOIN(PREV, NEXT) \
	do { \
		PREV->next = (Channel) NEXT; \
		NEXT->prev = (Channel) PREV; \
	} while (0)

/**
	// Extends ring from behind. (Relative to TARGET, usually a Array instance)
*/
#define CHANNEL_ATTACH_SEGMENT_BEFORE(TARGET, FIRST, LAST) do { \
	Channel T_prev = TARGET->prev; \
	FIRST->prev = T_prev; \
	LAST->next = (Channel) TARGET; \
	TARGET->prev = LAST; \
	T_prev->next = FIRST; \
} while (0)

/**
	// Extends ring from front.
*/
#define CHANNEL_ATTACH_SEGMENT_AFTER(TARGET, FIRST, LAST) do { \
	FIRST->prev = (Channel) TARGET; \
	LAST->next = (Channel) TARGET->next; \
	TARGET->next->prev = (Channel) LAST; \
	TARGET->next = (Channel) FIRST; \
} while (0)

#define CHANNEL_RELOCATE_SEGMENT_AFTER(TARGET, FIRST, LAST) do { \
	CHANNEL_DETACH_SEGMENT(FIRST, LAST); \
	CHANNEL_ATTACH_SEGMENT_AFTER(TARGET, FIRST, LAST); \
} while (0)

#define CHANNEL_DETACH_SEGMENT(FIRST, LAST) do { \
	Channel f_prev = FIRST->prev, l_next = LAST->next; \
	f_prev->next = (Channel) l_next; \
	l_next->prev = (Channel) f_prev; \
} while(0)

#define CHANNEL_RELOCATE_SEGMENT_BEFORE(TARGET, FIRST, LAST) do { \
	CHANNEL_DETACH_SEGMENT(FIRST, LAST); \
	CHANNEL_ATTACH_SEGMENT_BEFORE(TARGET, FIRST, LAST); \
} while (0)

#define CHANNEL_DETACH(CHANNEL) do { \
	CHANNEL_DETACH_SEGMENT(CHANNEL, CHANNEL); \
	CHANNEL->prev = NULL; CHANNEL->next = NULL; \
} while(0)

#define CHANNEL_ATTACH_BEFORE(TARGET, CHANNEL) \
	CHANNEL_ATTACH_SEGMENT_BEFORE(TARGET, CHANNEL, CHANNEL)
#define CHANNEL_ATTACH_AFTER(TARGET, CHANNEL) \
	CHANNEL_ATTACH_SEGMENT_AFTER(TARGET, CHANNEL, CHANNEL)
#define CHANNEL_ATTACH(CHANNEL) \
	CHANNEL_ATTACH_BEFORE(Channel_GetArray(CHANNEL), CHANNEL)

#define Channel_GetResourceArray(TYP, T) ((TYP *)(Channel_GetResourceBuffer(T) + (Channel_GetWindowStop(T) * sizeof(TYP))))
#define Channel_GetRemainder(TYP, T) ((Channel_GetResourceSize(T) / sizeof(TYP)) - Channel_GetWindowStop(T))

/**
	// Requires GIL.
*/
static void
Channel_ReleaseResource(Channel t)
{
	/**
		// Free any Python resources associated with the channel.
	*/
	if (Channel_HasResource(t))
	{
		PyBuffer_Release(Channel_GetResourceView(t));
		Py_DECREF(Channel_GetResource(t));
		Channel_SetResource(t, NULL);
		Channel_ClearWindow(t);
	}
}

/**
	// Requires GIL. Decrements the link reference and sets the field to &NULL.
*/
static void
Channel_ReleaseLink(Channel t)
{
	/**
		// Free any Python resources associated with the channel.
	*/
	if (Channel_GetLink(t))
	{
		Py_DECREF(Channel_GetLink(t));
		Channel_SetLink(t, NULL);
	}
}

#ifdef EVMECH_EPOLL
static void
kfilter_cancel(Channel t, kevent_t *kev)
{
	const int filters[2] = {EPOLLIN, EPOLLOUT};
	Port p;
	struct Port wp;

	kev->data.ptr = t;
	kev->events = EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET
		| filters[!Channel_GetControl(t, ctl_polarity)];

	if (kev->events & EPOLLOUT)
	{
		wp.point = Channel_GetArray(t)->choice.array.wfd;
		p = &wp;
	}
	else
		p = Channel_GetArrayPort(t);

	port_epoll_ctl(p, EPOLL_CTL_DEL, Channel_GetPort(t), kev);
}

static void
kfilter_attach(Channel t, kevent_t *kev)
{
	const int filters[2] = {EPOLLIN, EPOLLOUT};
	Port p;
	struct Port wp;

	kev->data.ptr = t;
	kev->events = EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET
		| filters[!Channel_GetControl(t, ctl_polarity)];

	if (kev->events & EPOLLOUT)
	{
		wp.point = Channel_GetArray(t)->choice.array.wfd;
		p = &wp;
	}
	else
		p = Channel_GetArrayPort(t);

	port_epoll_ctl(p, EPOLL_CTL_ADD, Channel_GetPort(t), kev);
}
#else
#if ! FV_OPTIMAL() || F_TRACE()
static void
pkevent(kevent_t *kev)
{
	const char *fname;
	switch (kev->filter)
	{

		#define FILTER(B) case B: fname = #B; break;
			KQ_FILTERS()
		#undef FILTER

		default:
			fname = "unknown filter";
		break;
	}
	errpf(
		"%s (%d), fflags: %d,"
		" ident: %p, data: %p, udata: %p, flags:"
		" %s%s%s%s%s%s%s%s%s%s\n",
		fname, kev->filter, kev->fflags,
		(void *) kev->ident,
		(void *) kev->data, (void *) kev->udata,
		#define FLAG(FLG) (kev->flags & FLG) ? (#FLG "|") : "",
			KQ_FLAGS()
		#undef KQF
		"" /* termination for the xmacros */
	);
	return;
}

/**
	// Print channel structure to standard error.
	// Used for tracing operations during debugging.
*/
static void
pchannel(Channel t)
{
	struct sockaddr_storage ss;
	socklen_t sslen = sizeof(ss);
	const char *callstr;
	char buf[512];
	struct aport_t port;
	port.data.numeric2 = 0;

	if (Channel_PortLatched(t))
	{
		if (Channel_Sends(t))
		{
			getpeername(Channel_GetKPoint(t), (if_addr_ref_t) &ss, &sslen);
		}
		else
		{
			getsockname(Channel_GetKPoint(t), (if_addr_ref_t) &ss, &sslen);
		}
	}
	else
	{
		/* skip sockaddr resolution */
		errno = 1;
	}

	if (errno == 0)
	{
		sockaddr_interface(&ss, buf, sizeof(buf));
		switch (sockaddr_port(&ss, &port, sizeof(port)))
		{
			case aport_kind_numeric2:
				snprintf(port.data.filename, sizeof(port.data.filename), "%d", port.data.numeric2);
			break;
			default:
			break;
		}
	}
	else
	{
		strcpy(buf, "noaddr");
	}
	errno = 0;

	errpf(
		"%s[%d] %s:%s, "
		"errno(%s/%d)[%s], "
		"state:%s%s%s%s%s%s%s, "
		"events:%s%s%s, "
		"ktype:%s {refcnt:%d}"
		"\n",
		Py_TYPE(t)->tp_name,
		Channel_GetKPoint(t),
		buf, port.data.filename,
		kcall_identifier(Channel_GetKCall(t)),
		Channel_GetKError(t),
		Channel_GetKError(t) != 0 ? strerror(Channel_GetKError(t)) : "",

		Channel_GetControl(t, ctl_polarity)  ? "IRECEIVES" : "ISENDS",
		Channel_IQualified(t, teq_terminate) ? "|ITerm" : "",
		Channel_IQualified(t, teq_transfer)  ? "|ITransfer" : "",
		Channel_XQualified(t, teq_terminate) ? "|XTerm" : "",
		Channel_XQualified(t, teq_transfer)  ? "|XTransfer" : "",
		Channel_GetControl(t, ctl_connect)   ? "|ctl_connect" : "",
		Channel_GetControl(t, ctl_force)     ? "|ctl_force" : "",
		Channel_GetControl(t, ctl_requeue)   ? "|ctl_requeue" : "",
		Channel_HasEvent(t, tev_terminate)   ? "|terminate" : "",
		Channel_HasEvent(t, tev_transfer)    ? "|transfer" : "",
		ktype_string(Channel_GetKType(t)),
		(int) Py_REFCNT(t)
	);
}

static void
pkevents(const char *where, Array J)
{
	int i;
	errpf("[%s]\n", where);
	for (i = 0; i < Channel_GetWindowStart(J); ++i)
	{
		pkevent(&(Array_GetKEvents(J)[i]));
		pchannel((Channel) (Array_GetKEvents(J)[i]).udata);
	}
	errpf("\n");
}
#endif

static void
kfilter_cancel(Channel t, kevent_t *kev)
{
	const int filters[2] = {EVFILT_READ, EVFILT_WRITE};

	kev->filter = filters[!Channel_GetControl(t, ctl_polarity)];
	kev->ident = Channel_GetKPoint(t);
	kev->flags = EV_CLEAR | EV_DELETE | EV_RECEIPT;
	kev->fflags = 0;
	kev->data = 0;
	kev->udata = t;
}

static void
kfilter_attach(Channel t, kevent_t *kev)
{
	const int filters[2] = {EVFILT_READ, EVFILT_WRITE};

	kev->filter = filters[!Channel_GetControl(t, ctl_polarity)];
	kev->ident = Channel_GetKPoint(t);
	kev->flags = EV_CLEAR | EV_ADD | EV_RECEIPT;
	kev->fflags = 0;
	kev->data = 0;
	kev->udata = t;
}
#endif

/**
	// &PyTypeObject extension structure for configuring the I/O callbacks to use.
*/
struct ChannelInterface
ChannelTIF = {
	{NULL, NULL},
	f_void, 0,
};

/**
	// Iterator created by &.kernel.Array.transfer().

	// Given that this is restricted to iteration,
	// the type is *not* exposed on the module.
*/
struct jxi {
	PyObject_HEAD

	/**
		// Subject &Array producing events.
	*/
	Array J;

	/**
		// Position in transfer linked list.
	*/
	Channel t;
};

/**
	// Iterator protocol next implementation.
*/
static PyObj
jxi_next(PyObj self)
{
	Channel this;
	struct jxi *i = (struct jxi *) self;

	if (i->t == NULL)
		return(NULL);

	if (!Channel_InCycle(i->t))
	{
		PyErr_SetString(PyExc_RuntimeError,
			"array transfer iterator used outside of cycle");
		return(NULL);
	}

	for (this = i->t; this != (Channel) i->J && Channel_GetEvents(this) == 0; this = Channel_GetNextTransfer(this));

	if (this == (Channel) i->J)
	{
		Py_DECREF(i->t);
		Py_DECREF(i->J);
		i->t = NULL;
		i->J = NULL;

		return(NULL);
	}
	else
	{
		i->t = Channel_GetNextTransfer(this);
		Py_INCREF(i->t);
	}

	return((PyObj) this);
}

static void
jxi_dealloc(PyObj self)
{
	struct jxi *i = (struct jxi *) self;
	Py_XDECREF(i->J);
	Py_XDECREF(i->t);
	i->J = NULL;
	i->t = NULL;
	Py_TYPE(self)->tp_free(self);
}

static PyObj
jxi_iter(PyObj self)
{
	Py_INCREF(self);
	return(self);
}

PyDoc_STRVAR(jxi_doc, "iterator producing Channels with events to be processed");
PyTypeObject jxi_type = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("jxi"),   /* tp_name */
	sizeof(struct jxi),          /* tp_basicsize */
	0,                           /* tp_itemsize */
	jxi_dealloc,                 /* tp_dealloc */
	NULL,                        /* tp_print */
	NULL,                        /* tp_getattr */
	NULL,                        /* tp_setattr */
	NULL,                        /* tp_compare */
	NULL,                        /* tp_repr */
	NULL,                        /* tp_as_number */
	NULL,                        /* tp_as_sequence */
	NULL,                        /* tp_as_mapping */
	NULL,                        /* tp_hash */
	NULL,                        /* tp_call */
	NULL,                        /* tp_str */
	NULL,                        /* tp_getattro */
	NULL,                        /* tp_setattro */
	NULL,                        /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,          /* tp_flags */
	jxi_doc,                     /* tp_doc */
	NULL,                        /* tp_traverse */
	NULL,                        /* tp_clear */
	NULL,                        /* tp_richcompare */
	0,                           /* tp_weaklistoffset */
	jxi_iter,                    /* tp_iter */
	jxi_next,                    /* tp_iternext */
	NULL,
};

static PyObj
new_jxi(Array J, int polarity)
{
	struct jxi *i;

	i = (struct jxi *) PyAllocate(&jxi_type);
	if (i == NULL)
		return(NULL);

	i->J = J;
	i->t = Channel_GetNextTransfer(J);
	Py_XINCREF(i->t);
	Py_INCREF(i->J);

	return((PyObj) i);
}

static char
channel_can_acquire(Channel t)
{
	/*
		// This should be called after receiving an exhaust event, which
		// removes this internal flag.
	*/
	if (Channel_IQualified(t, teq_transfer))
	{
		/*
			// This needs to error out as the flow may be using the
			// channel's resource at this particular moment.
		*/
		PyErr_SetChannelResourceError(t);
		return(0);
	}

	return(1);
}

/**
	// Acquire a resource for faciliting a transfer. Qualifies the Channel for transfers.

	// The given &resource object is set on the &Channel and the memory buffer is filled
	// out based on the direction of the channel. If the Channel has been acquired
	// by a &Array, the Channel will be marked and enqueued for a subsequent transfer
	// attempt. Otherwise, the Channel is qualified for transfer allowing the subsequent
	// &array_acquire to ready a transfer.
*/
static PyObj
channel_acquire(PyObj self, PyObj resource)
{
	Channel t = (Channel) self;

	if (Channel_Terminating(t))
	{
		/*
			// Ignore resource acquisitions if terminating.
			// In cases where Array is running in a parallel loop,
			// it is possible for a terminate event to follow exhaustion.

			// Essentially, raising an exception here would a race condition
			// for proper resource acquisition chains.
		*/
		Py_RETURN_NONE;
	}

	/*
		// Raise ResourceError; user isn't paying attention to exhaust events.
	*/
	if (!channel_can_acquire(t))
	{
		/*
			// Exhaust events are the only safe way that we can invalidate
			// resources without introducing addtional locks.
		*/
		return(NULL);
	}

	/*
		// REQUIRES GIL
	*/

	Channel_ReleaseResource(t);
	Py_INCREF(resource);
	Channel_SetResource(t, resource);

	if (PyObject_GetBuffer(resource, Channel_GetResourceView(t), Channel_Receives(t) ? PyBUF_WRITABLE : 0))
	{
		Channel_SetResource(t, NULL);
		Py_DECREF(resource);
		return(NULL);
	}

	Channel_ClearWindow(t);

	if (Channel_GetArray(t) != NULL)
	{
		Channel_DQualify(t, teq_transfer);
		Channel_EnqueueDelta(t); /* REQUIRES GIL */
	}
	else
	{
		/*
			// Not acquired by a array.
			// Directly apply the event qualification and
			// the array will enqueue it when acquired.
		*/
		Channel_IQualify(t, teq_transfer);
	}

	Py_INCREF(self);
	return(self);
}

static PyObj
channel_force(PyObj self)
{
	Channel t = (Channel) self;

	Channel_DControl(t, ctl_force);

	if (Channel_Attached(t) && Channel_IQualified(t, teq_transfer))
	{
		/* No Array? Do not enqueue, but allow the effect */
		/* to occur when it is later acquired.               */
		Channel_EnqueueDelta(t); /* REQUIRES GIL */
	}

	Py_RETURN_NONE;
}

/* Raw buffer interface */
static PyObj
channel_slice(PyObj self)
{
	Channel t = (Channel) self;

	if (!Channel_HasResource(t))
		Py_RETURN_NONE;

	return(_PySlice_FromIndices(Channel_GetWindowStart(t), Channel_GetWindowStop(t)));
}

static PyObj
channel_transfer(PyObj self)
{
	Channel t = (Channel) self;
	int unit = Channel_GetInterface(t)->ti_unit;
	PyObj rob;
	PyObj s;

	if (!Channel_HasResource(t)
		|| !Channel_HasEvent(t, tev_transfer))
	{
		Py_RETURN_NONE;
	}

	s = _PySlice_FromIndices(Channel_GetWindowStart(t) / unit, Channel_GetWindowStop(t) / unit);
	if (s == NULL) return(NULL);

	rob = PyObject_GetItem(Channel_GetResource(t), s);
	Py_DECREF(s);

	return(rob);
}

static PyObj
channel_sizeof_transfer(PyObj self)
{
	uint32_t size;
	Channel t = (Channel) self;

	if (!Channel_HasResource(t) || !Channel_HasEvent(t, tev_transfer))
		return(PyLong_FromLong(0));

	size = Channel_GetWindowStop(t) - Channel_GetWindowStart(t);

	return(PyLong_FromUnsignedLong(size));
}

static PyObj
channel_terminate(PyObj self)
{
	Channel t = (Channel) self;

	if (!Channel_Attached(t))
	{
		/*
			// Has GIL, not in Traffic.
			// Array instances cannot acquire Channels without the GIL.

			// Running terminate directly is safe.
		*/
		if (!Channel_Terminated(t))
		{
			Channel_IQualify(t, teq_terminate);
			Channel_ReleaseResource(t);
			Channel_ReleaseLink(t);
			port_unlatch(Channel_GetPort(t), Channel_Polarity(t)); /* Kernel Resources (file descriptor) */
		}
	}
	else if (!Channel_Terminating(t))
	{
		/*
			// Acquired by a Array instance, that Array
			// instance is responsible for performing termination.

			// Has GIL, so place teq_terminate event qualification on the delta.
		*/
		Channel_DQualify(t, teq_terminate);

		if ((PyObj) Py_TYPE(t) == arraytype)
		{
			array_fall((Array) t, 0);
		}
		else
		{
			Channel_EnqueueDelta(t); /* REQUIRES GIL */
		}
	}

	Py_RETURN_NONE;
}

static PyObj
channel_resize_exoresource(PyObj self, PyObj args)
{
	Py_RETURN_NONE;
}

static PyObj
channel_endpoint(PyObj self)
{
	Channel t = (Channel) self;
	kport_t kp = Channel_GetKPoint(t);
	any_addr_t addr;
	int r;

	socklen_t addrlen = sizeof(addr);

	if (!Channel_PortLatched(t))
		Py_RETURN_NONE;

	bzero(&addr, addrlen);
	addr.ss_family = AF_UNSPEC;

	if (Channel_Polarity(t) == p_output)
	{
		/*
			// Sends, get peer.
		*/
		r = getpeername(kp, (if_addr_ref_t) &(addr), &addrlen);
		if (r)
		{
			errno = 0;
			goto none;
		}
	}
	else
	{
		/*
			// It is the endpoint, get sockname.
		*/
		r = getsockname(kp, (if_addr_ref_t) &(addr), &addrlen);
		if (r)
		{
			errno = 0;
			goto none;
		}
	}

	if (addr.ss_family == AF_UNSPEC)
		goto none;

	if (addr.ss_family == AF_LOCAL)
	{
		/*
			// Check for anonymous sockets. (socketpair)
		*/
		local_addr_t *localaddr = (local_addr_t *) &addr;

		/*
			// Return the peereid if the remote is NULL/empty.
		*/
		if (local_addr_field(localaddr)[0] == 0)
		{
			PyObj ob, rob;
			uid_t uid = -1;
			gid_t gid = -1;

			if (getpeereid(kp, &uid, &gid))
			{
				errno = 0;
				goto none;
			}

			PYTHON_RECEPTACLE("new_tuple", &rob, PyTuple_New, 2);
			if (rob == NULL)
				return(NULL);

			PYTHON_RECEPTACLE("uid_long", &ob, PyLong_FromLong, uid);
			if (ob == NULL)
			{
				Py_DECREF(rob);
				return(NULL);
			}
			PyTuple_SET_ITEM(rob, 0, ob);

			PYTHON_RECEPTACLE("gid_long", &ob, PyLong_FromLong, gid);
			if (ob == NULL)
			{
				Py_DECREF(rob);
				return(NULL);
			}
			PyTuple_SET_ITEM(rob, 1, ob);

			return(rob);
		}
	}

	return((PyObj) EP->create(0, 0, (if_addr_ref_t) &addr, addrlen));

	none:
	{
		Py_RETURN_NONE;
	}
}

static PyMethodDef
channel_methods[] = {
	{"endpoint", (PyCFunction) channel_endpoint, METH_NOARGS,
		PyDoc_STR(
			"Construct an Endpoint object from the Channel describing the known destination of the channel, the end-point.\n"
			"For output channels, the endpoint will be the remote host. For input channels, the endpoint will be "
			"the local interface and port."
			"\n\n"
			"[Effect]\n"
			"/(&Endpoint)`Return`/\n\t"
			"A new Endpoint instance.\n"
			"\n"
		)
	},

	{"acquire",
		(PyCFunction) channel_acquire, METH_O,
		PyDoc_STR(
			"Acquire a resource for facilitating transfers. The `resource` type depends on\n"
			"the Channel subclass, but it is *normally* an object supporting the buffer interface.\n"
			"The particular Channel type should document the kind of object it expects."
			"\n\n"
			"[Parameters]\n"
			"/(&object)`resource`/\n"
			"\tThe resource to use facitate transfers.\n"
			"\n\n"
			"[Effects]\n"
			"/(&Channel)`Return`/"
			"\tThe Channel instance acquiring the resource.\n"
			"\n"
		)
	},

	{"resize_exoresource",
		(PyCFunction) channel_resize_exoresource, METH_VARARGS,
		PyDoc_STR(
			"Resize the related exoresource.\n"
			"[Parameters]\n"
			"/(&int)`new_size`/\n"
			"\tThe size, relative or absolute, of the kernel resource that should be used for the Channel.\n"
		)
	},

	{"force",
		(PyCFunction) channel_force, METH_NOARGS,
		PyDoc_STR(
			"Force the channel to perform a transfer.\n"
			"This causes an empty transfer event to occur.\n"
		)
	},

	{"transfer", (PyCFunction) channel_transfer, METH_NOARGS,
		PyDoc_STR(
			"This returns the slice of the resource that was transferred iff a transfer occurred.\n"
			"It is essentially `channel.resource[channel.slice()]`.\n"
			"\n"
			"[Effects]\n"
			"/(&object)`Return`/\n"
			"\tThe transferred data. Usually a &memoryview.\n"
			"\n"
		)
	},

	{"slice", (PyCFunction) channel_slice, METH_NOARGS,
		PyDoc_STR(
			"The slice method is always available. In cases where the channel is not in a cycle, a zero-distance slice "
			"will be returned desribing the current position in the resource's buffer.\n"
			"[Effects]\n"
			"/(&slice)`Return`/\n"
			"\tA slice specifying the portion of the resource that was transferred.\n"
		)
	},

	{"sizeof_transfer", (PyCFunction) channel_sizeof_transfer, METH_NOARGS,
		PyDoc_STR(
			"Get the size of the current transfer; `0` if there is no transfer.\n"
			"\n"
			"[Effects]\n"
			"/(&int)`Return`/\n"
			"\tThe number of units transferred.\n"
			"\n"
		)
	},

	{"terminate",
		(PyCFunction) channel_terminate, METH_NOARGS,
		PyDoc_STR(
			"Terminate the Channel permanently causing events to subside. Eventually, \n"
			"resources being held by the Tranist will be released.\n"
			"[Effects]\n"
			"/(&Channel)`Return`/\n"
			"\tThe channel being terminated.\n"
		)
	},

	{NULL,},
};

static PyMemberDef
channel_members[] = {
	{"array",
		T_OBJECT, offsetof(struct Channel, array), READONLY,
		PyDoc_STR(
			"The &Array instance that the Channel has been acquired by.\n"
			"`None` if the Channel has not been acquired by a Array instance."
		)
	},

	{"port",
		T_OBJECT, offsetof(struct Channel, port), READONLY,
		PyDoc_STR(
			"The &Port instance that the Channel uses to communicate with the kernel.\n"
			"This object is always present on the Channel."
		)
	},

	{"link",
		T_OBJECT, offsetof(struct Channel, link), 0,
		PyDoc_STR(
			"User storage slot for attaching data for adapter callback mechanisms."
		)
	},

	/*
		// Internal state access.
	*/
	#if FV_INJECTIONS()
		{"_state", T_UBYTE, offsetof(struct Channel, state), READONLY,
			PyDoc_STR("bit map defining the internal and external state of the channel")},
		{"_delta", T_UBYTE, offsetof(struct Channel, delta), READONLY,
			PyDoc_STR("bit map defining the internal state changes")},
		{"_event", T_UBYTE, offsetof(struct Channel, events), READONLY,
			PyDoc_STR("bit map of events that occurred this cycle")},
	#endif

	{NULL,},
};

static void
channel_dealloc(PyObj self)
{
	Channel t = (Channel) self;

	#if F_TRACE(dealloc)
		errpf("DEALLOC: %p %s\n", self, Py_TYPE(self)->tp_name);
	#endif

	/*
		// Array instances hold a reference to a Channel until it is
		// removed from the ring.
		// Channels hold their reference to the array until..now:
	*/
	Py_XDECREF(Channel_GetArray(t));
	Channel_SetArray(t, NULL);

	Py_DECREF(Channel_GetPort(t)); /* Alloc and init ports *before* using Channels. */
	Channel_SetPort(t, NULL);

	Py_XDECREF(Channel_GetLink(t)); /* Alloc and init ports *before* using Channels. */
	Channel_SetLink(t, NULL);

	Py_TYPE(self)->tp_free(self);
}

static PyObj
channel_get_polarity(PyObj self, void *_)
{
	Channel t = (Channel) self;

	if (!Channel_GetControl(t, ctl_polarity))
		return(PyLong_FromLong(-1));
	else
		return(PyLong_FromLong(1));

	Py_RETURN_NONE;
}

static PyObj
channel_get_terminated(PyObj self, void *_)
{
	Channel t = (Channel) self;
	PyObj rob;

	if (Channel_Terminating(t))
		rob = Py_True;
	else
		rob = Py_False;

	Py_INCREF(rob);
	return(rob);
}

static PyObj
channel_get_exhausted(PyObj self, void *_)
{
	Channel t = (Channel) self;

	if (Channel_Terminating(t))
	{
		/*
			// Don't indicate that a resource can be acquired.
		*/
		Py_INCREF(Py_False);
		return(Py_False);
	}

	/*
		// This should be called after receiving an exhaust event, which
		// removes this internal flag.
	*/
	if (Channel_IQualified(t, teq_transfer) || Channel_DQualified(t, teq_transfer))
	{
		/*
			// This needs to error out as the flow may be using the
			// channel's resource at this particular moment.
		*/
		Py_INCREF(Py_False);
		return(Py_False);
	}

	Py_INCREF(Py_True);
	return(Py_True);
}

static PyObj
channel_get_resource(PyObj self, void *_)
{
	Channel t = (Channel) self;
	PyObj r;

	r = Channel_GetResource(t);
	if (r == NULL)
		r = Py_None;

	Py_INCREF(r);
	return(r);
}

#if FV_INJECTIONS()
	static PyObj
	channel_get_xtransfer(PyObj self, void *_)
	{
		Channel t = (Channel) self;
		PyObj rob;

		if (Channel_XQualified(t, teq_transfer))
			rob = Py_True;
		else
			rob = Py_False;

		Py_INCREF(rob);
		return(rob);
	}

	static PyObj
	channel_get_itransfer(PyObj self, void *_)
	{
		Channel t = (Channel) self;
		PyObj rob;

		if (Channel_IQualified(t, teq_transfer))
			rob = Py_True;
		else
			rob = Py_False;

		Py_INCREF(rob);
		return(rob);
	}

	static int
	channel_set_xtransfer(PyObj self, PyObj val, void *_)
	{
		Channel t = (Channel) self;

		if (val == Py_True)
			Channel_XQualify(t, teq_transfer);
		else
			Channel_XNQualify(t, teq_transfer);

		return(0);
	}

	static int
	channel_set_itransfer(PyObj self, PyObj val, void *_)
	{
		Channel t = (Channel) self;

		if (val == Py_True)
			Channel_IQualify(t, teq_transfer);
		else
			Channel_INQualify(t, teq_transfer);

		return(0);
	}
#endif

static PyGetSetDef channel_getset[] = {
	/*
		// Event introspection.
	*/
	{"polarity", channel_get_polarity, NULL,
		PyDoc_STR("`1` if the channel receives, `-1` if it sends.")
	},

	{"terminated", channel_get_terminated, NULL,
		PyDoc_STR("Whether the channel is capable of transferring at all.")
	},

	{"exhausted", channel_get_exhausted, NULL,
		PyDoc_STR("Whether the channel has a resource capable of performing transfers.")
	},

	{"resource", channel_get_resource, NULL,
		PyDoc_STR("The object whose buffer was acquired, &Octets.acquire, "
			"as the Channel's transfer resource.\n\n&None if there is no resource.")
	},

	#if FV_INJECTIONS()
		{"_xtransfer",
			channel_get_xtransfer, channel_set_xtransfer,
			PyDoc_STR("Whether the exoresource is currently known to be capable of transfers.")
		},

		{"_itransfer",
			channel_get_itransfer, channel_set_xtransfer,
			PyDoc_STR("Whether the channel is currently known to be capable of transfers.")
		},
	#endif

	{NULL,},
};

PyDoc_STRVAR(channel_doc,
	"The base Channel type, &.abstract.Channel, created and used by &.kernel.\n"
);

/**
	// Base type for the channel implementations.
*/
ChannelPyTypeObject ChannelType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Channel"),   /* tp_name */
	sizeof(struct Channel),          /* tp_basicsize */
	0,                               /* tp_itemsize */
	channel_dealloc,                 /* tp_dealloc */
	NULL,                            /* tp_print */
	NULL,                            /* tp_getattr */
	NULL,                            /* tp_setattr */
	NULL,                            /* tp_compare */
	NULL,                            /* tp_repr */
	NULL,                            /* tp_as_number */
	NULL,                            /* tp_as_sequence */
	NULL,                            /* tp_as_mapping */
	NULL,                            /* tp_hash */
	NULL,                            /* tp_call */
	NULL,                            /* tp_str */
	NULL,                            /* tp_getattro */
	NULL,                            /* tp_setattro */
	NULL,                            /* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_DEFAULT,              /* tp_flags */
	channel_doc,                     /* tp_doc */
	NULL,                            /* tp_traverse */
	NULL,                            /* tp_clear */
	NULL,                            /* tp_richcompare */
	0,                               /* tp_weaklistoffset */
	NULL,                            /* tp_iter */
	NULL,                            /* tp_iternext */
	channel_methods,                 /* tp_methods */
	channel_members,                 /* tp_members */
	channel_getset,                  /* tp_getset */
	NULL,                            /* tp_base */
	NULL,                            /* tp_dict */
	NULL,                            /* tp_descr_get */
	NULL,                            /* tp_descr_set */
	0,                               /* tp_dictoffset */
	NULL,                            /* tp_init */
	NULL,                            /* tp_alloc */
	NULL,                            /* tp_new */
},
	&ChannelTIF,
};

static PyObj
octets_resize_exoresource(PyObj self, PyObj args)
{
	Port p = Channel_GetPort(((Channel) self));
	int size;

	if (!PyArg_ParseTuple(args, "i", &size))
		return(NULL);

	switch (p->type)
	{
		case kt_socket:
			if (port_set_socket_option(p, Channel_Sends(((Channel) self)) ? SO_SNDBUF : SO_RCVBUF, size))
			{
				/*
					// Throw Warning
				*/
			}
		break;

		default:
		break;
	}

	Py_RETURN_NONE;
}

#define alloc_quad() PyTuple_New(4)
#define alloc_pair() PyTuple_New(2)

static Port
alloc_port(void)
{
	Port p;

	PYTHON_RECEPTACLE(NULL, &p, PyAllocate, ((PyObj) porttype));

	if (p)
	{
		p->point = kp_invalid;
		p->cause = kc_pyalloc;
		p->type = kt_unknown;
		p->error = 0;
		p->latches = 0;
	}
	return(p);
}

static PyObj
alloci(PyObj isubtype, Port *out)
{
	Channel t;
	PyObj rob;
	Port p;

	p = alloc_port();
	if (p == NULL)
		return(NULL);

	PYTHON_RECEPTACLE(NULL, &rob, PyAllocate, isubtype);

	if (rob == NULL)
	{
		Py_DECREF(p);
		return(NULL);
	}
	t = (Channel) rob;

	INIT_INPUT_CHANNEL(t, NULL);
	Channel_SetPort(t, p);
	p->latches = 1;

	*out = p;

	return(rob);
}

static PyObj
alloco(PyObj osubtype, Port *out)
{
	PyObj rob;
	Channel t;
	Port p;

	p = alloc_port();
	if (p == NULL)
		return(NULL);

	PYTHON_RECEPTACLE(NULL, &rob, PyAllocate, osubtype);

	if (rob == NULL)
	{
		Py_DECREF(p);
		return(NULL);
	}

	t = (Channel) rob;

	INIT_OUTPUT_CHANNEL(t, NULL);
	Channel_SetPort(t, p);
	p->latches = 1 << 4;
	*out = p;

	return(rob);
}

/**
	// Create a pair of Objects and put them in a tuple to be returned.
*/
static PyObj
allocio(PyObj isubtype, PyObj osubtype, Port *out)
{
	PyObj rob;
	Port port;
	Channel i, o;

	PYTHON_RECEPTACLE("alloc_pair", &rob, alloc_pair);
	if (rob == NULL)
		return(NULL);

	/* channel_dealloc expects port to be non-null.  */
	/* This means ports must be allocated first.     */
	port = alloc_port();
	if (port == NULL)
		goto error;

	PYTHON_RECEPTACLE("alloc_isubtype", &i, PyAllocate, isubtype);
	if (i == NULL)
		goto error;

	Py_INCREF(port);
	Channel_SetPort(i, port);
	PyTuple_SET_ITEM(rob, 0, (PyObj) i);

	PYTHON_RECEPTACLE("alloc_osubtype", &o, PyAllocate, osubtype);
	if (o == NULL)
		goto error;

	Channel_SetPort(o, port);
	PyTuple_SET_ITEM(rob, 1, (PyObj) o);

	INIT_INPUT_CHANNEL(i, NULL);
	INIT_OUTPUT_CHANNEL(o, NULL);

	port->latches = (1 << 4) | 1;
	*out = port;

	return(rob);

	error:
		Py_XDECREF(port);
		Py_DECREF(rob);

	return(NULL);
}

/**
	// Same as allocio, but the Ports for each Channel are distinct objects.
	// (os.pipe(), dup() pairs.
*/
static PyObj
allociopair(PyObj isubtype, PyObj osubtype, Port p[])
{
	PyObj rob;
	PyObj input, output;
	Port x, y;

	PYTHON_RECEPTACLE("alloc_pair", &rob, alloc_pair);
	if (rob == NULL)
		return(NULL);

	input = alloci(isubtype, &x);
	if (input == NULL)
		goto error;
	PyTuple_SET_ITEM(rob, 0, (PyObj) input);

	output = alloco(osubtype, &y);
	if (output == NULL)
		goto error;
	PyTuple_SET_ITEM(rob, 1, (PyObj) output);

	p[0] = x;
	p[1] = y;

	return(rob);

	error:
	{
		p[0] = NULL;
		p[1] = NULL;
		Py_DECREF(rob);
		return(NULL);
	}
}

static PyMethodDef octets_methods[] = {
	{"resize_exoresource", (PyCFunction) octets_resize_exoresource, METH_VARARGS,
		PyDoc_STR(
			"Set the size of the external resource corresponding to transfers.\n"
			"In most cases, this attempts to configure the size of the socket's buffer.\n"

			"[Parameters]\n"
			"/new_size/\n"
			"\tThe number of octets to use as the external resource size.\n"
		)
	},

	{NULL,},
};

struct ChannelInterface
OctetsTIF = {
	{(io_op_t) port_input_octets, (io_op_t) port_output_octets},
	f_octets, 1,
};

PyDoc_STRVAR(Octets_doc, "Channel transferring binary data in bytes.");
ChannelPyTypeObject OctetsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Octets"),   /* tp_name */
	sizeof(struct Channel),         /* tp_basicsize */
	0,                              /* tp_itemsize */
	NULL,                           /* tp_dealloc */
	NULL,                           /* tp_print */
	NULL,                           /* tp_getattr */
	NULL,                           /* tp_setattr */
	NULL,                           /* tp_compare */
	NULL,                           /* tp_repr */
	NULL,                           /* tp_as_number */
	NULL,                           /* tp_as_sequence */
	NULL,                           /* tp_as_mapping */
	NULL,                           /* tp_hash */
	NULL,                           /* tp_call */
	NULL,                           /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	NULL,                           /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,             /* tp_flags */
	Octets_doc,                     /* tp_doc */
	NULL,                           /* tp_traverse */
	NULL,                           /* tp_clear */
	NULL,                           /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	octets_methods,                 /* tp_methods */
	NULL,                           /* tp_members */
	NULL,                           /* tp_getset */
	&ChannelType.typ,               /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	NULL,                           /* tp_alloc */
	NULL,                           /* tp_new */
},
	&OctetsTIF
};

struct ChannelInterface
SocketsTIF = {
	{(io_op_t) port_input_sockets, NULL},
	f_sockets, sizeof(int),
};

static PyObj
sockets_set_accept_filter(PyObj self, PyObj args)
{
	Channel t = (Channel) self;
	char *filtername;

	if (!PyArg_ParseTuple(args, "s", &filtername))
		return(NULL);

	if (Channel_PortLatched(t))
	{
		#ifdef SO_ACCEPTFILTER
		{
			struct accept_filter_arg afa;

			if (strlen(filtername)+1 > sizeof(afa.af_name))
			{
				PyErr_SetString(PyExc_ValueError, "filter name is too long");
				return(NULL);
			}

			bzero(&afa, sizeof(afa));
			strcpy(afa.af_name, filtername);
			setsockopt(Channel_GetKPoint(t), SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa));
		}
		#else
			;/* XXX: warn about accept filter absence? */
		#endif
	}

	Py_RETURN_NONE;
}

static PyObj
sockets_resize_exoresource(PyObj self, PyObj args)
{
	Channel t = (Channel) self;
	int backlog;

	if (!PyArg_ParseTuple(args, "i", &backlog))
		return(NULL);

	if (Channel_PortLatched(t))
	{
		/*
			// Failure to resize the listening queue is not necessarily
			// fatal; this is unlike listen during initialization as
			// we are essentially checking that the socket *can* listen.
		*/

		port_listen(Channel_GetPort(t), backlog);
	}

	Py_RETURN_NONE;
}

static PyMethodDef sockets_methods[] = {
	{"resize_exoresource",
		(PyCFunction) sockets_resize_exoresource,
		METH_VARARGS,
		PyDoc_STR(
			"Resize the Sockets' listening queue. Normally, this adjusts the backlog of a listening socket.\n"
			"\n"
			"[Parameters]\n"
			"/(&int)`backlog`/\n"
			"\tBacklog parameter given to &2.listen"
			"/(&int)`Return`/\n"
			"\tThe given backlog.\n"
			"\n"
		)
	},

	{"set_accept_filter",
		(PyCFunction) sockets_set_accept_filter,
		METH_VARARGS,
		PyDoc_STR(
			"Set an accept filter on the socket so that &2.accept "
			"only accepts sockets that meet the designated filter's requirements.\n"
			"\n"
			"On platforms that do support accept filters this method does nothing.\n"
			"Currently, this is a FreeBSD only feature: `'dataready'`, 'dnsready'`, 'httpready'`\n"

			"[Parameters]\n"
			"/(&str)`name`/\n"
			"\tThe name of the filter to use.\n"
			"\n"
			"[Effects]\n"
			"/(&None.__class__)`Return`/\n"
			"\tNothing.\n"
			"\n"
		)
	},

	{NULL,}
};

PyDoc_STRVAR(Sockets_doc, "channel transferring file descriptors accepted by accept(2)");

ChannelPyTypeObject SocketsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Sockets"),  /* tp_name */
	sizeof(struct Channel),         /* tp_basicsize */
	0,                              /* tp_itemsize */
	NULL,                           /* tp_dealloc */
	NULL,                           /* tp_print */
	NULL,                           /* tp_getattr */
	NULL,                           /* tp_setattr */
	NULL,                           /* tp_compare */
	NULL,                           /* tp_repr */
	NULL,                           /* tp_as_number */
	NULL,                           /* tp_as_sequence */
	NULL,                           /* tp_as_mapping */
	NULL,                           /* tp_hash */
	NULL,                           /* tp_call */
	NULL,                           /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	NULL,                           /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,             /* tp_flags */
	Sockets_doc,                    /* tp_doc */
	NULL,                           /* tp_traverse */
	NULL,                           /* tp_clear */
	NULL,                           /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	sockets_methods,                /* tp_methods */
	NULL,                           /* tp_members */
	NULL,                           /* tp_getset */
	&ChannelType.typ,               /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	NULL,                           /* tp_alloc */
	NULL,                           /* tp_new */
},
	&SocketsTIF,
};

struct ChannelInterface
PortsTIF = {
	{(io_op_t) port_input_ports, (io_op_t) port_output_ports},
	f_ports, sizeof(int),
};

static PyMethodDef
ports_methods[] = {
	{NULL,}
};

PyDoc_STRVAR(Ports_doc, "");

ChannelPyTypeObject
PortsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Ports"),    /* tp_name */
	sizeof(struct Channel),         /* tp_basicsize */
	0,                              /* tp_itemsize */
	NULL,                           /* tp_dealloc */
	NULL,                           /* tp_print */
	NULL,                           /* tp_getattr */
	NULL,                           /* tp_setattr */
	NULL,                           /* tp_compare */
	NULL,                           /* tp_repr */
	NULL,                           /* tp_as_number */
	NULL,                           /* tp_as_sequence */
	NULL,                           /* tp_as_mapping */
	NULL,                           /* tp_hash */
	NULL,                           /* tp_call */
	NULL,                           /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	NULL,                           /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,             /* tp_flags */
	Ports_doc,                      /* tp_doc */
	NULL,                           /* tp_traverse */
	NULL,                           /* tp_clear */
	NULL,                           /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	ports_methods,                  /* tp_methods */
	NULL,                           /* tp_members */
	NULL,                           /* tp_getset */
	&ChannelType.typ,               /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	NULL,                           /* tp_alloc */
	NULL,                           /* tp_new */
},
	&PortsTIF,
};

/**
	// Structure for &.kernel.DatagramArray providing access
	// to the composition of datagrams to be emitted or received.
*/
struct DatagramArray {
	PyObject_VAR_HEAD

	/**
		// Number of entries in &indexes.
	*/
	uint32_t ngrams;

	/**
		// Address Length of endpoints.
	*/
	socklen_t addrlen;

	/**
		// Packet Family of endpoints.
	*/
	int pf;

	/**
		// Address Space of endpoints.
	*/
	uint32_t space;

	/**
		// Current transfer.
	*/
	Py_buffer data;

	/**
		// The indexes to the Datagrams held by the Array inside
		// the buffer, `data`.
	*/
	struct Datagram *indexes[0];
};

static PyObj
datagramarray_get_memory(DatagramArray dga, uint32_t offset)
{
	PyObj rob, slice, mv;
	struct Datagram *dg;
	Py_ssize_t start, stop;
	Py_buffer buf;

	if (offset >= dga->ngrams)
	{
		PyErr_SetString(PyExc_IndexError, "index out of range");
		return(NULL);
	}

	dg = dga->indexes[offset];

	/**
		// Need the base buffer object for proper slicing.
	*/
	if (PyObject_GetBuffer(dga->data.obj, &buf, PyBUF_WRITABLE))
		return(NULL);

	start = ((char *) DatagramGetData(dg)) - (char *) buf.buf;
	stop = start + DatagramGetSpace(dg);

	slice = _PySlice_FromIndices(start, stop);
	if (slice == NULL)
	{
		PyBuffer_Release(&buf);
		return(NULL);
	}

	mv = PyMemoryView_FromObject(buf.obj);
	if (mv == NULL)
	{
		Py_DECREF(slice);
		PyBuffer_Release(&buf);
		return(NULL);
	}

	rob = PyObject_GetItem(mv, slice);
	PyBuffer_Release(&buf);
	Py_DECREF(slice);
	Py_DECREF(mv);

	return(rob);
}

static PyObj
datagramarray_get_endpoint(DatagramArray dga, uint32_t offset)
{
	struct Datagram *dg;

	if (offset >= dga->ngrams)
	{
		PyErr_SetString(PyExc_IndexError, "index out of range");
		return(NULL);
	}

	dg = dga->indexes[offset];

	return((PyObj) EP->create(0, 0, DatagramGetAddress(dg), DatagramGetAddressLength(dg)));
}

static PyObj
datagramarray_payload(PyObj self, PyObj args)
{
	DatagramArray dga = (DatagramArray) self;
	unsigned long offset;

	if (!PyArg_ParseTuple(args, "k", &offset))
		return(NULL);

	return(datagramarray_get_memory(dga, offset));
}

static PyObj
datagramarray_endpoint(PyObj self, PyObj args)
{
	DatagramArray dga = (DatagramArray) self;
	unsigned long offset;

	if (!PyArg_ParseTuple(args, "k", &offset))
		return(NULL);

	return(datagramarray_get_endpoint(dga, offset));
}

static PyObj
datagramarray_set_endpoint(PyObj self, PyObj args)
{
	struct Datagram *dg;
	DatagramArray dga = (DatagramArray) self;
	PyObj endpoint;
	unsigned long offset;

	if (!PyArg_ParseTuple(args, "kO", &offset, &endpoint))
		return(NULL);

	if (offset >= dga->ngrams)
	{
		PyErr_SetString(PyExc_IndexError, "index out of range");
		return(NULL);
	}

	dg = dga->indexes[offset];

	switch (dga->pf)
	{
		case ip4_pf:
			if (!EP->ip4_converter(endpoint, dg->addr))
				return(NULL);
		break;

		case ip6_pf:
			if (!EP->ip6_converter(endpoint, dg->addr))
				return(NULL);
		break;

		default:
			PyErr_SetString(PyExc_SystemError, "invalid packet family configured on DatagramArray");
			return(NULL);
		break;
	}

	Py_RETURN_NONE;
}

static PyMethodDef datagramarray_methods[] = {
	{"payload",
		(PyCFunction) datagramarray_payload, METH_VARARGS,
		PyDoc_STR("Extract the payload for the datagram at the given offset. Returns a &memoryview.")
	},

	{"endpoint",
		(PyCFunction) datagramarray_endpoint, METH_VARARGS,
		PyDoc_STR("Extract the endpoint for the datagram at the given offset.")
	},

	{"set_endpoint",
		(PyCFunction) datagramarray_set_endpoint, METH_VARARGS,
		PyDoc_STR("Set the endpoint for the specified datagram.")
	},

	{NULL,},
};

static PyObj
allocdga(PyTypeObject *subtype, int pf, uint32_t space, uint32_t ngrams)
{
	uint32_t unit;
	uint32_t i;
	PyObj ba = NULL, rob;
	DatagramArray dga;
	char *fdg;
	struct Datagram *cur;

	PYTHON_RECEPTACLE("tp_alloc", &rob, subtype->tp_alloc, subtype, ngrams + 1);
	if (rob == NULL)
		return(NULL);

	PYTHON_RECEPTACLE("new_ba", &ba, PyByteArray_FromStringAndSize, "", 0);
	if (ba == NULL)
	{
		Py_DECREF(rob);
		return(NULL);
	}

	dga = (DatagramArray) rob;
	dga->space = space;
	dga->ngrams = ngrams;
	dga->data.obj = NULL;

	dga->pf = pf;
	switch (pf)
	{
		case ip4_pf:
			dga->addrlen = sizeof(ip4_addr_t);
		break;

		case ip6_pf:
			dga->addrlen = sizeof(ip6_addr_t);
		break;

		default:
			PyErr_SetString(PyExc_TypeError, "unrecognized packet family");
			goto error;
		break;
	}
	unit = DatagramCalculateUnit(space, dga->addrlen);

	i = PyByteArray_Resize(ba, (unit * ngrams));
	if (i)
		goto error;

	i = PyObject_GetBuffer(ba, &(dga->data), PyBUF_WRITABLE);
	if (i)
		goto error;

	/*
		// Clear data. Allows memoryview payload access to copy first n-bytes and forget.
	*/
	bzero(dga->data.buf, dga->data.len);

	Py_DECREF(ba); /* `data` buffer holds our reference */

	if (ngrams > 0)
	{
		/*
			// The last index points to the end of the memory block.
		*/
		for (fdg = dga->data.buf, i = 0; i < ngrams; ++i, fdg += unit)
		{
			cur = dga->indexes[i] = (struct Datagram *) fdg;
			cur->addrlen = dga->addrlen;
			cur->gramspace = space;
		}

		/*
			// end of buffer index
		*/
		dga->indexes[i] = (struct Datagram *) fdg;
	}
	else
	{
		dga->indexes[0] = dga->data.buf;
	}

	return(rob);
	error:
		Py_DECREF(rob);
		Py_DECREF(ba);
		return(NULL);
}

static PyObj
slicedga(DatagramArray src, Py_ssize_t start, Py_ssize_t stop)
{
	PyTypeObject *subtype = Py_TYPE(src);
	uint32_t i;
	PyObj rob;
	DatagramArray dga;

	/*
		// Normalize indexes. If the index is greater than or less than,
		// set it to the terminal.
	*/
	if (start > src->ngrams)
		stop = start = src->ngrams;
	else if (stop > src->ngrams)
		stop = src->ngrams;
	else if (stop < start)
		stop = start;

	if (src->ngrams == 0 || (start == 0 && stop == src->ngrams))
	{
		/*
			// slice of empty array.
		*/
		Py_INCREF(src);
		return((PyObj) src);
	}

	/*
		// At least one index for the sentinal.
	*/
	PYTHON_RECEPTACLE(NULL, &rob, subtype->tp_alloc, subtype, (stop - start) + 1);
	if (rob == NULL)
		return(NULL);
	dga = (DatagramArray) rob;
	dga->data.obj = NULL;

	/*
		// No Python errors after this point.
	*/
	dga->addrlen = src->addrlen;
	dga->pf = src->pf;
	dga->space = src->space;
	dga->ngrams = stop - start;

	if (PyObject_GetBuffer(src->data.obj, &(dga->data), PyBUF_WRITABLE))
	{
		Py_DECREF(rob);
		return(NULL);
	}

	/*
		// The last index points to the end of the memory block.
	*/
	for (i = 0; start <= stop; ++i, ++start)
	{
		dga->indexes[i] = src->indexes[start];
	}

	dga->data.buf = (char *) src->indexes[stop];
	dga->data.len = ((intptr_t) dga->indexes[i-1]) - ((intptr_t) dga->indexes[0]);

	return(rob);
}

static PyObj
datagramarray_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"domain", "space", "number", NULL};
	unsigned long size;
	unsigned long ngrams;
	char *addrtype;
	PyObj sequence;
	int pf;
	PyObj rob;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "sLL", kwlist, &addrtype, &size, &ngrams))
		return(NULL);

	if (strcmp("ip4", addrtype) == 0)
		pf = ip4_pf;
	else if (strcmp("ip6", addrtype) == 0)
		pf = ip6_pf;
	else
	{
		PyErr_SetString(PyExc_TypeError, "invalid domain");
		return(NULL);
	}

	return(allocdga(subtype, pf, size, ngrams));
}

static Py_ssize_t
datagramarray_length(PyObj self)
{
	DatagramArray dga = (DatagramArray) self;
	return(dga->ngrams);
}

static PyObj
datagramarray_getitem(PyObj self, Py_ssize_t index)
{
	DatagramArray dga = (DatagramArray) self;
	PyObj ep, mv, rob;

	PYTHON_RECEPTACLE("get_endpoint", &ep, datagramarray_get_endpoint, dga, index);
	if (ep == NULL)
		return(NULL);

	PYTHON_RECEPTACLE("get_memory", &mv, datagramarray_get_memory, dga, index);
	if (mv == NULL)
	{
		Py_DECREF(ep);
		return(NULL);
	}

	PYTHON_RECEPTACLE("new_tuple", &rob, PyTuple_New, 2);
	if (rob == NULL)
	{
		Py_DECREF(ep);
		Py_DECREF(mv);
		return(NULL);
	}

	PyTuple_SET_ITEM(rob, 0, ep);
	PyTuple_SET_ITEM(rob, 1, mv);

	return(rob);
}

static PyObj
datagramarray_subscript(PyObj self, PyObj item)
{
	PyObj rob;
	DatagramArray dga = (DatagramArray) self;

	if (PyObject_IsInstance(item, (PyObj) &PySlice_Type))
	{
		Py_ssize_t start, stop, step, slen;

		if (PySlice_GetIndicesEx(item, dga->ngrams, &start, &stop, &step, &slen))
			return(NULL);

		if (step != 1)
		{
			PyErr_SetString(PyExc_TypeError, "only steps of `1` are supported by DatagramArray");
			return(NULL);
		}

		rob = slicedga(dga, start, stop);
	}
	else
	{
		PyObj lo;
		Py_ssize_t i;
		lo = PyNumber_Long(item);
		if (lo == NULL) return(NULL);

		i = PyLong_AsSsize_t(lo);
		Py_DECREF(lo);

		if (i < 0)
			i = i + dga->ngrams;

		if (i > dga->ngrams || i < 0)
		{
			PyErr_SetString(PyExc_IndexError, "index out of range");
			rob = NULL;
		}
		else
			rob = datagramarray_getitem(self, i);
	}

	return(rob);
}

static PySequenceMethods
datagramarray_sequence = {
	datagramarray_length,
	NULL,
	NULL,
	datagramarray_getitem,
};

static PyMappingMethods
datagramarray_mapping = {
	datagramarray_length,
	datagramarray_subscript,
	NULL,
};

static int
datagramarray_getbuffer(PyObj self, Py_buffer *view, int flags)
{
	int r;
	DatagramArray dga = (DatagramArray) self;

	r = PyObject_GetBuffer(dga->data.obj, view, flags);
	if (r) return(r);

	/*
		// slice according to the local perspective of the underlying bytearray
	*/
	view->buf = (void *) dga->indexes[0];
	view->len = ((intptr_t) dga->indexes[dga->ngrams]) - ((intptr_t) dga->indexes[0]);

	return(0);
}

static PyBufferProcs
datagramarray_buffer = {
	datagramarray_getbuffer,
	NULL,
};

static PyObj
datagramarray_iter(PyObj self)
{
	return(PySeqIter_New(self));
}

static void
datagramarray_dealloc(PyObj self)
{
	DatagramArray dga = (DatagramArray) self;
	if (dga->data.obj != NULL)
		PyBuffer_Release(&(dga->data));

	Py_TYPE(self)->tp_free(self);
}

PyDoc_STRVAR(datagramarray_doc, "A mutable buffer object for sending and receiving Datagrams; octets coupled with an IP address.");
PyTypeObject DatagramArrayType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("DatagramArray"), /* tp_name */
	sizeof(struct DatagramArray),        /* tp_basicsize */
	sizeof(struct Datagram *),           /* tp_itemsize */
	datagramarray_dealloc,               /* tp_dealloc */
	NULL,                                /* tp_print */
	NULL,                                /* tp_getattr */
	NULL,                                /* tp_setattr */
	NULL,                                /* tp_compare */
	NULL,                                /* tp_repr */
	NULL,                                /* tp_as_number */
	&datagramarray_sequence,             /* tp_as_sequence */
	&datagramarray_mapping,              /* tp_as_mapping */
	NULL,                                /* tp_hash */
	NULL,                                /* tp_call */
	NULL,                                /* tp_str */
	NULL,                                /* tp_getattro */
	NULL,                                /* tp_setattro */
	&datagramarray_buffer,               /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,                  /* tp_flags */
	datagramarray_doc,                   /* tp_doc */
	NULL,                                /* tp_traverse */
	NULL,                                /* tp_clear */
	NULL,                                /* tp_richcompare */
	0,                                   /* tp_weaklistoffset */
	datagramarray_iter,                  /* tp_iter */
	NULL,                                /* tp_iternext */
	datagramarray_methods,               /* tp_methods */
	NULL,                                /* tp_members */
	NULL,                                /* tp_getset */
	NULL,                                /* tp_base */
	NULL,                                /* tp_dict */
	NULL,                                /* tp_descr_get */
	NULL,                                /* tp_descr_set */
	0,                                   /* tp_dictoffset */
	NULL,                                /* tp_init */
	NULL,                                /* tp_alloc */
	datagramarray_new,                   /* tp_new */
};

static PyObj
datagrams_transfer(PyObj self)
{
	Channel t = (Channel) self;
	DatagramArray resource;
	uint32_t unit;
	PyObj rob;
	PyObj s;

	if (!Channel_HasResource(t)
		|| !Channel_HasEvent(t, tev_transfer))
	{
		Py_RETURN_NONE;
	}

	resource = (DatagramArray) Channel_GetResource(t);
	unit = DatagramCalculateUnit(resource->space, resource->addrlen);

	s = _PySlice_FromIndices(Channel_GetWindowStart(t) / unit, Channel_GetWindowStop(t) / unit);
	if (s == NULL) return(NULL);

	rob = PyObject_GetItem((PyObj) resource, s);
	Py_DECREF(s);

	return(rob);
}

static PyMethodDef datagrams_methods[] = {
	{"transfer",
		(PyCFunction) datagrams_transfer, METH_NOARGS,
		PyDoc_STR(
			"The slice of the Datagrams representing the Transfer.\n"
			"\n"
			"[ Return ]\n"
			"The transferred data as a &DatagramArray.\n"
		)
	},
	{NULL,},
};

struct ChannelInterface
DatagramsTIF = {
	{(io_op_t) port_input_datagrams, (io_op_t) port_output_datagrams},
	f_datagrams, 1,
};

/*
	// deallocation is straight forward except in the case of sockets,
	// which have a shared file descriptor and must refer to each other
	// to identify whether or not the kpoint can be closed.
*/

PyDoc_STRVAR(datagrams_doc, "channel transferring DatagramArray's");
ChannelPyTypeObject DatagramsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Datagrams"),  /* tp_name */
	sizeof(struct Datagrams),         /* tp_basicsize */
	0,                                /* tp_itemsize */
	NULL,                             /* tp_dealloc */
	NULL,                             /* tp_print */
	NULL,                             /* tp_getattr */
	NULL,                             /* tp_setattr */
	NULL,                             /* tp_compare */
	NULL,                             /* tp_repr */
	NULL,                             /* tp_as_number */
	NULL,                             /* tp_as_sequence */
	NULL,                             /* tp_as_mapping */
	NULL,                             /* tp_hash */
	NULL,                             /* tp_call */
	NULL,                             /* tp_str */
	NULL,                             /* tp_getattro */
	NULL,                             /* tp_setattro */
	NULL,                             /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,               /* tp_flags */
	datagrams_doc,                    /* tp_doc */
	NULL,                             /* tp_traverse */
	NULL,                             /* tp_clear */
	NULL,                             /* tp_richcompare */
	0,                                /* tp_weaklistoffset */
	NULL,                             /* tp_iter */
	NULL,                             /* tp_iternext */
	datagrams_methods,                /* tp_methods */
	NULL,                             /* tp_members */
	NULL,                             /* tp_getset */
	&ChannelType.typ,                 /* tp_base */
	NULL,                             /* tp_dict */
	NULL,                             /* tp_descr_get */
	NULL,                             /* tp_descr_set */
	0,                                /* tp_dictoffset */
	NULL,                             /* tp_init */
	NULL,                             /* tp_alloc */
	NULL,                             /* tp_new */
},
	&DatagramsTIF
};

static PyObj
array_resize_exoresource(PyObj self, PyObj args)
{
	kevent_t *new_area;
	unsigned int new_size;
	Array J = (Array) self;

	/*
		// This adjusts the size of the kevents array, which is technically a
		// process resource, but arrays are special so use the exoresource
		// as a means to hint to the size of the kevent array as well.
	*/

	/*
		// Requires GIL.
	*/

	if (!PyArg_ParseTuple(args, "I", &new_size))
		return(NULL);

	if (Array_Cycling(J))
	{
		PyErr_SetString(PyExc_RuntimeError, "cannot resize array inside cycle");
		return(NULL);
	}

	new_area = (kevent_t *) PyMem_Realloc((void *) Array_GetKEvents(J), (size_t) new_size * sizeof(kevent_t));
	if (new_area != NULL)
	{
		Array_SetKEvents(J, new_area);
		Channel_SetWindowStop(J, new_size);
	}

	return(PyLong_FromUnsignedLong(Channel_GetWindowStop(J)));
}

static PyObj
array_acquire(PyObj self, PyObj ob)
{
	Array J = (Array) self;
	Channel t = (Channel) ob;

	if (!PyObject_IsInstance(ob, (PyObj) &ChannelType))
	{
		PyErr_SetString(PyExc_TypeError, "cannot attach objects that are not channels");
		return(NULL);
	}

	if (Channel_Terminating(J))
	{
		/* Array is Terminated */
		PyErr_SetChannelTerminatedError(J);
		return(NULL);
	}

	if (!Channel_Attached(t))
	{
		if (Channel_Terminated(t))
		{
			/* Given Channel is Terminated */
			/*
				// Terminating check performed after the NULL check because
				// if the given channel is already acquired by the Array,
				// it shouldn't complain due to it already being acquired.

				// Additionally, it's desired that ResourceError is consistently
				// thrown in this case.
			*/
			PyErr_SetChannelTerminatedError(t);
			return(NULL);
		}

		/* Control bit signals needs to connect. (kfilter) */
		Channel_DControl(t, ctl_connect);

		Py_INCREF(J); /* Newly acquired channel's reference to Array.     */
		Py_INCREF(t); /* Array's reference to the newly acquired Channel. */

		Channel_SetArray(t, J);
		CHANNEL_ATTACH(t);

		Array_IncrementChannelCount(J);
	}
	else
	{
		if (Channel_GetArray(t) != J)
		{
			/* Another Array instance acquired the Channel */
			PyErr_SetChannelResourceError(t);
			return(NULL);
		}

		/* Otherwise, just fall through as it's already acquired. */
	}

	Py_INCREF(ob);
	return(ob);
}

static void
array_init(Array J)
{
	const struct timespec ts = {0,0};
	Port p = Channel_GetPort(J);
	int nkevents;
	kevent_t kev;

	#ifdef EVMECH_EPOLL
		if (port_epoll_create(p))
			return;
		else
		{
			kevent_t k;
			struct Port wp;
			port_epoll_create(&wp);

			J->choice.array.wfd = wp.point;
			J->choice.array.efd = eventfd(0, EFD_CLOEXEC);

			k.events = EPOLLERR | EPOLLHUP | EPOLLIN;
			k.data.ptr = NULL;

			epoll_ctl(p->point, EPOLL_CTL_ADD, J->choice.array.efd, &k);

			k.events = EPOLLERR | EPOLLHUP | EPOLLIN | EPOLLOUT;
			k.data.ptr = J;
			epoll_ctl(p->point, EPOLL_CTL_ADD, J->choice.array.wfd, &k);
		}
	#else
		/* kqueue */
		if (port_kqueue(p))
			return;

		kev.udata = (void *) J;
		kev.ident = (uintptr_t) J;
		kev.flags = EV_ADD|EV_RECEIPT|EV_CLEAR;
		kev.filter = EVFILT_USER;
		kev.fflags = 0;
		kev.data = 0;

		port_kevent(p, 1, &nkevents, &kev, 1, &kev, 1, &ts);
	#endif
}

static void
array_start_cycle(Array J)
{
	Channel_SetNextTransfer(J, (Channel) J); /* Start with an Empty Transfer List */
}

static void
array_finish_cycle(Array J)
{
	/* Complete Array termination? */

	Channel_SetNextTransfer(J, NULL); /* NULL transfer list means the cycle is over. */
	Array_ResetTransferCount(J);
}

#ifdef EVMECH_EPOLL
	#define array_kevent_change(J)
#else
static void
array_kevent_change(Array J)
{
	const static struct timespec nowait = {0,0}; /* Never wait for change submission. */
	Port port = Channel_GetPort(J);
	int r = 0, nkevents = Array_NChanges(J);
	kevent_t *kevs = Array_GetKEvents(J);

	Array_ResetWindow(J);

	/*
		// Receipts are demanded, so the enties are only used for error reporting.
	*/
	#if F_TRACE(subscribe)
		for (int i = 0; i < nkevents; ++i)
			pkevent(&(kevs[i]));
	#endif

	/*
		// These must finish, so don't accept EINTR/EAGAIN.
	*/
	if (nkevents)
		port_kevent(port, -1, &r, kevs, nkevents, kevs, nkevents, &nowait);
}
#endif

static void
array_kevent_collect(Array J, int waiting)
{
	Port port = Channel_GetPort(J);
	kevent_t *kevs = Array_GetKEvents(J);
	int nkevents = 0;

	#ifdef EVMECH_EPOLL
		const static int nowait = 0, wait = 9 * 1000;

		/*
			// For epoll, there are two file descriptors; one epoll referring to readers
			// and the other to the writers. The interface doesn't provide details about
			// whether reading is possible or writing can occur, so it has to be split up.
		*/
		switch (J->choice.array.haswrites)
		{
			case 1:
			{
				struct Port wp;
				wp.point = J->choice.array.wfd;
				port_epoll_wait(&wp, &nkevents, kevs, Channel_GetWindowStop(J), (waiting ? wait : nowait));

				if (nkevents < Channel_GetWindowStop(J))
					J->choice.array.haswrites = 0;
				else
					J->choice.array.haswrites = 2;
			}
			break;

			case 2:
				J->choice.array.haswrites = 1; /* alternates between reads and writes */
			case 0:
				port_epoll_wait(port, &nkevents, kevs, Channel_GetWindowStop(J), (waiting ? wait : nowait));
			break;
		}
	#else
		const static struct timespec nowait = {0,0};
		const static struct timespec waitfor = {9,0};

		struct timespec *wait = (struct timespec *) (waiting ? &waitfor : &nowait);
		port_kevent(port, 1, &nkevents, NULL, 0, kevs, Channel_GetWindowStop(J), wait);
	#endif

	Array_SetNCollected(J, nkevents);

	#if F_TRACE(collect)
		errpf("pid: %d\n", getpid());
		for (int i = 0; i < nkevents; ++i)
		{
			pkevent(&(kevs[i]));
			pchannel((Channel) kevs->udata);
		}
	#endif
}

/**
	// Note tev_join events on all Channels.

	// Run before array_transfer_delta to have all
	// Channel's corresponding kevent filter to be loaded.
*/
static void
array_reload(Array J)
{
	Channel t = J->next;

	/* MUST HAVE GIL */

	while (t != (Channel) J)
	{
		Channel_DControl(t, ctl_connect);
		t = t->next;
	}
}

/**
	// Enqueue the delta into the lltransfer list.
*/
static void
array_transfer_delta(Array J)
{
	Channel t;

	/* MUST HAVE GIL */

	/*
		// Scans the ring behind the Array.
		// Process Events are queued up by moving the Channel behind the Array after
		// applying flags to channel->delta.
	*/
	for (t = J->prev; Channel_GetDelta(t) != 0; t = t->prev)
	{
		/*
			// prepend to the lltransfer list.
			// The first 't' was the last enqueued.
		*/
		Channel_StateMerge(t, Channel_GetDelta(t)); /* Record the internal event quals. */
		Channel_ClearDelta(t); /* for subsequent use; after gil gets released */

		/*
			// Add to event list.
		*/
		Array_AddTransfer(J, t);
	}
}

static kevent_t *
array_current_kevent_slot(Array J)
{
	/* Flush changes if the window is empty. */
	if (Array_MaxCollected(J))
	{
		/* Full, flush the existing changes. */
		array_kevent_change(J);
	}

	return(Array_GetKEventSlot(J, Channel_GetWindowStart(J)));
}

/**
	// Process delta and setup for event processing
*/
static void
array_apply_delta(Array J)
{
	Channel prev, t;

	/* NO GIL */

	prev = (Channel) J;

	/* Reset kev state for slot acquisition. */
	Array_ResetWindow(J);

	/*
		// Iterate through the transfer list in order to make any necessary
		// changes to the Channel's kfilter.

		// There is a need to keep track of the previous item on the list in case we need to
		// evict the Channel from our event list.
	*/

	for (t = Channel_GetNextTransfer(J); t != (Channel) J; t = Channel_GetNextTransfer(prev))
	{
		if (Channel_ShouldXConnect(t))
		{
			/*
				// iff xterminate hasn't occurred.
				// Happens with channels that are terminated at creation due to syscall failure.
			*/
			if (Channel_PortError(t) || !Channel_PortLatched(t))
			{
				/*
					// Inherit error or ignore connect if unlatched.
				*/
				Channel_XQualify(t, teq_terminate);
			}
			else if (!Channel_GetControl(t, ctl_requeue))
			{
				/*
					// Only connect if our port is latched
					// and the requeue flag is *not* set.
				*/
				kfilter_attach(t, array_current_kevent_slot(J));
				Array_ConsumeKEventSlot(J);
			}

			Channel_NulControl(t, ctl_connect);
		}

		if (Channel_GetControl(t, ctl_force))
		{
			/*
				// Remove the flag.
			*/
			Channel_NulControl(t, ctl_force);

			/*
				// It's a lie. The buffer will be zero, but the transfer
				// attempt will still occur likely resulting in zero read.
			*/
			Channel_XQualify(t, teq_transfer);
		}

		/*
			// Determine whether or not the Channel should be processed due to
			// the state change performed by the process.
		*/
		if (Channel_EventState(t))
		{
			/*
				// There is no check for "should exhaust" as exhaustion only
				// follows a transfer. Exhaust events are determined after the transfer
				// is emitted.
			*/
			prev = t;
		}
		else
		{
			/*
				// Incomplete qualifications, remove channel from list.
			*/

			/*
				// Current `prev` is valid, so set the next to this channel's next.
				// Afterwards, this Tranit's next pointer to NULL to signal that
				// it is not participating in a Transfer.
			*/
			Channel_SetNextTransfer(prev, Channel_GetNextTransfer(t));
			Channel_SetNextTransfer(t, NULL);
		}
	}

	/*
		// Make any remaining changes.
	*/
	array_kevent_change(J);
}

#ifdef EVMECH_EPOLL
/**
	// Transform the collected events into local Channel state.
	// Place actionable events onto their respective transfer list.
*/
static void
array_kevent_transform(Array J)
{
	Channel t;
	Port p;
	kevent_t *kev, *kevs = Array_GetKEvents(J);
	uint32_t i, nkevents = Array_NCollected(J);

	/*
		// Iterate over the collected events and
		// transform the kevent state data into Channel state.
	*/
	for (i = 0; i < nkevents; ++i)
	{
		kev = &(kevs[i]);
		t = (Channel) kev->data.ptr;

		/*
			// The eventfd to trip epoll_wait()
		*/
		if (t == NULL)
		{
			uint64_t buf;
			read(J->choice.array.efd, &buf, sizeof(buf));
			continue;
		}
		else if (t == J)
		{
			/*
				// Writes signal.
			*/
			J->choice.array.haswrites = 1;
		}

		p = Channel_GetPort(t);

		if (kev->events & EPOLLIN
			|| kev->events & EPOLLOUT)
		{
			Channel_XQualify(t, teq_transfer);

			if (Channel_IQualified(t, teq_transfer))
				Array_AddTransfer(J, t);
		}

		if (kev->events & EPOLLRDHUP
			|| kev->events & EPOLLERR
			|| kev->events & EPOLLHUP)
		{
			Channel_XQualify(t, teq_terminate);
			Array_AddTransfer(J, t);
		}
	}
}

#else

/**
	// Transform the collected events into local Channel state.
	// Place actionable events onto their respective transfer list.
*/
static void
array_kevent_transform(Array J)
{
	Channel t;
	Port p;
	kevent_t *kev, *kevs = Array_GetKEvents(J);
	uint32_t i, nkevents = Array_NCollected(J);

	/*
		// Iterate over the collected events and
		// transform the kevent state data into Channel state.
	*/
	for (i = 0; i < nkevents; ++i)
	{
		kev = &(kevs[i]);
		t = (Channel) kev->udata;

		/*
			// (EVFILT_USER) user signaled for kevent exit?
		*/
		if (t == (Channel) J)
		{
			continue;
		}

		p = Channel_GetPort(t);

		if (kev->filter == EVFILT_WRITE && kev->flags & EV_EOF)
		{
			/*
				// Only xterminate when it's an Output channel.
				// io_terminate will handle termination on Input channels
				// in order to make sure that all data has been transferred into the process.
			*/
			Channel_XQualify(t, teq_terminate);
			Port_SetError(p, kev->fflags, kc_eof);

			/*
				// ShouldTerminate
			*/
			Array_AddTransfer(J, t);
		}
		else
		{
			/*
				// Always note when a transfer is *possible*.
				// The iTransfer must be present in order for an event to be enqueued.
			*/

			/* Zero read triggers termination, writes are terminated by [local] host. */
			Channel_XQualify(t, teq_transfer);

			/*
				// Kernel can transfer, if the channel can too, then queue it up.
			*/
			if (Channel_IQualified(t, teq_transfer))
				Array_AddTransfer(J, t);
		}
	}
}
#endif

static int
array_fall(Array J, int force)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	if (!force && J->choice.array.will_wait == 0)
		return(0);

	#ifdef EVMECH_EPOLL
	{
		uint64_t buf = 1;
		write(J->choice.array.efd, &buf, sizeof(buf));
	}
	#else
	{
		kev.udata = (void *) J;
		kev.ident = (uintptr_t) J;
		kev.filter = EVFILT_USER;
		kev.fflags = NOTE_TRIGGER;
		kev.data = 0;
		kev.flags = EV_RECEIPT;

		if (port_kevent(Channel_GetPort(J), 1, &out, &kev, 1, NULL, 0, &ts))
		{
			return(-1);
		}
	}
	#endif

	return(1);
}

static PyObj
array_force(PyObj self)
{
	PyObj rob;
	Array J = (Array) self;

	if (Channel_Terminating(J))
		Py_RETURN_NONE;

	rob = array_fall(J, 1) == 0 ? Py_False : Py_True;
	Py_INCREF(rob);
	return(rob);
}

static void
_array_terminate(Channel J)
{
	Channel t;
	Channel_IQualify(J, teq_terminate);

	/*
		// Terminate all the Channels in the Array's ring.
	*/
	for (t = J->next; t != J; t = t->next)
	{
		/*
			// Enqueue is necessary here because ALL channels will
			// have a terminate action.
		*/
		Channel_DQualify(t, teq_terminate);
	}

	port_unlatch(Channel_GetPort(J), 0);

	#ifdef EVMECH_EPOLL
	{
		close(J->choice.array.efd);
		close(J->choice.array.wfd);
	}
	#endif

	return;
}

/**
	// Collect and process events.
*/
static void
_array_flow(Array J)
{
	Channel t;

	array_start_cycle(J);

	/*
		// Check for Array termination.
	*/
	if (Channel_Terminating(J))
	{
		/*
			// terminate all channels
		*/
		_array_terminate((Channel) J);
	}
	else if (!Channel_PortLatched(J))
	{
		/*
			// kqueue file descriptor went bad.
			// Either a fork occurred or the user close()'d it.
		*/
		array_init(J);
		array_reload(J);
	}
	Channel_ClearDelta(J);

	/*
		// Enqueue changed channels to lltransfer.
		// *REQUIRES GIL*
	*/
	array_transfer_delta(J);

	if (Array_ShouldWait(J))
	{
		/*
			// Signals that an EVFILT_USER is necessary to cause it
			// to fall through.

			// If not set, we can avoid a syscall.
		*/
		J->choice.array.will_wait = 1;
	}

	/*
		// The GIL is no longer necessary, and concurrent
		// code can send signals to Channels as desired.
	*/

	Py_BEGIN_ALLOW_THREADS

	/*
		// The ring portion of the Channel objects are managed with the GIL.
		// t->next/t->prev CAN BE USED BY OTHER THREADS. DO NOT USE WITHOUT GIL.
	*/

	array_apply_delta(J);

	/* don't bother collecting/transforming if terminating */
	if (!Channel_Terminating(J))
	{
		unsigned int countdown = 3;

		/*
			// Wait *iff* there are no transfers available for processing.
		*/
		array_kevent_collect(J, Array_ShouldWait(J));
		J->choice.array.will_wait = 0; /* clear flag to avoid superfluous falls */

		array_kevent_transform(J);

		/*
			// Iff more kevents may exists.
			// The previous collection of events must be equal to the size of our
			// eventlist in order to run this loop.
		*/
		#ifdef EVMECH_EPOLL
			while (countdown)
			{
				array_kevent_collect(J, /* no wait */ 0);
				array_kevent_transform(J);
				--countdown;
			}
		#else
			while (Array_MaxCollected(J) && countdown)
			{
				array_kevent_collect(J, /* no wait */ 0);
				array_kevent_transform(J);
				--countdown;
			}
		#endif
	}

	/*
		// Prepare for array_next_kevent_slot()
	*/
	Array_ResetWindow(J);

	/*
		// Iterate over all the channels in the transfer list and process their events.
		// Sort the list into the I/O list.
	*/
	for (t = Channel_GetNextTransfer(J); t != (Channel) J; t = Channel_GetNextTransfer(t))
	{
		int polarity = !Channel_GetControl(t, ctl_polarity);

		#if F_TRACE(transfers)
			pchannel(t);
		#endif

		Array_IncrementTransferCount(J);

		if (Channel_ShouldTerminate(t))
		{
			/*
				// Disconnect from the kevent stream iff requeue is not configured.
			*/
			if (!Channel_GetControl(t, ctl_requeue))
			{
				kfilter_cancel(t, array_current_kevent_slot(J));
				Array_ConsumeKEventSlot(J);
			}

			Channel_NoteEvent(t, tev_terminate);

			/*
				// _flush will perform resource releases (close and ReleaseResource)

				// This is necessary for two reasons:
				// 1. User may need to refer to port.
				// 2. GIL is needed to release local resources.
			*/
		}
		else if (Channel_ShouldTransfer(t))
		{
			/*
				// Transfers are preempted by termination.
			*/
			io_status_t stat;
			uint32_t xfer = 0;
			Port p = Channel_GetPort(t);
			char *buf = Channel_GetResourceBuffer(t);

			/*
				// The max transfer window spans from the end of the current window
				// to the end of the resource. The stop is adjusted after the operation
				// cannot transfer anymore.
			*/
			uint32_t rsize = Channel_GetResourceSize(t);
			uint32_t pos = Channel_GetWindowStop(t);
			uint32_t request = rsize - pos;

			Channel_NoteEvent(t, tev_transfer);

			/*
				// Adjust by the channel's window.
			*/
			buf += (intptr_t) pos;

			/*
				// Acquire the IO operation from the ChannelType
				// using the polarity to select the proper function pointer.
			*/
			io_op_t io = Channel_GetInterface(t)->io[polarity];

			#if F_TRACE(transfers)
				#define trace(...) errpf(__VA_ARGS__)
				trace("\n\nReSIZE: %d; REQUEST: %d\n", rsize, request);
				pchannel(t);
			#else
				#define trace(...)
			#endif

			stat = io(p, &xfer, buf, request);
			Channel_ExpandWindow(t, xfer);
			if (Channel_GetWindowStop(t) > rsize)
				fprintf(stderr, "\nwindow stop exceeded resource\n");

			trace("XFER: %u %s\n", xfer, Channel_Sends(t) ? "OUT" : "IN");
			switch (stat)
			{
				/*
					// map io_status_t to state change.
				*/
				case io_flow:
					/*
						// Buffer exhausted and EAGAIN *not* triggered
						// Channel_XQualified(t, teq_transfer) == True
					*/
					Channel_INQualify(t, teq_transfer);
					trace(" FLOWS\n");
				break;

				case io_stop:
					/*
						// EAGAIN; wait for kernel event for continuation.
					*/
					Channel_XNQualify(t, teq_transfer);
					trace(" WOULDBLOCK\n");
				break;

				case io_terminate:
					/*
						// EOF condition or error returned.
						// It is possible that this has a transfer.
					*/
					Channel_XQualify(t, teq_terminate);
					Channel_NoteEvent(t, tev_terminate);

					if (!Channel_GetControl(t, ctl_requeue))
					{
						kfilter_cancel(t, array_current_kevent_slot(J));
						Array_ConsumeKEventSlot(J);
					}
					trace(" ERROR\n");
				break;
			}

			#if F_TRACE(transfers)
				trace("transform: ");
				pchannel(t);
				trace("\n ////// \n");
			#endif
			#undef trace
		}
		else
		{
			/*
				// No event. Filter.
			*/
			#if F_TRACE(no_events)
				pchannel(t);
			#endif
		}
	}

	/*
		// Perform any disconnects queued up in the loop.
	*/
	if (!Channel_Terminating(J))
		array_kevent_change(J);

	Py_END_ALLOW_THREADS
}

struct ChannelInterface
ArrayTIF = {
	{NULL, NULL},
	f_events, 1,
};

/**
	// Return an iterable to the collected events. &.kernel.Array.transfer
*/
static PyObj
array_transfer(PyObj self)
{
	Array J = (Array) self;
	PyObj rob;

	if (!Channel_InCycle(J))
		rob = PyTuple_New(0);
	else
		rob = new_jxi(J, 0);

	return(rob);
}

static PyObj
array_sizeof_transfer(PyObj self)
{
	Array J = (Array) self;

	if (!Channel_InCycle(J))
		return(PyLong_FromLong(0));

	return(PyLong_FromUnsignedLong(Array_GetTransferCount(J)));
}

static void
_array_flush(Array J)
{
	Channel t, next;

	/* REQUIRES GIL */

	t = Channel_GetNextTransfer(J);
	while (t != (Channel) J)
	{
		next = Channel_GetNextTransfer(t);
		Channel_SetNextTransfer(t, NULL);

		/*
			// Unconditionally collapse the window here.
			// We have the GIL so no concurrent Channel.acquire() calls are in progress.
			// If the user acquired the resource during the cycle, collapse will merely
			// set the stop to zero.

			// In cases where no transfer occurred, it's a no-op.
		*/
		Channel_CollapseWindow(t);

		if (Channel_HasEvent(t, tev_terminate))
		{
			/*
				// Release any resources owned by the channel.

				// In the case where the resource was acquired in the cycle,
				// we're not doing anything with the resource anyways, so get rid of it.
			*/
			Channel_ReleaseResource(t);
			Channel_ReleaseLink(t);
			port_unlatch(Channel_GetPort(t), Channel_Polarity(t));

			CHANNEL_DETACH(t);
			Array_DecrementChannelCount(J);

			/*
				// Emitted termination? Release reference to the channel.
			*/
			Py_DECREF(t);
		}
		else
		{
			/*
				// If the delta qualification exists, the user channel.acquire()'d during
				// the cycle, so don't release the new resource.
			*/
			int exhausted = !Channel_DQualified(t, teq_transfer)
				&& !Channel_IQualified(t, teq_transfer);

			if (exhausted)
			{
				/*
					// Exhaust event occurred, but no new resource supplied in cycle.
					// Release any internal resources.

					// The user has the option to acquire() a new buffer within and
					// after a cycle.
				*/
				Channel_ReleaseResource(t);
			}
		}

		/*
			// Cycle is over. Clear events.
		*/
		Channel_ClearEvents(t);

		t = next;
	}

	array_finish_cycle(J);
}

/**
	// Close file descriptors and release references; destroy entire ring.
*/
static PyObj
array_void(PyObj self)
{
	Array J = (Array) self;
	Channel t;

	/* GIL Required */

	if (Array_Cycling(J))
		array_finish_cycle(J);

	for (t = J->next; t != (Channel) J; t = t->next)
	{
		Port p = Channel_GetPort(t);
		/*
			// Clear any transfer state.
		*/
		Channel_IQualify(t, teq_terminate);
		Channel_SetNextTransfer(t, NULL);
		port_unlatch(p, 0);
		p->cause = kc_void;

		t->prev->next = NULL;
		t->prev = NULL;
		Py_DECREF(t);

		/*
			// The Array and Port references will be cleared by dealloc.
		*/
	}
	t->next = NULL;

	J->next = (Channel) J;
	J->prev = (Channel) J;
	Array_ResetTransferCount(J);
	Array_ResetChannelCount(J);
	port_unlatch(Channel_GetPort(J), 0);

	#ifdef EVMECH_EPOLL
		close(J->choice.array.efd);
		close(J->choice.array.wfd);
	#endif

	Py_RETURN_NONE;
}

/**
	// Begin a transfer processing cycle.
*/
static PyObj
array_enter(PyObj self)
{
	Array J = (Array) self;

	if (Channel_Terminating(J) && !Channel_PortLatched(J))
	{
		PyErr_SetChannelTerminatedError(J);
		return(NULL);
	}

	if (Channel_InCycle(J))
	{
		PyErr_SetString(PyExc_RuntimeError,
			"cycle must be completed before starting another");
		return(NULL);
	}

	_array_flow(J);

	Py_INCREF(self);
	return(self);
}

/**
	// Close a transfer processing cycle.
*/
static PyObj
array_exit(PyObj self, PyObj args)
{
	Array J = (Array) self;

	if (Channel_InCycle(J))
		_array_flush(J);

	Py_RETURN_NONE;
}

static PyMethodDef
array_methods[] = {
	{"resize_exoresource",
		(PyCFunction) array_resize_exoresource, METH_VARARGS,
		PyDoc_STR(
			"In cases where resize fails, the old size will be returned "
			"and no error will be mentioned. For Arrays, resize *must* "
			"be called outside of a cycle--outside of the context manager block."
			"\n"
			"[Parameters]\n"
			"/(&int)`max_events`/\n"
			"\tThe maximum number events to transfer.\n"
			"\n"
			"[Return]\n"
			"The new size as an &int.\n"
		)
	},

	{"acquire",
		(PyCFunction) array_acquire, METH_O,
		PyDoc_STR(
			"Acquires the Channel so that it may participate in &Array cycles.\n"

			"[Parameters]\n"
			"/channel/\n"
			"\tThe &Channel that will be managed by this Array.\n"
		)
	},

	{"void",
		(PyCFunction) array_void, METH_NOARGS,
		PyDoc_STR(
			"Void all attached channels in an unfriendly manner.\n"
			"Terminate events will not be generated, the current cycle, if any, will be exited.\n\n"
			"! NOTE:\n"
			"\tNormally, this function should be only be used by child processes destroying the parent's state."
		)
	},

	{"force",
		(PyCFunction) array_force, METH_NOARGS,
		PyDoc_STR(
			"Causes the next cycle to not wait for events. If a cycle has been started\n"
			"and is currently waiting for events, force will cause it to stop waiting for events.\n"
			"\n"
			"Returns the Array instance being forced for method chaining.\n"
		)
	},

	{"transfer",
		(PyCFunction) array_transfer, METH_NOARGS,
		PyDoc_STR(
			"Returns an iterable producing the channels that have events.\n"
		)
	},

	{"sizeof_transfer",
		(PyCFunction) array_sizeof_transfer, METH_NOARGS,
		PyDoc_STR(
			"Get the number of transfers currently available; `0` if there is no transfers."
			"! NOTE:\n"
			"\tCurrently unavailable.\n\n"
			"\n"
			"Returns the number of Channels with events this cycle.\n"
		)
	},

	{"__enter__",
		(PyCFunction) array_enter, METH_NOARGS,
		PyDoc_STR("Enter a Array cycle allowing channelion state to be examined.")
	},

	{"__exit__",
		(PyCFunction) array_exit, METH_VARARGS,
		PyDoc_STR("Exit the Array cycle destroying the channelion state.")
	},

	{NULL,},
};

static PyMemberDef array_members[] = {
	{"volume", T_PYSSIZET, offsetof(struct Array, choice.array.nchannels), READONLY,
		PyDoc_STR("The number of channels being managed by the Array instance.")},
	{NULL,},
};

static PyObj
array_get_resource(PyObj self, void *_)
{
	PyObj l;
	Py_ssize_t i = 0;
	Array J = (Array) self;
	Channel t = J->next;

	/*
		// Requires GIL.
	*/

	l = PyList_New(Array_GetChannelCount(J));
	while (t != (Channel) J)
	{
		PyObj ob = (PyObj) t;

		Py_INCREF(ob);
		PyList_SET_ITEM(l, i, ob);

		++i;
		t = t->next;
	}

	return(l);
}

static PyGetSetDef array_getset[] = {
	{"resource", array_get_resource, NULL,
		PyDoc_STR("A &list of all Channels attached to this Array instance, save the Array instance.")
	},
	{NULL,},
};

static void
array_dealloc(PyObj self)
{
	Array J = (Array) self;
	PyMem_Free(Array_GetKEvents(J));
	channel_dealloc(self);
}

static PyObj
array_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	Array J;
	Port p;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL);

	J = (Array) alloci((PyObj) subtype, &p);
	if (J == NULL)
		return(NULL);
	p->type = kt_kqueue;
	p->freight = Type_GetInterface(subtype)->ti_freight;

	Channel_SetArray(J, J);
	Channel_XQualify(J, teq_transfer);
	Channel_SetControl(J, ctl_polarity);

	Array_ResetChannelCount(J);
	Array_ResetTransferCount(J);

	/*
		// For Arrays, the Window's Stop is the size of malloc / sizeof(struct kevent)
	*/
	Channel_SetWindow(J, 0, CONFIG_DEFAULT_ARRAY_SIZE);
	Array_SetKEvents(J, PyMem_Malloc(sizeof(kevent_t) * Channel_GetWindowStop(J)));

	J->next = (Channel) J;
	J->prev = (Channel) J;

	array_init(J);

	return((PyObj) J);
}

PyDoc_STRVAR(Array_doc,
"The Array implementation, &.abstract.Array, for performing I/O with the kernel.");

ChannelPyTypeObject
ArrayType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Array"),  /* tp_name */
	sizeof(struct Array),         /* tp_basicsize */
	0,                            /* tp_itemsize */
	array_dealloc,                /* tp_dealloc */
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
	Py_TPFLAGS_DEFAULT,           /* tp_flags */
	Array_doc,                    /* tp_doc */
	NULL,                         /* tp_traverse */
	NULL,                         /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	NULL,                         /* tp_iter */
	NULL,                         /* tp_iternext */
	array_methods,                /* tp_methods */
	array_members,                /* tp_members */
	array_getset,                 /* tp_getset */
	&ChannelType.typ,             /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	array_new,                    /* tp_new */
},
	&ArrayTIF,
};

static PyObj
_talloc_sockets_input(PyObj module, PyObj param)
{
	const PyObj typ = &SocketsType;
	Port p = NULL;
	PyObj rob = NULL;
	kport_t fd = -1;

	fd = PyLong_AsLong(param);
	if (PyErr_Occurred())
		return(0);

	rob = alloci(typ, &p);
	if (rob == NULL)
		return(NULL);

	p->freight = f_sockets;
	p->point = fd;
	ports_identify_socket(p);
	if (p->cause == kc_pyalloc)
		p->cause = kc_none;

	return(rob);
}

static PyObj
_talloc_octets_input(PyObj module, PyObj param)
{
	const PyObj typ = &OctetsType;
	Port p = NULL;
	PyObj rob = NULL;
	kport_t fd = -1;

	fd = PyLong_AsLong(param);
	if (PyErr_Occurred())
		return(0);

	rob = alloci(typ, &p);
	if (rob == NULL)
		return(NULL);

	p->freight = f_octets;
	p->point = fd;
	ports_identify_input(p);
	if (p->cause == kc_pyalloc)
		p->cause = kc_none;

	return(rob);
}

static PyObj
_talloc_octets_output(PyObj module, PyObj param)
{
	const PyObj typ = &OctetsType;
	Port p = NULL;
	PyObj rob = NULL;
	kport_t fd = -1;

	fd = PyLong_AsLong(param);
	if (PyErr_Occurred())
		return(0);

	rob = alloco(typ, &p);
	if (rob == NULL)
		return(NULL);

	p->freight = f_octets;
	p->point = fd;
	ports_identify_output(p);
	if (p->cause == kc_pyalloc)
		p->cause = kc_none;

	return(rob);
}

static PyObj
_talloc_datagrams_socket(PyObj module, PyObj param)
{
	const PyObj typ = &DatagramsType;
	Port p = NULL;
	PyObj rob = NULL;
	kport_t fd = -1;

	fd = PyLong_AsLong(param);
	if (PyErr_Occurred())
		return(0);

	rob = allocio(typ, typ, &p);
	if (rob == NULL)
		return(NULL);

	p->freight = f_datagrams;
	p->point = fd;
	ports_identify_socket(p);
	if (p->cause == kc_pyalloc)
		p->cause = kc_none;

	return(rob);
}

static PyObj
_talloc_octets_socket(PyObj module, PyObj param)
{
	const PyObj typ = &OctetsType;
	Port p = NULL;
	PyObj rob = NULL;
	kport_t fd = -1;

	fd = PyLong_AsLong(param);
	if (PyErr_Occurred())
		return(0);

	rob = allocio(typ, typ, &p);
	if (rob == NULL)
		return(NULL);

	p->freight = f_octets;
	p->point = fd;
	ports_identify_socket(p);
	if (p->cause == kc_pyalloc)
		p->cause = kc_none;

	return(rob);
}

static PyObj
_talloc_ports_socket(PyObj module, PyObj param)
{
	const PyObj typ = &PortsType;
	Port p = NULL;
	PyObj rob = NULL;
	kport_t fd = -1;

	fd = PyLong_AsLong(param);
	if (PyErr_Occurred())
		return(0);

	rob = allocio(typ, typ, &p);
	if (rob == NULL)
		return(NULL);

	p->freight = f_ports;
	p->point = fd;
	ports_identify_socket(p);
	if (p->cause == kc_pyalloc)
		p->cause = kc_none;

	return(rob);
}

/**
	// Access to channel allocators.
*/
#define MODULE_FUNCTIONS() \
	PYMETHOD( \
		alloc_input, _talloc_octets_input, METH_O, \
		"Allocate Octets for the given read-only file descriptor.") \
	PYMETHOD( \
		alloc_output, _talloc_octets_output, METH_O, \
		"Allocate Octets for the given write-only file descriptor.") \
	PYMETHOD( \
		alloc_service, _talloc_sockets_input, METH_O, \
		"Allocate Sockets for the given listening socket file descriptor.") \
	PYMETHOD( \
		alloc_datagrams, _talloc_datagrams_socket, METH_O, \
		"Allocate a Datagrams pair for the given socket file descriptor.") \
	PYMETHOD( \
		alloc_octets, _talloc_octets_socket, METH_O, \
		"Allocate an Octets pair for the given socket file descriptor.") \
	PYMETHOD( \
		alloc_ports, _talloc_ports_socket, METH_O, \
		"Allocate a Ports pair for the given socket file descriptor.")

#include <fault/python/module.h>

INIT(module, 0, PyDoc_STR("Asynchronous System I/O"))
{
	/*
		// Safely shared by subinterpreters.
	*/
	if (KP == NULL)
	{
		PyObj cap;
		cap = PyImport_ImportAdjacentEx(module, "kernel", "_kports_api");
		if (cap == NULL)
			goto error;

		KP = PyCapsule_GetPointer(cap, "_kports_api");
		Py_DECREF(cap);
		if (KP == NULL)
			goto error;
	}

	if (EP == NULL)
	{
		PyObj cap;
		cap = PyImport_ImportAdjacentEx(module, "network", "_endpoint_api");
		if (cap == NULL)
			goto error;

		EP = PyCapsule_GetPointer(cap, "_endpoint_api");
		Py_DECREF(cap);
		if (EP == NULL)
			goto error;
	}

	#if FV_INJECTIONS()
		/*
			// Need this to help with the skip condition in the tests.
		*/
		#ifdef F_SETNOSIGPIPE
			PyModule_AddIntConstant(module, "F_SETNOSIGPIPE", 1);
		#else
			PyModule_AddIntConstant(module, "F_SETNOSIGPIPE", 0);
		#endif
	#endif

	if (PyType_Ready(&(jxi_type)))
		goto error;

	/*
		// Initialize Channel types.
	*/
	#define ID(NAME, IGNORED) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(module, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		CHANNEL_TYPES()
		PY_TYPES()
	#undef ID

	/**
		// Setup exception instances.
	*/
	{
		PyExc_TransitionViolation = PyErr_NewException(PYTHON_MODULE_PATH("TransitionViolation"), NULL, NULL);
		if (PyExc_TransitionViolation == NULL)
			goto error;

		if (PyModule_AddObject(module, "TransitionViolation", PyExc_TransitionViolation) < 0)
		{
			Py_DECREF(PyExc_TransitionViolation);
			PyExc_TransitionViolation = NULL;
			goto error;
		}
	}

	return(0);

	error:
	{
		return(-1);
	}
}
