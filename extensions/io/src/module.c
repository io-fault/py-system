/**
	# Kernel based traffic implementation using kqueue and epoll.
	# See &.documentation.mechanics for more information.
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

#include "module.h"
#include "python.h"
#include "port.h"
#include "endpoint.h"

/* Number of kevent structs to allocate when working with kevent(). */
#ifndef CONFIG_DEFAULT_JUNCTION_SIZE
	#define CONFIG_DEFAULT_JUNCTION_SIZE 16
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
	# Get the name of the errno.
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
		case f_wolves:
			return 'w';
		case f_void:
			return 'v';
		case f_transits:
			return 't'; /* Junction */
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
		case f_wolves:
			return "wolves";
		case f_void:
			return "void";
		case f_transits:
			return "transits";
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
	# [Parameters]
	# /ss/
		# Source address structure.
	# /dst/
		# Destination memory buffer for the interface string
		# to be written to.
	# /dstlen/
		# Length of &dst string.

	# [Return]
	# The &ss parameter is the destination of the interface
	# described in &dst.
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
	static void ptransit(Channel);
	static void pkevent(kevent_t *);
#endif

static kcall_t
kcall_id(char *str)
{
	/*
		# Naturally a hash is better, but
		# this is only used during Port.__new__,
		# which is rarely used.
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

static PyObj new_array = NULL; /* array.array("i", [-1]).__mul__ */
static PyObj polarity_objects[2] = {NULL,NULL};

#define PyErr_SetChannelTerminatedError(t) \
	PyErr_SetString(PyExc_ChannelionViolation, "already terminated")
#define PyErr_SetChannelResourceError(t) \
	PyErr_SetString(PyExc_ChannelionViolation, "resource already present")

static int
socket_receive_buffer(kpoint_t kp)
{
	int size = -1;
	socklen_t ssize = sizeof(size);
	getsockopt(kp, SOL_SOCKET, SO_RCVBUF, &size, &ssize);
	return(size);
}

static int
socket_send_buffer(kpoint_t kp)
{
	int size = -1;
	socklen_t ssize = sizeof(size);
	getsockopt(kp, SOL_SOCKET, SO_SNDBUF, &size, &ssize);
	return(size);
}

static PyObj
path(kpoint_t kp)
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
				# Ignore error; file path not available.
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
			"Ports with Junction attached Channels should never be shattered as it causes the event subscription to be lost. "
			"Subsequently, the Channel will remain in the Junction ring until terminated by user code.\n\n"
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
	{"id", T_KPOINT, offsetof(struct Port, point), READONLY,
		PyDoc_STR(
			"The identifier of the port used to communicate with the kernel."
	)},
	{"error_code", T_KERROR, offsetof(struct Port, error), READONLY,
		PyDoc_STR(
			"The error code associated with the Port."
	)},

	/*
		# Some aliases to lend toward convention.
	*/

	{"fileno", T_KPOINT, offsetof(struct Port, point), READONLY,
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
			"The system library call or traffic.kernel call performed that caused the error associated with the Port.\n"
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
	# String representation suitable for text based displays.
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
	kpoint_t kid = -1;
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
	p->freight = f_wolves;
	p->latches = 0;

	return(rob);
}

PyDoc_STRVAR(port_doc,
"Port(id = -1, error_code = 0, call = 'none', freight = 'wolves')\n\n");

static void
port_dealloc(PyObj self)
{
	Port p = (Port) self;

	/**
		# Junction instances hold a reference to a point until it is
		# explicitly closed. At that point, it is detached from the ring,
		# and Junction's reference is released.
	*/
	if (p->latches && p->point != kp_invalid && p->cause != kc_leak)
	{
		#if PY_MAJOR_VERSION > 2
		PyErr_WarnFormat(PyExc_ResourceWarning, 0, "port was latched at deallocation");
		#endif
	}
}

PyTypeObject
PortType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Port"),   /* tp_name */
	sizeof(struct Port),   /* tp_basicsize */
	0,                     /* tp_itemsize */
	port_dealloc,          /* tp_dealloc */
	NULL,                  /* tp_print */
	NULL,                  /* tp_getattr */
	NULL,                  /* tp_setattr */
	NULL,                  /* tp_compare */
	port_repr,             /* tp_repr */
	NULL,                  /* tp_as_number */
	NULL,                  /* tp_as_sequence */
	NULL,                  /* tp_as_mapping */
	NULL,                  /* tp_hash */
	NULL,                  /* tp_call */
	port_str,              /* tp_str */
	NULL,                  /* tp_getattro */
	NULL,                  /* tp_setattro */
	NULL,                  /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,    /* tp_flags */
	port_doc,              /* tp_doc */
	NULL,                  /* tp_traverse */
	NULL,                  /* tp_clear */
	NULL,                  /* tp_richcompare */
	0,                     /* tp_weaklistoffset */
	NULL,                  /* tp_iter */
	NULL,                  /* tp_iternext */
	port_methods,          /* tp_methods */
	port_members,          /* tp_members */
	port_getset,           /* tp_getset */
	NULL,                  /* tp_base */
	NULL,                  /* tp_dict */
	NULL,                  /* tp_descr_get */
	NULL,                  /* tp_descr_set */
	0,                     /* tp_dictoffset */
	NULL,                  /* tp_init */
	NULL,                  /* tp_alloc */
	port_new,              /* tp_new */
};

static PyMethodDef endpoint_methods[] = {
	{NULL,}
};

static PyObj
endpoint_get_address_type(PyObj self, void *_)
{
	Endpoint E = (Endpoint) self;

	switch (Endpoint_GetAddress(E)->ss_family)
	{
		#define A(AF) \
			case AF##_pf: \
				return(PyUnicode_FromString(AF##_name)); \
			break;

			ADDRESSING()
		#undef A
	}

	Py_RETURN_NONE;
}

static PyObj
endpoint_get_interface(PyObj self, void *_)
{
	char addrstr[1024];
	Endpoint E = (Endpoint) self;

	sockaddr_interface(Endpoint_GetAddress(E), addrstr, sizeof(addrstr));
	return(PyUnicode_FromString(addrstr));
}

static PyObj
endpoint_get_port(PyObj self, void *_)
{
	Endpoint E = (Endpoint) self;
	struct aport_t port;

	switch (sockaddr_port(Endpoint_GetAddress(E), &port, sizeof(port)))
	{
		case aport_kind_numeric2:
			return(PyLong_FromLong(port.data.numeric2));
		break;

		case aport_kind_filename:
			/* xxx: encoding */
			return(PyUnicode_FromString(port.data.filename));
		break;

		case aport_kind_none:
		default:
			Py_RETURN_NONE;
		break;
	}
}

static PyObj
endpoint_get_pair(PyObj self, void *_)
{
	Endpoint E = (Endpoint) self;
	char buf[PATH_MAX];
	struct aport_t port;
	PyObj rob;

	sockaddr_interface(Endpoint_GetAddress(E), buf, sizeof(buf));
	port.kind = sockaddr_port(Endpoint_GetAddress(E), &port, sizeof(port));

	switch (port.kind)
	{
		case aport_kind_numeric2:
			rob = Py_BuildValue("(sl)", buf, (unsigned int) port.data.numeric2);
		break;

		case aport_kind_filename:
			rob = Py_BuildValue("(ss)", buf, port.data.filename);
		break;

		case aport_kind_none:
		default:
			rob = Py_None; Py_INCREF(rob); break;
	}

	return(rob);
}

static PyGetSetDef
endpoint_getset[] = {
	{"address_type", endpoint_get_address_type, NULL,
		PyDoc_STR(
			"The type of addressing used to reference the endpoint.\n"
			"One of `'ip6'`, `'ip4'`, `'local'`, or `None` if irrelevant.")
	},

	{"interface", endpoint_get_interface, NULL,
		PyDoc_STR(
			"The local endpoint of the transit. Normally regarding socket connections.\n"
			"If the connection is to a local socket, the interface will be the directory containing the socket file.")
	},

	{"port", endpoint_get_port, NULL,
		PyDoc_STR(
			"The port of the endpoint as an &int. &None if none\n"
			"or if the concept of a port does not apply to the endpoint's domain.")
	},

	#if 0
		{"port_int", endpoint_get_port_string, NULL,
			PyDoc_STR(
				"The port of the endpoint as an `int`. `None` if none\n"
				"or if the concept of a port does not apply to the endpoint's domain.")
		},
	#endif

	{"pair", endpoint_get_pair, NULL,
		PyDoc_STR(
			"A tuple consisting of the interface and port attributes.\n"
		)
	},

	{NULL,},
};

static PyObj
endpoint_richcompare(PyObj self, PyObj x, int op)
{
	Endpoint a = (Endpoint) self, b = (Endpoint) x;
	PyObj rob;

	if (!PyObject_IsInstance(x, ((PyObj) &EndpointType)))
	{
		Py_INCREF(Py_NotImplemented);
		return(Py_NotImplemented);
	}

	switch (op)
	{
		case Py_NE:
		case Py_EQ:
			rob = Py_False;

			if (Endpoint_GetLength(a) == Endpoint_GetLength(b))
			{
				if (memcmp((char *) Endpoint_GetAddress(a), (char *) Endpoint_GetAddress(b), Endpoint_GetLength(a)) == 0)
				{
					rob = Py_True;
					Py_INCREF(rob);
					break;
				}
			}

			if (op == Py_NE)
			{
				/*
					# Invert result.
				*/
				rob = (rob == Py_True) ? Py_False : Py_True;
			}
			Py_INCREF(rob);
		break;

		default:
			PyErr_SetString(PyExc_TypeError, "endpoint only supports equality");
			rob = NULL;
		break;
	}

	return(rob);
}

/**
	# String representation suitable for text based displays.
*/
static PyObj
endpoint_str(PyObj self)
{
	Endpoint E = (Endpoint) self;
	char buf[PATH_MAX];
	struct aport_t port;
	PyObj rob;

	sockaddr_interface(Endpoint_GetAddress(E), buf, sizeof(buf));

	port.kind = sockaddr_port(Endpoint_GetAddress(E), &port, sizeof(port));
	switch (port.kind)
	{
		case aport_kind_numeric2:
			rob = PyUnicode_FromFormat("[%s]:%d", buf, (int) port.data.numeric2);
		break;

		case aport_kind_filename:
			rob = PyUnicode_FromFormat("%s/%s", buf, port.data.filename);
		break;

		case aport_kind_none:
		default:
			rob = PyUnicode_FromFormat("%s", buf);
		break;
	}

	return(rob);
}

#define A(DOMAIN) \
/**\
	# Constructor.\
*/\
static PyObj \
endpoint_new_##DOMAIN(PyTypeObject *subtype, PyObj rep) \
{ \
	const int addrlen = sizeof(DOMAIN##_addr_t); \
	int r; \
	\
	PyObj rob; \
	Endpoint E; \
	\
	r = addrlen / subtype->tp_itemsize; \
	if (r * subtype->tp_itemsize <= addrlen) \
		++r; \
	\
	rob = subtype->tp_alloc(subtype, r); \
	if (rob == NULL) \
		return(NULL); \
	E = (Endpoint) rob; \
	\
	if (! (DOMAIN##_from_object(rep, (DOMAIN##_addr_t *) Endpoint_GetAddress(E)))) \
	{ \
		Py_DECREF(rob); \
		return(NULL); \
	} \
	E->len = addrlen; \
	\
	return(rob); \
}

ADDRESSING()
#undef A

static Endpoint
endpoint_create(if_addr_ref_t addr, socklen_t addrlen)
{
	#define endpoint_alloc(x) EndpointType.tp_alloc(&EndpointType, x)

	const int itemsize = EndpointType.tp_itemsize;
	int r;
	Endpoint E;

	r = addrlen / itemsize;
	if (r * itemsize <= addrlen)
		++r;

	PYTHON_RECEPTACLE(NULL, ((PyObj *) &E), endpoint_alloc, r);
	if (E == NULL)
		return(NULL);

	E->len = addrlen;
	memcpy(Endpoint_GetAddress(E), addr, addrlen);

	return(E);

	#undef endpoint_alloc
}

static PyObj
endpoint_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"domain", "address", NULL};
	char *domain;
	PyObj rob = NULL, address;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "sO", kwlist, &domain, &address))
		return(NULL);

	/*
		# XXX: While it's not expected for endpoint_new to occur often, this should be a hash.
	*/
	if (0) ;
	#define A(DOMAIN) \
		else if (strcmp(domain, #DOMAIN) == 0) \
			rob = endpoint_new_##DOMAIN(subtype, address);
		ADDRESSING()
	#undef A
	else
	{
		PyErr_Format(PyExc_ValueError, "unknown address domain: %s", domain);
	}

	return(rob);
}

PyDoc_STRVAR(endpoint_doc, "Endpoint(domain, address)\n\n""\n");

PyTypeObject
EndpointType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Endpoint"), /* tp_name */
	sizeof(struct Endpoint), /* tp_basicsize */
	sizeof(void *),          /* tp_itemsize */
	NULL,                    /* tp_dealloc */
	NULL,                    /* tp_print */
	NULL,                    /* tp_getattr */
	NULL,                    /* tp_setattr */
	NULL,                    /* tp_compare */
	NULL,                    /* tp_repr */
	NULL,                    /* tp_as_number */
	NULL,                    /* tp_as_sequence */
	NULL,                    /* tp_as_mapping */
	NULL,                    /* tp_hash */
	NULL,                    /* tp_call */
	endpoint_str,            /* tp_str */
	NULL,                    /* tp_getattro */
	NULL,                    /* tp_setattro */
	NULL,                    /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,      /* tp_flags */
	endpoint_doc,            /* tp_doc */
	NULL,                    /* tp_traverse */
	NULL,                    /* tp_clear */
	endpoint_richcompare,    /* tp_richcompare */
	0,                       /* tp_weaklistoffset */
	NULL,                    /* tp_iter */
	NULL,                    /* tp_iternext */
	endpoint_methods,        /* tp_methods */
	NULL,                    /* tp_members */
	endpoint_getset,         /* tp_getset */
	NULL,                    /* tp_base */
	NULL,                    /* tp_dict */
	NULL,                    /* tp_descr_get */
	NULL,                    /* tp_descr_set */
	0,                       /* tp_dictoffset */
	NULL,                    /* tp_init */
	NULL,                    /* tp_alloc */
	endpoint_new,            /* tp_new */
};

#include "transit.h"

/**
	# Datagrams (struct)&Channel Structure.
*/
struct Datagrams {
	Channel_HEAD
	int pf; /* Necessary for resource allocation (ip4 vs ip6) */
};

#define INIT_TRANSIT(t, J) do { \
	Channel_SetJunction(t, J); \
	Channel_SetNextTransfer(t, NULL); \
	Channel_SetResource(t, NULL); \
	Channel_SetLink(t, NULL); \
	Channel_ClearWindow(t); \
	Channel_State(t) = 0; \
	Channel_SetDelta(t, 0); \
	Channel_SetEvents(t, 0); \
} while(0)

#define INIT_INPUT_TRANSIT(t, J) do { \
	INIT_TRANSIT(t, J); \
	Channel_SetControl(t, ctl_polarity); \
} while(0)

#define INIT_OUTPUT_TRANSIT(t, J) do { \
	INIT_TRANSIT(t, J); \
	Channel_NulControl(t, ctl_polarity); \
} while(0)

#define Junction_Cycling(J)               (J->lltransfer != NULL)
#define Junction_GetChannelCount(J)       ((J->choice.junction.ntransits))
#define Junction_ResetChannelCount(J)     ((J->choice.junction.ntransits) = 0)
#define Junction_IncrementChannelCount(J) (++ Junction_GetChannelCount(J))
#define Junction_DecrementChannelCount(t) (-- Junction_GetChannelCount(t))

static int junction_fall(Junction, int);

/* Append to the end of the doubly linked list; requires GIL. */
#define Channel_EnqueueDelta(t) do { \
	Junction J = (Channel_GetJunction(t)); \
	if (Channel_GetDelta(t) != 0 && ((Junction)(t)) != J) { \
		TRANSIT_RELOCATE_SEGMENT_BEFORE(J, (t), (t)); \
		junction_fall(J, 0); \
	} \
} while(0)

/**
	# Transfer Linked List management.

	# The transfer linked list is the list that manages the transits
	# that have significant state changes: transfer occurred--with exhaustion--or termination.
*/
#define Channel_GetNextTransfer(t)       ((t)->lltransfer)
#define Channel_IsTransfer(t)            (Channel_GetNextTransfer(t) != NULL)
#define Channel_SetNextTransfer(t, sett) (Channel_GetNextTransfer(t) = sett)

#define Junction_AddTransfer(J, t) \
	do { \
		if (Channel_GetNextTransfer(t) == NULL) { \
			Channel_SetNextTransfer(t, Channel_GetNextTransfer(J)); \
			Channel_SetNextTransfer(J, t); \
		} \
	} while(0)

/**
	# Junction only holds one reference to transits no matter
	# how often they're referenced.
*/
#define TRANSIT_JOIN(PREV, NEXT) \
	do { \
		PREV->next = (Channel) NEXT; \
		NEXT->prev = (Channel) PREV; \
	} while (0)

/**
	# Extends ring from behind. (Relative to TARGET, usually a Junction instance)
*/
#define TRANSIT_ATTACH_SEGMENT_BEFORE(TARGET, FIRST, LAST) do { \
	Channel T_prev = TARGET->prev; \
	FIRST->prev = T_prev; \
	LAST->next = (Channel) TARGET; \
	TARGET->prev = LAST; \
	T_prev->next = FIRST; \
} while (0)

/**
	# Extends ring from front.
*/
#define TRANSIT_ATTACH_SEGMENT_AFTER(TARGET, FIRST, LAST) do { \
	FIRST->prev = (Channel) TARGET; \
	LAST->next = (Channel) TARGET->next; \
	TARGET->next->prev = (Channel) LAST; \
	TARGET->next = (Channel) FIRST; \
} while (0)

#define TRANSIT_RELOCATE_SEGMENT_AFTER(TARGET, FIRST, LAST) do { \
	TRANSIT_DETACH_SEGMENT(FIRST, LAST); \
	TRANSIT_ATTACH_SEGMENT_AFTER(TARGET, FIRST, LAST); \
} while (0)

#define TRANSIT_DETACH_SEGMENT(FIRST, LAST) do { \
	Channel f_prev = FIRST->prev, l_next = LAST->next; \
	f_prev->next = (Channel) l_next; \
	l_next->prev = (Channel) f_prev; \
} while(0)

#define TRANSIT_RELOCATE_SEGMENT_BEFORE(TARGET, FIRST, LAST) do { \
	TRANSIT_DETACH_SEGMENT(FIRST, LAST); \
	TRANSIT_ATTACH_SEGMENT_BEFORE(TARGET, FIRST, LAST); \
} while (0)

#define TRANSIT_DETACH(TRANSIT) do { \
	TRANSIT_DETACH_SEGMENT(TRANSIT, TRANSIT); \
	TRANSIT->prev = NULL; TRANSIT->next = NULL; \
} while(0)

#define TRANSIT_ATTACH_BEFORE(TARGET, TRANSIT) \
	TRANSIT_ATTACH_SEGMENT_BEFORE(TARGET, TRANSIT, TRANSIT)
#define TRANSIT_ATTACH_AFTER(TARGET, TRANSIT) \
	TRANSIT_ATTACH_SEGMENT_AFTER(TARGET, TRANSIT, TRANSIT)
#define TRANSIT_ATTACH(TRANSIT) \
	TRANSIT_ATTACH_BEFORE(Channel_GetJunction(TRANSIT), TRANSIT)

#define Channel_GetResourceArray(TYP, T) ((TYP *)(Channel_GetResourceBuffer(T) + (Channel_GetWindowStop(T) * sizeof(TYP))))
#define Channel_GetRemainder(TYP, T) ((Channel_GetResourceSize(T) / sizeof(TYP)) - Channel_GetWindowStop(T))

/**
	# Requires GIL.
*/
static void
Channel_ReleaseResource(Channel t)
{
	/**
		# Free any Python resources associated with the transit.
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
	# Requires GIL. Decrements the link reference and sets the field to &NULL.
*/
static void
Channel_ReleaseLink(Channel t)
{
	/**
		# Free any Python resources associated with the transit.
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
		wp.point = Channel_GetJunction(t)->choice.junction.wfd;
		p = &wp;
	}
	else
		p = Channel_GetJunctionPort(t);

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
		wp.point = Channel_GetJunction(t)->choice.junction.wfd;
		p = &wp;
	}
	else
		p = Channel_GetJunctionPort(t);

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
	# Print transit structure to standard error.
	# Used for tracing operations during debugging.
*/
static void
ptransit(Channel t)
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
pkevents(const char *where, Junction J)
{
	int i;
	errpf("[%s]\n", where);
	for (i = 0; i < Channel_GetWindowStart(J); ++i)
	{
		pkevent(&(Junction_GetKEvents(J)[i]));
		ptransit((Channel) (Junction_GetKEvents(J)[i]).udata);
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
	# &PyTypeObject extension structure for configuring the I/O callbacks to use.
*/
struct ChannelInterface
ChannelTIF = {
	{NULL, NULL},
	f_void, 0,
};

/**
	# Iterator created by &.kernel.Junction.transfer().

	# Given that this is restricted to iteration,
	# the type is *not* exposed on the module.
*/
struct jxi {
	PyObject_HEAD

	/**
		# Subject &Junction producing events.
	*/
	Junction J;

	/**
		# Position in transfer linked list.
	*/
	Channel t;
};

/**
	# Iterator protocol next implementation.
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
			"junction transfer iterator used outside of cycle");
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
	sizeof(struct jxi),   /* tp_basicsize */
	0,                    /* tp_itemsize */
	jxi_dealloc,          /* tp_dealloc */
	NULL,                 /* tp_print */
	NULL,                 /* tp_getattr */
	NULL,                 /* tp_setattr */
	NULL,                 /* tp_compare */
	NULL,                 /* tp_repr */
	NULL,                 /* tp_as_number */
	NULL,                 /* tp_as_sequence */
	NULL,                 /* tp_as_mapping */
	NULL,                 /* tp_hash */
	NULL,                 /* tp_call */
	NULL,                 /* tp_str */
	NULL,                 /* tp_getattro */
	NULL,                 /* tp_setattro */
	NULL,                 /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,   /* tp_flags */
	jxi_doc,              /* tp_doc */
	NULL,                 /* tp_traverse */
	NULL,                 /* tp_clear */
	NULL,                 /* tp_richcompare */
	0,                    /* tp_weaklistoffset */
	jxi_iter,             /* tp_iter */
	jxi_next,             /* tp_iternext */
	NULL,
};

static PyObj
new_jxi(Junction J, int polarity)
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
transit_can_acquire(Channel t)
{
	/*
		# This should be called after receiving an exhaust event, which
		# removes this internal flag.
	*/
	if (Channel_IQualified(t, teq_transfer))
	{
		/*
			# This needs to error out as the traffic flow may be using the
			# transit's resource at this particular moment.
		*/
		PyErr_SetChannelResourceError(t);
		return(0);
	}

	return(1);
}

/**
	# Acquire a resource for faciliting a transfer. Qualifies the Channel for transfers.

	# The given &resource object is set on the &Channel and the memory buffer is filled
	# out based on the direction of the transit. If the Channel has been acquired
	# by a &Junction, the Channel will be marked and enqueued for a subsequent transfer
	# attempt. Otherwise, the Channel is qualified for transfer allowing the subsequent
	# &junction_acquire to ready a transfer.
*/
static PyObj
transit_acquire(PyObj self, PyObj resource)
{
	Channel t = (Channel) self;

	if (Channel_Terminating(t))
	{
		/*
			# Ignore resource acquisitions if terminating.
			# In cases where Junction is running in a parallel loop,
			# it is possible for a terminate event to follow exhaustion.

			# Essentially, raising an exception here would a race condition
			# for proper resource acquisition chains.
		*/
		Py_RETURN_NONE;
	}

	/*
		# Raise ResourceError; user isn't paying attention to exhaust events.
	*/
	if (!transit_can_acquire(t))
	{
		/*
			# Exhaust events are the only safe way that we can invalidate
			# resources without introducing addtional locks.
		*/
		return(NULL);
	}

	/*
		# REQUIRES GIL
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

	if (Channel_GetJunction(t) != NULL)
	{
		Channel_DQualify(t, teq_transfer);
		Channel_EnqueueDelta(t); /* REQUIRES GIL */
	}
	else
	{
		/*
			# Not acquired by a junction.
			# Directly apply the event qualification and
			# the junction will enqueue it when acquired.
		*/
		Channel_IQualify(t, teq_transfer);
	}

	Py_INCREF(self);
	return(self);
}

static PyObj
transit_force(PyObj self)
{
	Channel t = (Channel) self;

	Channel_DControl(t, ctl_force);

	if (Channel_Attached(t) && Channel_IQualified(t, teq_transfer))
	{
		/* No Junction? Do not enqueue, but allow the effect */
		/* to occur when it is later acquired.               */
		Channel_EnqueueDelta(t); /* REQUIRES GIL */
	}

	Py_RETURN_NONE;
}

/* Raw buffer interface */
static PyObj
transit_slice(PyObj self)
{
	Channel t = (Channel) self;

	if (!Channel_HasResource(t))
		Py_RETURN_NONE;

	return(_PySlice_FromIndices(Channel_GetWindowStart(t), Channel_GetWindowStop(t)));
}

static PyObj
transit_transfer(PyObj self)
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
transit_sizeof_transfer(PyObj self)
{
	uint32_t size;
	Channel t = (Channel) self;

	if (!Channel_HasResource(t) || !Channel_HasEvent(t, tev_transfer))
		return(PyLong_FromLong(0));

	size = Channel_GetWindowStop(t) - Channel_GetWindowStart(t);

	return(PyLong_FromUnsignedLong(size));
}

static PyObj
transit_terminate(PyObj self)
{
	Channel t = (Channel) self;

	if (!Channel_Attached(t))
	{
		/*
			# Has GIL, not in Traffic.
			# Junction instances cannot acquire Channels without the GIL.

			# Running terminate directly is safe.
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
			# Acquired by a Junction instance, that Junction
			# instance is responsible for performing termination.

			# Has GIL, so place teq_terminate event qualification on the delta.
		*/
		Channel_DQualify(t, teq_terminate);

		if ((PyObj) Py_TYPE(t) == junctiontype)
		{
			junction_fall((Junction) t, 0);
		}
		else
		{
			Channel_EnqueueDelta(t); /* REQUIRES GIL */
		}
	}

	Py_RETURN_NONE;
}

static PyObj
transit_resize_exoresource(PyObj self, PyObj args)
{
	Py_RETURN_NONE;
}

static PyObj
transit_endpoint(PyObj self)
{
	Channel t = (Channel) self;
	kpoint_t kp = Channel_GetKPoint(t);
	any_addr_t addr;
	int r;

	socklen_t addrlen = sizeof(addr);

	if (!Channel_PortLatched(t))
		Py_RETURN_NONE;

	addrlen = sizeof(addr);
	addr.ss_family = AF_UNSPEC;
	bzero(&addr, addrlen);

	if (Channel_Polarity(t) == p_output)
	{
		/*
			# Sends, get peer.
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
			# It is the endpoint, get sockname.
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
			# Check for anonymous sockets. (socketpair)
		*/
		local_addr_t *localaddr = (local_addr_t *) &addr;

		/*
			# Return the peereid if the remote is NULL/empty.
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

	return((PyObj) endpoint_create((if_addr_ref_t) &addr, addrlen));

	none:
	{
		Py_RETURN_NONE;
	}
}

static PyMethodDef
transit_methods[] = {
	{"endpoint", (PyCFunction) transit_endpoint, METH_NOARGS,
		PyDoc_STR(
			"Construct an Endpoint object from the Channel describing the known destination of the channel, the end-point.\n"
			"For output transits, the endpoint will be the remote host. For input transits, the endpoint will be "
			"the local interface and port."
			"\n\n"
			"[Effect]\n"
			"/(&Endpoint)`Return`/\n\t"
			"A new Endpoint instance.\n"
			"\n"
		)
	},

	{"acquire",
		(PyCFunction) transit_acquire, METH_O,
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
		(PyCFunction) transit_resize_exoresource, METH_VARARGS,
		PyDoc_STR(
			"Resize the related exoresource.\n"
			"[Parameters]\n"
			"/(&int)`new_size`/\n"
			"\tThe size, relative or absolute, of the kernel resource that should be used for the Channel.\n"
		)
	},

	{"force",
		(PyCFunction) transit_force, METH_NOARGS,
		PyDoc_STR(
			"Force the transit to perform a transfer.\n"
			"This causes an empty transfer event to occur.\n"
		)
	},

	{"transfer", (PyCFunction) transit_transfer, METH_NOARGS,
		PyDoc_STR(
			"This returns the slice of the resource that was transferred iff a transfer occurred.\n"
			"It is essentially `transit.resource[transit.slice()]`.\n"
			"\n"
			"[Effects]\n"
			"/(&object)`Return`/\n"
			"\tThe transferred data. Usually a &memoryview.\n"
			"\n"
		)
	},

	{"slice", (PyCFunction) transit_slice, METH_NOARGS,
		PyDoc_STR(
			"The slice method is always available. In cases where the transit is not in a cycle, a zero-distance slice "
			"will be returned desribing the current position in the resource's buffer.\n"
			"[Effects]\n"
			"/(&slice)`Return`/\n"
			"\tA slice specifying the portion of the resource that was transferred.\n"
		)
	},

	{"sizeof_transfer", (PyCFunction) transit_sizeof_transfer, METH_NOARGS,
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
		(PyCFunction) transit_terminate, METH_NOARGS,
		PyDoc_STR(
			"Terminate the Channel permanently causing events to subside. Eventually, \n"
			"resources being held by the Tranist will be released.\n"
			"[Effects]\n"
			"/(&Channel)`Return`/\n"
			"\tThe transit being terminated.\n"
		)
	},

	{NULL,},
};

static PyMemberDef
transit_members[] = {
	{"junction",
		T_OBJECT, offsetof(struct Channel, junction), READONLY,
		PyDoc_STR(
			"The &Junction instance that the Channel has been acquired by.\n"
			"`None` if the Channel has not been acquired by a Junction instance."
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
		# Internal state access.
	*/
	#if FV_INJECTIONS()
		{"_state", T_UBYTE, offsetof(struct Channel, state), READONLY,
			PyDoc_STR("bit map defining the internal and external state of the transit")},
		{"_delta", T_UBYTE, offsetof(struct Channel, delta), READONLY,
			PyDoc_STR("bit map defining the internal state changes")},
		{"_event", T_UBYTE, offsetof(struct Channel, events), READONLY,
			PyDoc_STR("bit map of events that occurred this cycle")},
	#endif

	{NULL,},
};

static void
transit_dealloc(PyObj self)
{
	Channel t = (Channel) self;

	#if F_TRACE(dealloc)
		errpf("DEALLOC: %p %s\n", self, Py_TYPE(self)->tp_name);
	#endif

	/*
		# Junction instances hold a reference to a Channel until it is
		# removed from the ring.
		# Channels hold their reference to the junction until..now:
	*/
	Py_XDECREF(Channel_GetJunction(t));
	Channel_SetJunction(t, NULL);

	Py_DECREF(Channel_GetPort(t)); /* Alloc and init ports *before* using Channels. */
	Channel_SetPort(t, NULL);

	Py_XDECREF(Channel_GetLink(t)); /* Alloc and init ports *before* using Channels. */
	Channel_SetLink(t, NULL);
}

static PyObj
transit_get_polarity(PyObj self, void *_)
{
	Channel t = (Channel) self;
	PyObj rob;

	/*
		# The polarity of a Channel may be often accessed, so
		# avoid creating new objects.
	*/
	rob = polarity_objects[!Channel_GetControl(t, ctl_polarity)];
	Py_INCREF(rob);

	return(rob);
}

static PyObj
transit_get_terminated(PyObj self, void *_)
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
transit_get_exhausted(PyObj self, void *_)
{
	Channel t = (Channel) self;

	if (Channel_Terminating(t))
	{
		/*
			# Don't indicate that a resource can be acquired.
		*/
		Py_INCREF(Py_False);
		return(Py_False);
	}

	/*
		# This should be called after receiving an exhaust event, which
		# removes this internal flag.
	*/
	if (Channel_IQualified(t, teq_transfer) || Channel_DQualified(t, teq_transfer))
	{
		/*
			# This needs to error out as the traffic flow may be using the
			# transit's resource at this particular moment.
		*/
		Py_INCREF(Py_False);
		return(Py_False);
	}

	Py_INCREF(Py_True);
	return(Py_True);
}

static PyObj
transit_get_resource(PyObj self, void *_)
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
	transit_get_xtransfer(PyObj self, void *_)
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
	transit_get_itransfer(PyObj self, void *_)
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
	transit_set_xtransfer(PyObj self, PyObj val, void *_)
	{
		Channel t = (Channel) self;

		if (val == Py_True)
			Channel_XQualify(t, teq_transfer);
		else
			Channel_XNQualify(t, teq_transfer);

		return(0);
	}

	static int
	transit_set_itransfer(PyObj self, PyObj val, void *_)
	{
		Channel t = (Channel) self;

		if (val == Py_True)
			Channel_IQualify(t, teq_transfer);
		else
			Channel_INQualify(t, teq_transfer);

		return(0);
	}
#endif

static PyGetSetDef transit_getset[] = {
	/*
		# Event introspection.
	*/
	{"polarity", transit_get_polarity, NULL,
		PyDoc_STR("`1` if the transit receives, `-1` if it sends.")
	},

	{"terminated", transit_get_terminated, NULL,
		PyDoc_STR("Whether the transit is capable of transferring at all.")
	},

	{"exhausted", transit_get_exhausted, NULL,
		PyDoc_STR("Whether the transit has a resource capable of performing transfers.")
	},

	{"resource", transit_get_resource, NULL,
		PyDoc_STR("The object whose buffer was acquired, &Octets.acquire, "
			"as the Channel's transfer resource.\n\n&None if there is no resource.")
	},

	#if FV_INJECTIONS()
		{"_xtransfer",
			transit_get_xtransfer, transit_set_xtransfer,
			PyDoc_STR("Whether the exoresource is currently known to be capable of transfers.")
		},

		{"_itransfer",
			transit_get_itransfer, transit_set_xtransfer,
			PyDoc_STR("Whether the transit is currently known to be capable of transfers.")
		},
	#endif

	{NULL,},
};

PyDoc_STRVAR(transit_doc,
	"The base Channel type, &.abstract.Channel, created and used by &.kernel.\n"
);

/**
	# Base type for the transit implementations.
*/
ChannelPyTypeObject ChannelType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Channel"),   /* tp_name */
	sizeof(struct Channel),   /* tp_basicsize */
	0,                        /* tp_itemsize */
	transit_dealloc,          /* tp_dealloc */
	NULL,                     /* tp_print */
	NULL,                     /* tp_getattr */
	NULL,                     /* tp_setattr */
	NULL,                     /* tp_compare */
	NULL,                     /* tp_repr */
	NULL,                     /* tp_as_number */
	NULL,                     /* tp_as_sequence */
	NULL,                     /* tp_as_mapping */
	NULL,                     /* tp_hash */
	NULL,                     /* tp_call */
	NULL,                     /* tp_str */
	NULL,                     /* tp_getattro */
	NULL,                     /* tp_setattro */
	NULL,                     /* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_DEFAULT,       /* tp_flags */
	transit_doc,              /* tp_doc */
	NULL,                     /* tp_traverse */
	NULL,                     /* tp_clear */
	NULL,                     /* tp_richcompare */
	0,                        /* tp_weaklistoffset */
	NULL,                     /* tp_iter */
	NULL,                     /* tp_iternext */
	transit_methods,          /* tp_methods */
	transit_members,          /* tp_members */
	transit_getset,           /* tp_getset */
	NULL,                     /* tp_base */
	NULL,                     /* tp_dict */
	NULL,                     /* tp_descr_get */
	NULL,                     /* tp_descr_set */
	0,                        /* tp_dictoffset */
	NULL,                     /* tp_init */
	NULL,                     /* tp_alloc */
	NULL,                     /* tp_new */
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
					# Throw Warning
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
alloci(PyObj isubtype, PyObj osubtype, Port *out)
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

	INIT_INPUT_TRANSIT(t, NULL);
	Channel_SetPort(t, p);
	p->latches = 1;

	*out = p;

	return(rob);
}

static PyObj
alloco(PyObj isubtype, PyObj osubtype, Port *out)
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

	INIT_OUTPUT_TRANSIT(t, NULL);
	Channel_SetPort(t, p);
	p->latches = 1 << 4;
	*out = p;

	return(rob);
}

/**
	# Create a pair of Objects and put them in a tuple to be returned.
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

	/* transit_dealloc expects port to be non-null.  */
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

	INIT_INPUT_TRANSIT(i, NULL);
	INIT_OUTPUT_TRANSIT(o, NULL);

	port->latches = (1 << 4) | 1;
	*out = port;

	return(rob);

	error:
		Py_XDECREF(port);
		Py_DECREF(rob);

	return(NULL);
}

/**
	# Same as allocio, but the Ports for each Channel are distinct objects.
	# (os.pipe(), dup() pairs.
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

	input = alloci(isubtype, osubtype, &x);
	if (input == NULL)
		goto error;
	PyTuple_SET_ITEM(rob, 0, (PyObj) input);

	output = alloco(isubtype, osubtype, &y);
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

/**
	# Create a pair of Objects and put them in a tuple to be returned.
*/
static PyObj
allocioio(PyObj isubtype, PyObj osubtype, Port p[])
{
	Channel r1 = NULL, w1 = NULL, r2 = NULL, w2 = NULL;
	PyObj rob;
	Port porta = NULL, portb = NULL;

	PYTHON_RECEPTACLE("alloc_quad", &rob, alloc_quad);
	if (rob == NULL)
		return(NULL);

	PYTHON_RECEPTACLE("alloc_porta", &porta, alloc_port);
	if (porta == NULL)
		goto error;

	PYTHON_RECEPTACLE("alloc_portb", &portb, alloc_port);
	if (portb == NULL)
		goto error;

	PYTHON_RECEPTACLE("alloc_isubtype1", &r1, PyAllocate, isubtype);
	if (r1 == NULL)
		goto error;
	INIT_INPUT_TRANSIT(r1, NULL);
	Channel_SetPort(r1, porta);
	Py_INCREF(porta);

	PYTHON_RECEPTACLE("alloc_osubtype1", &w1, PyAllocate, osubtype);
	if (w1 == NULL)
		goto error;
	INIT_OUTPUT_TRANSIT(w1, NULL);
	Channel_SetPort(w1, porta);
	Py_INCREF(porta);

	PYTHON_RECEPTACLE("alloc_isubtype2", &r2, PyAllocate, isubtype);
	if (r2 == NULL)
		goto error;
	INIT_INPUT_TRANSIT(r2, NULL);
	Channel_SetPort(r2, portb);
	Py_INCREF(portb);

	PYTHON_RECEPTACLE("alloc_osubtype2", &w2, PyAllocate, osubtype);
	if (w2 == NULL)
		goto error;
	INIT_OUTPUT_TRANSIT(w2, NULL);
	Channel_SetPort(w2, portb);
	Py_INCREF(portb);

	PyTuple_SET_ITEM(rob, 0, (PyObj) r1);
	PyTuple_SET_ITEM(rob, 1, (PyObj) w1);
	PyTuple_SET_ITEM(rob, 2, (PyObj) r2);
	PyTuple_SET_ITEM(rob, 3, (PyObj) w2);
	Py_DECREF(porta);
	Py_DECREF(portb);

	porta->latches = (1 << 4) | 1;
	portb->latches = (1 << 4) | 1;
	p[0] = porta;
	p[1] = portb;

	return(rob);

	error:
	{
		Py_XDECREF(porta);
		Py_XDECREF(portb);
		Py_XDECREF(r1);
		Py_XDECREF(r2);
		Py_XDECREF(w1);
		Py_XDECREF(w2);
		Py_DECREF(rob);
		return(NULL);
	}
}

static PyObj
octets_rallocate(PyObj subtype, PyObj size)
{
	PyObj args, rob = NULL;

	args = PyTuple_New(1);
	if (args == NULL)
		return(NULL);

	PyTuple_SET_ITEM(args, 0, PyByteArray_FromObject(size));
	if (PyTuple_GET_ITEM(args, 0) != NULL)
	{
		rob = PyObject_CallObject(((PyObj) &PyMemoryView_Type), args);
	}

	Py_DECREF(args);
	return(rob);
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

	{"rallocate",
		(PyCFunction) octets_rallocate,
		METH_O|METH_CLASS,
		PyDoc_STR(
			"Create a mutable resource capable of being written into by a Octets instance.\n"

			"[Parameters]\n"
			"/size/\n"
			"Number of bytes that can be written in the resource.\n"
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
	sizeof(struct Channel),  /* tp_basicsize */
	0,                       /* tp_itemsize */
	NULL,                    /* tp_dealloc */
	NULL,                    /* tp_print */
	NULL,                    /* tp_getattr */
	NULL,                    /* tp_setattr */
	NULL,                    /* tp_compare */
	NULL,                    /* tp_repr */
	NULL,                    /* tp_as_number */
	NULL,                    /* tp_as_sequence */
	NULL,                    /* tp_as_mapping */
	NULL,                    /* tp_hash */
	NULL,                    /* tp_call */
	NULL,                    /* tp_str */
	NULL,                    /* tp_getattro */
	NULL,                    /* tp_setattro */
	NULL,                    /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,      /* tp_flags */
	Octets_doc,              /* tp_doc */
	NULL,                    /* tp_traverse */
	NULL,                    /* tp_clear */
	NULL,                    /* tp_richcompare */
	0,                       /* tp_weaklistoffset */
	NULL,                    /* tp_iter */
	NULL,                    /* tp_iternext */
	octets_methods,          /* tp_methods */
	NULL,                    /* tp_members */
	NULL,                    /* tp_getset */
	&ChannelType.typ,        /* tp_base */
	NULL,                    /* tp_dict */
	NULL,                    /* tp_descr_get */
	NULL,                    /* tp_descr_set */
	0,                       /* tp_dictoffset */
	NULL,                    /* tp_init */
	NULL,                    /* tp_alloc */
	NULL,                    /* tp_new */
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
allocate_array(PyObj subtype, PyObj args)
{
	return(PyObject_Call(new_array, args, NULL));
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
			# Failure to resize the listening queue is not necessarily
			# fatal; this is unlike listen during initialization as
			# we are essentially checking that the socket *can* listen.
		*/

		port_listen(Channel_GetPort(t), backlog);
	}

	Py_RETURN_NONE;
}

static PyMethodDef sockets_methods[] = {
	{"rallocate",
		(PyCFunction) allocate_array,
		METH_VARARGS|METH_CLASS,
		PyDoc_STR(
			"Create a mutable resource capable of being written into by a Sockets instance.\n"
			"\n"
			"[Parameters]\n"
			"/(&int)`size`/\n"
			"\tNumber of sockets that can be accepted.\n"
			"\n"
			"[Effects]\n"
			"/(&array.array)`Return`/\n"
			"\tA new mutable resource.\n"
			"\n"
		)
	},

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

PyDoc_STRVAR(Sockets_doc, "transit transferring file descriptors accepted by accept(2)");

ChannelPyTypeObject SocketsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Sockets"),  /* tp_name */
	sizeof(struct Channel),  /* tp_basicsize */
	0,                       /* tp_itemsize */
	NULL,                    /* tp_dealloc */
	NULL,                    /* tp_print */
	NULL,                    /* tp_getattr */
	NULL,                    /* tp_setattr */
	NULL,                    /* tp_compare */
	NULL,                    /* tp_repr */
	NULL,                    /* tp_as_number */
	NULL,                    /* tp_as_sequence */
	NULL,                    /* tp_as_mapping */
	NULL,                    /* tp_hash */
	NULL,                    /* tp_call */
	NULL,                    /* tp_str */
	NULL,                    /* tp_getattro */
	NULL,                    /* tp_setattro */
	NULL,                    /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,      /* tp_flags */
	Sockets_doc,             /* tp_doc */
	NULL,                    /* tp_traverse */
	NULL,                    /* tp_clear */
	NULL,                    /* tp_richcompare */
	0,                       /* tp_weaklistoffset */
	NULL,                    /* tp_iter */
	NULL,                    /* tp_iternext */
	sockets_methods,         /* tp_methods */
	NULL,                    /* tp_members */
	NULL,                    /* tp_getset */
	&ChannelType.typ,        /* tp_base */
	NULL,                    /* tp_dict */
	NULL,                    /* tp_descr_get */
	NULL,                    /* tp_descr_set */
	0,                       /* tp_dictoffset */
	NULL,                    /* tp_init */
	NULL,                    /* tp_alloc */
	NULL,                    /* tp_new */
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
	{"rallocate",
		(PyCFunction) allocate_array,
		METH_VARARGS|METH_CLASS,
		PyDoc_STR(
			"Create a mutable resource capable of being written into by a Ports instance.\n"
			"\n[Parameters]\n"
			"/(&int)size/\n"
			"\tNumber of ports that can be accepted before the resource is exhausted.\n"
			"\n[Effects]\n"
			"/(&array.array)`Return`/\n"
			"\tA new mutable resource.\n"
			"\n"
		)
	},

	{NULL,}
};

PyDoc_STRVAR(Ports_doc, "");

ChannelPyTypeObject
PortsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Ports"),    /* tp_name */
	sizeof(struct Channel),  /* tp_basicsize */
	0,                       /* tp_itemsize */
	NULL,                    /* tp_dealloc */
	NULL,                    /* tp_print */
	NULL,                    /* tp_getattr */
	NULL,                    /* tp_setattr */
	NULL,                    /* tp_compare */
	NULL,                    /* tp_repr */
	NULL,                    /* tp_as_number */
	NULL,                    /* tp_as_sequence */
	NULL,                    /* tp_as_mapping */
	NULL,                    /* tp_hash */
	NULL,                    /* tp_call */
	NULL,                    /* tp_str */
	NULL,                    /* tp_getattro */
	NULL,                    /* tp_setattro */
	NULL,                    /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,      /* tp_flags */
	Ports_doc,               /* tp_doc */
	NULL,                    /* tp_traverse */
	NULL,                    /* tp_clear */
	NULL,                    /* tp_richcompare */
	0,                       /* tp_weaklistoffset */
	NULL,                    /* tp_iter */
	NULL,                    /* tp_iternext */
	ports_methods,           /* tp_methods */
	NULL,                    /* tp_members */
	NULL,                    /* tp_getset */
	&ChannelType.typ,        /* tp_base */
	NULL,                    /* tp_dict */
	NULL,                    /* tp_descr_get */
	NULL,                    /* tp_descr_set */
	0,                       /* tp_dictoffset */
	NULL,                    /* tp_init */
	NULL,                    /* tp_alloc */
	NULL,                    /* tp_new */
},
	&PortsTIF,
};

/**
	# Structure for &.kernel.DatagramArray providing access
	# to the composition of datagrams to be emitted or received.
*/
struct DatagramArray {
	PyObject_VAR_HEAD

	/**
		# Number of entries in &indexes.
	*/
	uint32_t ngrams;

	/**
		# Address Length of endpoint.
	*/
	socklen_t addrlen;

	/**
		# Packet Family of endpoint.
	*/
	int pf;

	/**
		# Address Space of endpoint.
	*/
	uint32_t space;

	/**
		# Current transfer.
	*/
	Py_buffer data;

	/**
		# The indexes to the Datagrams held by the Array inside
		# the buffer, `data`.
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
		# Need the base buffer object for proper slicing.
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

	return((PyObj) endpoint_create(DatagramGetAddress(dg), DatagramGetAddressLength(dg)));
}

static PyObj
datagramarray_payload(PyObj self, PyObj args)
{
	DatagramArray dga = (DatagramArray) self;
	unsigned long offset;

	if (!PyArg_ParseTuple(args, "L", &offset))
		return(NULL);

	return(datagramarray_get_memory(dga, offset));
}

static PyObj
datagramarray_endpoint(PyObj self, PyObj args)
{
	DatagramArray dga = (DatagramArray) self;
	unsigned long offset;

	if (!PyArg_ParseTuple(args, "L", &offset))
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

	if (!PyArg_ParseTuple(args, "LO", &offset, &endpoint))
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
			if (!ip4_from_object(endpoint, dg->addr))
				return(NULL);
		break;

		case ip6_pf:
			if (!ip6_from_object(endpoint, dg->addr))
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
	}
	unit = DatagramCalculateUnit(space, dga->addrlen);

	PYTHON_RECEPTACLE("new_ba", &ba, PyByteArray_FromStringAndSize, "", 0);
	if (ba == NULL)
	{
		Py_DECREF(rob);
		return(NULL);
	}

	i = PyByteArray_Resize(ba, (unit * ngrams));
	if (i) goto error;

	i = PyObject_GetBuffer(ba, &(dga->data), PyBUF_WRITABLE);
	if (i) goto error;

	/*
		# Clear data. Allows memoryview payload access to copy first n-bytes and forget.
	*/
	bzero(dga->data.buf, dga->data.len);

	Py_DECREF(ba); /* `data` buffer holds our reference */

	if (ngrams > 0)
	{
		/*
			# The last index points to the end of the memory block.
		*/
		for (fdg = dga->data.buf, i = 0; i < ngrams; ++i, fdg += unit)
		{
			cur = dga->indexes[i] = (struct Datagram *) fdg;
			cur->addrlen = dga->addrlen;
			cur->gramspace = space;
		}

		/*
			# end of buffer index
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
		# Normalize indexes. If the index is greater than or less than,
		# set it to the terminal.
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
			# slice of empty array.
		*/
		Py_INCREF(src);
		return((PyObj) src);
	}

	/*
		# At least one index for the sentinal.
	*/
	PYTHON_RECEPTACLE(NULL, &rob, subtype->tp_alloc, subtype, (stop - start) + 1);
	if (rob == NULL)
		return(NULL);
	dga = (DatagramArray) rob;
	dga->data.obj = NULL;

	/*
		# No Python errors after this point.
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
		# The last index points to the end of the memory block.
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
		# slice according to the local perspective of the underlying bytearray
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
}

PyDoc_STRVAR(datagramarray_doc, "A mutable buffer object for sending and receiving Datagrams; octets coupled with an IP address.");
PyTypeObject DatagramArrayType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("DatagramArray"), /* tp_name */
	sizeof(struct DatagramArray), /* tp_basicsize */
	sizeof(struct Datagram *),    /* tp_itemsize */
	datagramarray_dealloc,        /* tp_dealloc */
	NULL,                         /* tp_print */
	NULL,                         /* tp_getattr */
	NULL,                         /* tp_setattr */
	NULL,                         /* tp_compare */
	NULL,                         /* tp_repr */
	NULL,                         /* tp_as_number */
	&datagramarray_sequence,      /* tp_as_sequence */
	&datagramarray_mapping,       /* tp_as_mapping */
	NULL,                         /* tp_hash */
	NULL,                         /* tp_call */
	NULL,                         /* tp_str */
	NULL,                         /* tp_getattro */
	NULL,                         /* tp_setattro */
	&datagramarray_buffer,        /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,           /* tp_flags */
	datagramarray_doc,            /* tp_doc */
	NULL,                         /* tp_traverse */
	NULL,                         /* tp_clear */
	NULL,                         /* tp_richcompare */
	0,                            /* tp_weaklistoffset */
	datagramarray_iter,           /* tp_iter */
	NULL,                         /* tp_iternext */
	datagramarray_methods,        /* tp_methods */
	NULL,                         /* tp_members */
	NULL,                         /* tp_getset */
	NULL,                         /* tp_base */
	NULL,                         /* tp_dict */
	NULL,                         /* tp_descr_get */
	NULL,                         /* tp_descr_set */
	0,                            /* tp_dictoffset */
	NULL,                         /* tp_init */
	NULL,                         /* tp_alloc */
	datagramarray_new,            /* tp_new */
};

static PyObj
datagrams_rallocate(PyObj self, PyObj args)
{
	Datagrams t = (Datagrams) self;
	unsigned long ngrams, size = 512;
	PyObj rob;

	if (!PyArg_ParseTuple(args, "L|L", &ngrams, &size))
		return(NULL);

	rob = allocdga(&DatagramArrayType, t->pf, size, ngrams);
	return(rob);
}

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
	{"rallocate",
		(PyCFunction) datagrams_rallocate, METH_VARARGS,
		PyDoc_STR("Allocate a DatagramArray for use with the Datagrams transit.")
	},

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
	# deallocation is straight forward except in the case of sockets,
	# which have a shared file descriptor and must refer to each other
	# to identify whether or not the kpoint can be closed.
*/

PyDoc_STRVAR(datagrams_doc, "transit transferring DatagramArray's");
ChannelPyTypeObject DatagramsType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Datagrams"),  /* tp_name */
	sizeof(struct Datagrams),  /* tp_basicsize */
	0,                         /* tp_itemsize */
	NULL,                      /* tp_dealloc */
	NULL,                      /* tp_print */
	NULL,                      /* tp_getattr */
	NULL,                      /* tp_setattr */
	NULL,                      /* tp_compare */
	NULL,                      /* tp_repr */
	NULL,                      /* tp_as_number */
	NULL,                      /* tp_as_sequence */
	NULL,                      /* tp_as_mapping */
	NULL,                      /* tp_hash */
	NULL,                      /* tp_call */
	NULL,                      /* tp_str */
	NULL,                      /* tp_getattro */
	NULL,                      /* tp_setattro */
	NULL,                      /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,        /* tp_flags */
	datagrams_doc,             /* tp_doc */
	NULL,                      /* tp_traverse */
	NULL,                      /* tp_clear */
	NULL,                      /* tp_richcompare */
	0,                         /* tp_weaklistoffset */
	NULL,                      /* tp_iter */
	NULL,                      /* tp_iternext */
	datagrams_methods,         /* tp_methods */
	NULL,                      /* tp_members */
	NULL,                      /* tp_getset */
	&ChannelType.typ,          /* tp_base */
	NULL,                      /* tp_dict */
	NULL,                      /* tp_descr_get */
	NULL,                      /* tp_descr_set */
	0,                         /* tp_dictoffset */
	NULL,                      /* tp_init */
	NULL,                      /* tp_alloc */
	NULL,                      /* tp_new */
},
	&DatagramsTIF
};

static PyObj
junction_resize_exoresource(PyObj self, PyObj args)
{
	kevent_t *new_area;
	unsigned int new_size;
	Junction J = (Junction) self;

	/*
		# This adjusts the size of the kevents array, which is technically a
		# process resource, but junctions are special so use the exoresource
		# as a means to hint to the size of the kevent array as well.
	*/

	/*
		# Requires GIL.
	*/

	if (!PyArg_ParseTuple(args, "I", &new_size))
		return(NULL);

	if (Junction_Cycling(J))
	{
		PyErr_SetString(PyExc_RuntimeError, "cannot resize junction inside cycle");
		return(NULL);
	}

	new_area = (kevent_t *) PyMem_Realloc((void *) Junction_GetKEvents(J), (size_t) new_size * sizeof(kevent_t));
	if (new_area != NULL)
	{
		Junction_SetKEvents(J, new_area);
		Channel_SetWindowStop(J, new_size);
	}

	return(PyLong_FromUnsignedLong(Channel_GetWindowStop(J)));
}

/*
	# Relay to the generated transit alloc functions.
	# See the preproc blackmagic at the bottom of the file.
*/
static PyObj _jra_map = NULL; /* module init */

static PyObj
junction_rallocate(PyObj self, PyObj args)
{
	PyObj (*talloc)(PyObj, PyObj);
	PyObj req, params;
	PyObj cap, rob;
	Py_ssize_t i, len = PyTuple_GET_SIZE(args);

	if (len < 1)
	{
		PyErr_SetString(PyExc_TypeError, "Junction.rallocate requires at least one argument");
		return(NULL);
	}

	req = PyTuple_GET_ITEM(args, 0);

	/*
		# It's essentially a method table.
		# Capsules are used because we are resolving methods using arbitrary Python objects.
		# (If it were just strings, we'd probably use a plain hash table with uthash)
	*/
	cap = PyDict_GetItem(_jra_map, req);
	if (cap == NULL)
	{
		PyErr_SetString(PyExc_LookupError, "no such resource type");
		return(NULL);
	}

	talloc = PyCapsule_GetPointer(cap, NULL);
	if (len > 1)
		rob = talloc(self, PyTuple_GET_ITEM(args, 1));
	else
		rob = talloc(self, Py_None);

	return(rob);
}

static PyObj
junction_rtypes(PyObj self)
{
	return(PyObject_CallMethod(_jra_map, "keys", ""));
}

static PyObj
junction_acquire(PyObj self, PyObj ob)
{
	Junction J = (Junction) self;
	Channel t = (Channel) ob;

	if (!PyObject_IsInstance(ob, (PyObj) &ChannelType))
	{
		PyErr_SetString(PyExc_TypeError, "cannot attach objects that are not transits");
		return(NULL);
	}

	if (Channel_Terminating(J))
	{
		/* Junction is Terminated */
		PyErr_SetChannelTerminatedError(J);
		return(NULL);
	}

	if (!Channel_Attached(t))
	{
		if (Channel_Terminated(t))
		{
			/* Given Channel is Terminated */
			/*
				# Terminating check performed after the NULL check because
				# if the given transit is already acquired by the Junction,
				# it shouldn't complain due to it already being acquired.

				# Additionally, it's desired that ResourceError is consistently
				# thrown in this case.
			*/
			PyErr_SetChannelTerminatedError(t);
			return(NULL);
		}

		/* Control bit signals needs to connect. (kfilter) */
		Channel_DControl(t, ctl_connect);

		Py_INCREF(J); /* Newly acquired transit's reference to Junction.     */
		Py_INCREF(t); /* Junction's reference to the newly acquired Channel. */

		Channel_SetJunction(t, J);
		TRANSIT_ATTACH(t);

		Junction_IncrementChannelCount(J);
	}
	else
	{
		if (Channel_GetJunction(t) != J)
		{
			/* Another Junction instance acquired the Channel */
			PyErr_SetChannelResourceError(t);
			return(NULL);
		}

		/* Otherwise, just fall through as it's already acquired. */
	}

	Py_INCREF(ob);
	return(ob);
}

static void
junction_init(Junction J)
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

			J->choice.junction.wfd = wp.point;
			J->choice.junction.efd = eventfd(0, EFD_CLOEXEC);

			k.events = EPOLLERR | EPOLLHUP | EPOLLIN;
			k.data.ptr = NULL;

			epoll_ctl(p->point, EPOLL_CTL_ADD, J->choice.junction.efd, &k);

			k.events = EPOLLERR | EPOLLHUP | EPOLLIN | EPOLLOUT;
			k.data.ptr = J;
			epoll_ctl(p->point, EPOLL_CTL_ADD, J->choice.junction.wfd, &k);
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
junction_start_cycle(Junction J)
{
	Channel_SetNextTransfer(J, (Channel) J); /* Start with an Empty Transfer List */
}

static void
junction_finish_cycle(Junction J)
{
	/* Complete Junction termination? */

	Channel_SetNextTransfer(J, NULL); /* NULL transfer list means the cycle is over. */
	Junction_ResetTransferCount(J);
}

#ifdef EVMECH_EPOLL
	#define junction_kevent_change(J)
#else
static void
junction_kevent_change(Junction J)
{
	const static struct timespec nowait = {0,0}; /* Never wait for change submission. */
	Port port = Channel_GetPort(J);
	int r = 0, nkevents = Junction_NChanges(J);
	kevent_t *kevs = Junction_GetKEvents(J);

	Junction_ResetWindow(J);

	/*
		# Receipts are demanded, so the enties are only used for error reporting.
	*/
	#if F_TRACE(subscribe)
		for (int i = 0; i < nkevents; ++i)
			pkevent(&(kevs[i]));
	#endif

	/*
		# These must finish, so don't accept EINTR/EAGAIN.
	*/
	if (nkevents)
		port_kevent(port, -1, &r, kevs, nkevents, kevs, nkevents, &nowait);
}
#endif

static void
junction_kevent_collect(Junction J, int waiting)
{
	Port port = Channel_GetPort(J);
	kevent_t *kevs = Junction_GetKEvents(J);
	int nkevents = 0;

	#ifdef EVMECH_EPOLL
		const static int nowait = 0, wait = 9 * 1000;

		/*
			# For epoll, there are two file descriptors; one epoll referring to readers
			# and the other to the writers. The interface doesn't provide details about
			# whether reading is possible or writing can occur, so it has to be split up.
		*/
		switch (J->choice.junction.haswrites)
		{
			case 1:
			{
				struct Port wp;
				wp.point = J->choice.junction.wfd;
				port_epoll_wait(&wp, &nkevents, kevs, Channel_GetWindowStop(J), (waiting ? wait : nowait));

				if (nkevents < Channel_GetWindowStop(J))
					J->choice.junction.haswrites = 0;
				else
					J->choice.junction.haswrites = 2;
			}
			break;

			case 2:
				J->choice.junction.haswrites = 1; /* alternates between reads and writes */
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

	Junction_SetNCollected(J, nkevents);

	#if F_TRACE(collect)
		errpf("pid: %d\n", getpid());
		for (int i = 0; i < nkevents; ++i)
		{
			pkevent(&(kevs[i]));
			ptransit((Channel) kevs->udata);
		}
	#endif
}

/**
	# Note tev_join events on all Channels.

	# Run before junction_transfer_delta to have all
	# Channel's corresponding kevent filter to be loaded.
*/
static void
junction_reload(Junction J)
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
	# Enqueue the delta into the lltransfer list.
*/
static void
junction_transfer_delta(Junction J)
{
	Channel t;

	/* MUST HAVE GIL */

	/*
		# Scans the ring behind the Junction.
		# Process Events are queued up by moving the Channel behind the Junction after
		# applying flags to transit->delta.
	*/
	for (t = J->prev; Channel_GetDelta(t) != 0; t = t->prev)
	{
		/*
			# prepend to the lltransfer list.
			# The first 't' was the last enqueued.
		*/
		Channel_StateMerge(t, Channel_GetDelta(t)); /* Record the internal event quals. */
		Channel_ClearDelta(t); /* for subsequent use; after gil gets released */

		/*
			# Add to event list.
		*/
		Junction_AddTransfer(J, t);
	}
}

static kevent_t *
junction_current_kevent_slot(Junction J)
{
	/* Flush changes if the window is empty. */
	if (Junction_MaxCollected(J))
	{
		/* Full, flush the existing changes. */
		junction_kevent_change(J);
	}

	return(Junction_GetKEventSlot(J, Channel_GetWindowStart(J)));
}

/**
	# Process delta and setup for event processing
*/
static void
junction_apply_delta(Junction J)
{
	Channel prev, t;

	/* NO GIL */

	prev = (Channel) J;

	/* Reset kev state for slot acquisition. */
	Junction_ResetWindow(J);

	/*
		# Iterate through the transfer list in order to make any necessary
		# changes to the Channel's kfilter.

		# There is a need to keep track of the previous item on the list in case we need to
		# evict the Channel from our event list.
	*/

	for (t = Channel_GetNextTransfer(J); t != (Channel) J; t = Channel_GetNextTransfer(prev))
	{
		if (Channel_ShouldXConnect(t))
		{
			/*
				# iff xterminate hasn't occurred.
				# Happens with transits that are terminated at creation due to syscall failure.
			*/
			if (Channel_PortError(t) || !Channel_PortLatched(t))
			{
				/*
					# Inherit error or ignore connect if unlatched.
				*/
				Channel_XQualify(t, teq_terminate);
			}
			else if (!Channel_GetControl(t, ctl_requeue))
			{
				/*
					# Only connect if our port is latched
					# and the requeue flag is *not* set.
				*/
				kfilter_attach(t, junction_current_kevent_slot(J));
				Junction_ConsumeKEventSlot(J);
			}

			Channel_NulControl(t, ctl_connect);
		}

		if (Channel_GetControl(t, ctl_force))
		{
			/*
				# Remove the flag.
			*/
			Channel_NulControl(t, ctl_force);

			/*
				# It's a lie. The buffer will be zero, but the transfer
				# attempt will still occur likely resulting in zero read.
			*/
			Channel_XQualify(t, teq_transfer);
		}

		/*
			# Determine whether or not the Channel should be processed due to
			# the state change performed by the process.
		*/
		if (Channel_EventState(t))
		{
			/*
				# There is no check for "should exhaust" as exhaustion only
				# follows a transfer. Exhaust events are determined after the transfer
				# is emitted.
			*/
			prev = t;
		}
		else
		{
			/*
				# Incomplete qualifications, remove transit from list.
			*/

			/*
				# Current `prev` is valid, so set the next to this transit's next.
				# Afterwards, this Tranit's next pointer to NULL to signal that
				# it is not participating in a Transfer.
			*/
			Channel_SetNextTransfer(prev, Channel_GetNextTransfer(t));
			Channel_SetNextTransfer(t, NULL);
		}
	}

	/*
		# Make any remaining changes.
	*/
	junction_kevent_change(J);
}

#ifdef EVMECH_EPOLL
/**
	# Transform the collected events into local Channel state.
	# Place actionable events onto their respective transfer list.
*/
static void
junction_kevent_transform(Junction J)
{
	Channel t;
	Port p;
	kevent_t *kev, *kevs = Junction_GetKEvents(J);
	uint32_t i, nkevents = Junction_NCollected(J);

	/*
		# Iterate over the collected events and
		# transform the kevent state data into Channel state.
	*/
	for (i = 0; i < nkevents; ++i)
	{
		kev = &(kevs[i]);
		t = (Channel) kev->data.ptr;

		/*
			# The eventfd to trip epoll_wait()
		*/
		if (t == NULL)
		{
			uint64_t buf;
			read(J->choice.junction.efd, &buf, sizeof(buf));
			continue;
		}
		else if (t == J)
		{
			/*
				# Writes signal.
			*/
			J->choice.junction.haswrites = 1;
		}

		p = Channel_GetPort(t);

		if (kev->events & EPOLLIN
			|| kev->events & EPOLLOUT)
		{
			Channel_XQualify(t, teq_transfer);

			if (Channel_IQualified(t, teq_transfer))
				Junction_AddTransfer(J, t);
		}

		if (kev->events & EPOLLRDHUP
			|| kev->events & EPOLLERR
			|| kev->events & EPOLLHUP)
		{
			Channel_XQualify(t, teq_terminate);
			Junction_AddTransfer(J, t);
		}
	}
}

#else

/**
	# Transform the collected events into local Channel state.
	# Place actionable events onto their respective transfer list.
*/
static void
junction_kevent_transform(Junction J)
{
	Channel t;
	Port p;
	kevent_t *kev, *kevs = Junction_GetKEvents(J);
	uint32_t i, nkevents = Junction_NCollected(J);

	/*
		# Iterate over the collected events and
		# transform the kevent state data into Channel state.
	*/
	for (i = 0; i < nkevents; ++i)
	{
		kev = &(kevs[i]);
		t = (Channel) kev->udata;

		/*
			# (EVFILT_USER) user signaled for kevent exit?
		*/
		if (t == (Channel) J)
		{
			continue;
		}

		p = Channel_GetPort(t);

		if (kev->filter == EVFILT_WRITE && kev->flags & EV_EOF)
		{
			/*
				# Only xterminate when it's an Output transit.
				# io_terminate will handle termination on Input transits
				# in order to make sure that all data has been transferred into the process.
			*/
			Channel_XQualify(t, teq_terminate);
			Port_SetError(p, kev->fflags, kc_eof);

			/*
				# ShouldTerminate
			*/
			Junction_AddTransfer(J, t);
		}
		else
		{
			/*
				# Always note when a transfer is *possible*.
				# The iTransfer must be present in order for an event to be enqueued.
			*/

			/* Zero read triggers termination, writes are terminated by [local] host. */
			Channel_XQualify(t, teq_transfer);

			/*
				# Kernel can transfer, if the transit can too, then queue it up.
			*/
			if (Channel_IQualified(t, teq_transfer))
				Junction_AddTransfer(J, t);
		}
	}
}
#endif

static int
junction_fall(Junction J, int force)
{
	struct timespec ts = {0,0};
	kevent_t kev;
	int out = 0;

	if (!force && J->choice.junction.will_wait == 0)
		return(0);

	#ifdef EVMECH_EPOLL
	{
		uint64_t buf = 1;
		write(J->choice.junction.efd, &buf, sizeof(buf));
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
junction_force(PyObj self)
{
	PyObj rob;
	Junction J = (Junction) self;

	if (Channel_Terminating(J))
		Py_RETURN_NONE;

	rob = junction_fall(J, 1) == 0 ? Py_False : Py_True;
	Py_INCREF(rob);
	return(rob);
}

static void
_junction_terminate(Channel J)
{
	Channel t;
	Channel_IQualify(J, teq_terminate);

	/*
		# Terminate all the Channels in the Junction's ring.
	*/
	for (t = J->next; t != J; t = t->next)
	{
		/*
			# Enqueue is necessary here because ALL transits will
			# have a terminate action.
		*/
		Channel_DQualify(t, teq_terminate);
	}

	port_unlatch(Channel_GetPort(J), 0);

	#ifdef EVMECH_EPOLL
	{
		close(J->choice.junction.efd);
		close(J->choice.junction.wfd);
	}
	#endif

	return;
}

/**
	# Collect and process traffic events.
*/
static void
_junction_flow(Junction J)
{
	Channel t;

	junction_start_cycle(J);

	/*
		# Check for Junction termination.
	*/
	if (Channel_Terminating(J))
	{
		/*
			# terminate all transits
		*/
		_junction_terminate((Channel) J);
	}
	else if (!Channel_PortLatched(J))
	{
		/*
			# kqueue file descriptor went bad.
			# Either a fork occurred or the user close()'d it.
		*/
		junction_init(J);
		junction_reload(J);
	}
	Channel_ClearDelta(J);

	/*
		# Enqueue changed transits to lltransfer.
		# *REQUIRES GIL*
	*/
	junction_transfer_delta(J);

	if (Junction_ShouldWait(J))
	{
		/*
			# Signals that an EVFILT_USER is necessary to cause it
			# to fall through.

			# If not set, we can avoid a syscall.
		*/
		J->choice.junction.will_wait = 1;
	}

	/*
		# The GIL is no longer necessary, and concurrent
		# code can send signals to Channels as desired.
	*/

	Py_BEGIN_ALLOW_THREADS

	/*
		# The ring portion of the Channel objects are managed with the GIL.
		# t->next/t->prev CAN BE USED BY OTHER THREADS. DO NOT USE WITHOUT GIL.
	*/

	junction_apply_delta(J);

	/* don't bother collecting/transforming if terminating */
	if (!Channel_Terminating(J))
	{
		unsigned int countdown = 3;

		/*
			# Wait *iff* there are no transfers available for processing.
		*/
		junction_kevent_collect(J, Junction_ShouldWait(J));
		J->choice.junction.will_wait = 0; /* clear flag to avoid superfluous falls */

		junction_kevent_transform(J);

		/*
			# Iff more kevents may exists.
			# The previous collection of events must be equal to the size of our
			# eventlist in order to run this loop.
		*/
		#ifdef EVMECH_EPOLL
			while (countdown)
			{
				junction_kevent_collect(J, /* no wait */ 0);
				junction_kevent_transform(J);
				--countdown;
			}
		#else
			while (Junction_MaxCollected(J) && countdown)
			{
				junction_kevent_collect(J, /* no wait */ 0);
				junction_kevent_transform(J);
				--countdown;
			}
		#endif
	}

	/*
		# Prepare for junction_next_kevent_slot()
	*/
	Junction_ResetWindow(J);

	/*
		# Iterate over all the transits in the transfer list and process their events.
		# Sort the list into the I/O list.
	*/
	for (t = Channel_GetNextTransfer(J); t != (Channel) J; t = Channel_GetNextTransfer(t))
	{
		int polarity = !Channel_GetControl(t, ctl_polarity);

		#if F_TRACE(transfers)
			ptransit(t);
		#endif

		Junction_IncrementTransferCount(J);

		if (Channel_ShouldTerminate(t))
		{
			/*
				# Disconnect from the kevent stream iff requeue is not configured.
			*/
			if (!Channel_GetControl(t, ctl_requeue))
			{
				kfilter_cancel(t, junction_current_kevent_slot(J));
				Junction_ConsumeKEventSlot(J);
			}

			Channel_NoteEvent(t, tev_terminate);

			/*
				# _flush will perform resource releases (close and ReleaseResource)

				# This is necessary for two reasons:
				# 1. User may need to refer to port.
				# 2. GIL is needed to release local resources.
			*/
		}
		else if (Channel_ShouldTransfer(t))
		{
			/*
				# Transfers are preempted by termination.
			*/
			io_status_t stat;
			uint32_t xfer = 0;
			Port p = Channel_GetPort(t);
			char *buf = Channel_GetResourceBuffer(t);

			/*
				# The max transfer window spans from the end of the current window
				# to the end of the resource. The stop is adjusted after the operation
				# cannot transfer anymore.
			*/
			uint32_t rsize = Channel_GetResourceSize(t);
			uint32_t pos = Channel_GetWindowStop(t);
			uint32_t request = rsize - pos;

			Channel_NoteEvent(t, tev_transfer);

			/*
				# Adjust by the transit's window.
			*/
			buf += (intptr_t) pos;

			/*
				# Acquire the IO operation from the ChannelType
				# using the polarity to select the proper function pointer.
			*/
			io_op_t io = Channel_GetInterface(t)->io[polarity];

			#if F_TRACE(transfers)
				#define trace(...) errpf(__VA_ARGS__)
				trace("\n\nReSIZE: %d; REQUEST: %d\n", rsize, request);
				ptransit(t);
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
					# map io_status_t to state change.
				*/
				case io_flow:
					/*
						# Buffer exhausted and EAGAIN *not* triggered
						# Channel_XQualified(t, teq_transfer) == True
					*/
					Channel_INQualify(t, teq_transfer);
					trace(" FLOWS\n");
				break;

				case io_stop:
					/*
						# EAGAIN; wait for kernel event for continuation.
					*/
					Channel_XNQualify(t, teq_transfer);
					trace(" WOULDBLOCK\n");
				break;

				case io_terminate:
					/*
						# EOF condition or error returned.
						# It is possible that this has a transfer.
					*/
					Channel_XQualify(t, teq_terminate);
					Channel_NoteEvent(t, tev_terminate);

					if (!Channel_GetControl(t, ctl_requeue))
					{
						kfilter_cancel(t, junction_current_kevent_slot(J));
						Junction_ConsumeKEventSlot(J);
					}
					trace(" ERROR\n");
				break;
			}

			#if F_TRACE(transfers)
				trace("transform: ");
				ptransit(t);
				trace("\n ////// \n");
			#endif
			#undef trace
		}
		else
		{
			/*
				# No event. Filter.
			*/
			#if F_TRACE(no_events)
				ptransit(t);
			#endif
		}
	}

	/*
		# Perform any disconnects queued up in the loop.
	*/
	if (!Channel_Terminating(J))
		junction_kevent_change(J);

	Py_END_ALLOW_THREADS
}

struct ChannelInterface
JunctionTIF = {
	{NULL, NULL},
	f_transits, 1,
};

/**
	# Return an iterable to the collected events. &.kernel.Junction.transfer
*/
static PyObj
junction_transfer(PyObj self)
{
	Junction J = (Junction) self;
	PyObj rob;

	if (!Channel_InCycle(J))
		rob = PyTuple_New(0);
	else
		rob = new_jxi(J, 0);

	return(rob);
}

static PyObj
junction_sizeof_transfer(PyObj self)
{
	Junction J = (Junction) self;

	if (!Channel_InCycle(J))
		return(PyLong_FromLong(0));

	return(PyLong_FromUnsignedLong(Junction_GetTransferCount(J)));
}

static void
_junction_flush(Junction J)
{
	Channel t, next;

	/* REQUIRES GIL */

	t = Channel_GetNextTransfer(J);
	while (t != (Channel) J)
	{
		next = Channel_GetNextTransfer(t);
		Channel_SetNextTransfer(t, NULL);

		/*
			# Unconditionally collapse the window here.
			# We have the GIL so no concurrent Channel.acquire() calls are in progress.
			# If the user acquired the resource during the cycle, collapse will merely
			# set the stop to zero.

			# In cases where no transfer occurred, it's a no-op.
		*/
		Channel_CollapseWindow(t);

		if (Channel_HasEvent(t, tev_terminate))
		{
			/*
				# Release any resources owned by the transit.

				# In the case where the resource was acquired in the cycle,
				# we're not doing anything with the resource anyways, so get rid of it.
			*/
			Channel_ReleaseResource(t);
			Channel_ReleaseLink(t);
			port_unlatch(Channel_GetPort(t), Channel_Polarity(t));

			TRANSIT_DETACH(t);
			Junction_DecrementChannelCount(J);

			/*
				# Emitted termination? Release traffic's reference to the transit.
			*/
			Py_DECREF(t);
		}
		else
		{
			/*
				# If the delta qualification exists, the user transit.acquire()'d during
				# the cycle, so don't release the new resource.
			*/
			int exhausted = !Channel_DQualified(t, teq_transfer)
				&& !Channel_IQualified(t, teq_transfer);

			if (exhausted)
			{
				/*
					# Exhaust event occurred, but no new resource supplied in cycle.
					# Release any internal resources.

					# The user has the option to acquire() a new buffer within and
					# after a cycle.
				*/
				Channel_ReleaseResource(t);
			}
		}

		/*
			# Cycle is over. Clear events.
		*/
		Channel_ClearEvents(t);

		t = next;
	}

	junction_finish_cycle(J);
}

/**
	# Close file descriptors and release references; destroy entire ring.
*/
static PyObj
junction_void(PyObj self)
{
	Junction J = (Junction) self;
	Channel t;

	/* GIL Required */

	if (Junction_Cycling(J))
		junction_finish_cycle(J);

	for (t = J->next; t != (Channel) J; t = t->next)
	{
		Port p = Channel_GetPort(t);
		/*
			# Clear any transfer state.
		*/
		Channel_IQualify(t, teq_terminate);
		Channel_SetNextTransfer(t, NULL);
		port_unlatch(p, 0);
		p->cause = kc_void;

		t->prev->next = NULL;
		t->prev = NULL;
		Py_DECREF(t);

		/*
			# The Junction and Port references will be cleared by dealloc.
		*/
	}
	t->next = NULL;

	J->next = (Channel) J;
	J->prev = (Channel) J;
	Junction_ResetTransferCount(J);
	Junction_ResetChannelCount(J);
	port_unlatch(Channel_GetPort(J), 0);

	#ifdef EVMECH_EPOLL
		close(J->choice.junction.efd);
		close(J->choice.junction.wfd);
	#endif

	Py_RETURN_NONE;
}

/**
	# Begin a transfer processing cycle.
*/
static PyObj
junction_enter(PyObj self)
{
	Junction J = (Junction) self;

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

	_junction_flow(J);

	Py_INCREF(self);
	return(self);
}

/**
	# Close a transfer processing cycle.
*/
static PyObj
junction_exit(PyObj self, PyObj args)
{
	Junction J = (Junction) self;

	if (Channel_InCycle(J))
		_junction_flush(J);

	Py_RETURN_NONE;
}

static PyMethodDef
junction_methods[] = {
	{"resize_exoresource",
		(PyCFunction) junction_resize_exoresource, METH_VARARGS,
		PyDoc_STR(
			"In cases where resize fails, the old size will be returned "
			"and no error will be mentioned. For Junctions, resize *must* "
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

	{"rallocate",
		(PyCFunction) junction_rallocate, METH_VARARGS|METH_CLASS,
		PyDoc_STR(
			"Returns a Channel or a sequence of Channels constructed from the request.\n"

			"[ Parameters ]\n"
			"/(&tuple)`request`/\n"
			"\tThe address and parameters to the Channel allocator.\n"
		)
	},

	{"rtypes",
		(PyCFunction) junction_rtypes, METH_NOARGS|METH_CLASS,
		PyDoc_STR(
			"Returns an iterator to resource allocation request types recognized by the Junction."
		)
	},

	{"acquire",
		(PyCFunction) junction_acquire, METH_O,
		PyDoc_STR(
			"Acquires the Channel so that it may participate in &Junction cycles.\n"

			"[Parameters]\n"
			"/transit/\n"
			"\tThe &Channel that will be managed by this Junction.\n"
		)
	},

	{"void",
		(PyCFunction) junction_void, METH_NOARGS,
		PyDoc_STR(
			"Void all attached transits in an unfriendly manner.\n"
			"Terminate events will not be generated, the current cycle, if any, will be exited.\n\n"
			"! NOTE:\n"
			"\tNormally, this function should be only be used by child processes destroying the parent's state."
		)
	},

	{"force",
		(PyCFunction) junction_force, METH_NOARGS,
		PyDoc_STR(
			"Causes the next traffic cycle not *wait* for events. If a cycle has been started\n"
			"and is currently waiting for events, force will cause it to stop waiting for events.\n"
			"\n"
			"Returns the Junction instance being forced for method chaining.\n"
		)
	},

	{"transfer",
		(PyCFunction) junction_transfer, METH_NOARGS,
		PyDoc_STR(
			"Returns an iterable producing the transits that have events.\n"
		)
	},

	{"sizeof_transfer",
		(PyCFunction) junction_sizeof_transfer, METH_NOARGS,
		PyDoc_STR(
			"Get the number of transfers currently available; `0` if there is no transfers."
			"! NOTE:\n"
			"\tCurrently unavailable.\n\n"
			"\n"
			"Returns the number of Channels with events this cycle.\n"
		)
	},

	{"__enter__",
		(PyCFunction) junction_enter, METH_NOARGS,
		PyDoc_STR("Enter a Junction cycle allowing transition state to be examined.")
	},

	{"__exit__",
		(PyCFunction) junction_exit, METH_VARARGS,
		PyDoc_STR("Exit the Junction cycle destroying the transition state.")
	},

	{NULL,},
};

static PyMemberDef junction_members[] = {
	{"volume", T_PYSSIZET, offsetof(struct Junction, choice.junction.ntransits), READONLY,
		PyDoc_STR("The number of transits being managed by the Junction instance.")},
	{NULL,},
};

static PyObj
junction_get_resource(PyObj self, void *_)
{
	PyObj l;
	Py_ssize_t i = 0;
	Junction J = (Junction) self;
	Channel t = J->next;

	/*
		# Requires GIL.
	*/

	l = PyList_New(Junction_GetChannelCount(J));
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

static PyGetSetDef junction_getset[] = {
	{"resource", junction_get_resource, NULL,
		PyDoc_STR("A &list of all Channels attached to this Junction instance, save the Junction instance.")
	},
	{NULL,},
};

static void
junction_dealloc(PyObj self)
{
	Junction J = (Junction) self;
	PyMem_Free(Junction_GetKEvents(J));
	transit_dealloc(self);
}

static PyObj
junction_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {NULL,};
	Junction J;
	Port p;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "", kwlist))
		return(NULL);

	J = (Junction) alloci((PyObj) subtype, (PyObj) subtype, &p);
	if (J == NULL)
		return(NULL);
	p->type = kt_kqueue;
	p->freight = Type_GetInterface(subtype)->ti_freight;

	Channel_SetJunction(J, J);
	Channel_XQualify(J, teq_transfer);
	Channel_SetControl(J, ctl_polarity);

	Junction_ResetChannelCount(J);
	Junction_ResetTransferCount(J);

	/*
		# For Junctions, the Window's Stop is the size of malloc / sizeof(struct kevent)
	*/
	Channel_SetWindow(J, 0, CONFIG_DEFAULT_JUNCTION_SIZE);
	Junction_SetKEvents(J, PyMem_Malloc(sizeof(kevent_t) * Channel_GetWindowStop(J)));

	J->next = (Channel) J;
	J->prev = (Channel) J;

	junction_init(J);

	return((PyObj) J);
}

PyDoc_STRVAR(Junction_doc,
"The Junction implementation, &.abstract.Junction, for performing I/O with the kernel.");

ChannelPyTypeObject
JunctionType = {{
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Junction"),  /* tp_name */
	sizeof(struct Junction),  /* tp_basicsize */
	0,                        /* tp_itemsize */
	junction_dealloc,         /* tp_dealloc */
	NULL,                     /* tp_print */
	NULL,                     /* tp_getattr */
	NULL,                     /* tp_setattr */
	NULL,                     /* tp_compare */
	NULL,                     /* tp_repr */
	NULL,                     /* tp_as_number */
	NULL,                     /* tp_as_sequence */
	NULL,                     /* tp_as_mapping */
	NULL,                     /* tp_hash */
	NULL,                     /* tp_call */
	NULL,                     /* tp_str */
	NULL,                     /* tp_getattro */
	NULL,                     /* tp_setattro */
	NULL,                     /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,       /* tp_flags */
	Junction_doc,             /* tp_doc */
	NULL,                     /* tp_traverse */
	NULL,                     /* tp_clear */
	NULL,                     /* tp_richcompare */
	0,                        /* tp_weaklistoffset */
	NULL,                     /* tp_iter */
	NULL,                     /* tp_iternext */
	junction_methods,         /* tp_methods */
	junction_members,         /* tp_members */
	junction_getset,          /* tp_getset */
	&ChannelType.typ,         /* tp_base */
	NULL,                     /* tp_dict */
	NULL,                     /* tp_descr_get */
	NULL,                     /* tp_descr_set */
	0,                        /* tp_dictoffset */
	NULL,                     /* tp_init */
	NULL,                     /* tp_alloc */
	junction_new,             /* tp_new */
},
	&JunctionTIF,
};

/**
	# Build out the array.array("i", (-1,)).__mul__ object
	# for use in Sockets.rallocate(n)
*/
static PyObj
_init_intarray(void)
{
	PyObj rob = NULL; /* array.array("i", (-1,)).__mul__ */
	PyObj args, typ, vals; /* args to array.array() */
	PyObj mod, at, ai; /* array mod, array type, array instance */

	args = alloc_pair();
	if (args == NULL)
		return(NULL);

	typ = PyUnicode_FromString("i");
	if (typ == NULL)
		goto d_args;
	PyTuple_SET_ITEM(args, 0, typ);

	vals = PyTuple_New(1);
	if (vals == NULL)
		goto d_args;
	PyTuple_SET_ITEM(args, 1, vals);

	PyTuple_SET_ITEM(vals, 0, PyLong_FromLong(-1));
	if (PyTuple_GET_ITEM(vals, 0) == NULL)
		goto d_args;

	mod = PyImport_ImportModule("array");
	if (mod == NULL)
		goto d_args;

	at = PyObject_GetAttrString(mod, "array");
	if (at == NULL)
		goto d_mod;

	ai = PyObject_CallObject(at, args);
	if (ai == NULL)
		goto d_at;

	rob = PyObject_GetAttrString(ai, "__mul__");

	Py_DECREF(ai);
	d_at:
		Py_DECREF(at);
	d_mod:
		Py_DECREF(mod);
	d_args:
		Py_DECREF(args);
	return(rob);
}

/**
	# The initial parameter designates the allocation type to support the new transits.
	# The second parameter defines the freight type and the Python type used.
	# The third parameter is the domain/address family.
	# The fourth parameter is the "protocol".
*/
#define JUNCTION_RESOURCE_ALLOCATION_DEFAULTS() \
	X(io,octets,local,DEFAULT) \
	X(io,octets,ip4,DEFAULT) \
	X(io,octets,ip6,DEFAULT) \
	X(i,sockets,ip4,DEFAULT) \
	X(i,sockets,ip6,DEFAULT) \
	X(i,sockets,local,DEFAULT) \
	X(i,sockets,acquire,DEFAULT) \
	X(io,ports,acquire,DEFAULT) \
	X(io,datagrams,ip4,DEFAULT) \
	X(io,datagrams,ip6,DEFAULT) \

/**
	# Protocol explicitly stated.
*/
#define JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL() \
	X(io,octets,ip4,tcp) \
	X(io,octets,ip6,tcp) \
	X(io,octets,ip4,udp) \
	X(io,octets,ip6,udp) \

#define JUNCTION_RESOURCE_ALLOCATION_PROTOCOL() \
	X(io,datagrams,ip4,udp) \
	X(io,datagrams,ip6,udp) \

#define JUNCTION_RESOURCE_ALLOCATION_SELECTION() \
	X(iopair,octets,spawn,unidirectional) \
	X(ioio,octets,spawn,bidirectional) \
	X(io,octets,acquire,socket) \
	X(i,octets,acquire,input) \
	X(o,octets,acquire,output) \
	\
	X(i,sockets,acquire,socket) \
	X(io,ports,acquire,socket) \
	X(ioio,ports,spawn,bidirectional) \
	\
	X(i,octets,file,read) \
	X(o,octets,file,overwrite) \
	X(o,octets,file,append) \

#define DEFAULT_REQUEST(IOF, FREIGHT, DOMAIN, ...) #FREIGHT, #DOMAIN
#define PROTOCOL_REQUEST(IOF, FREIGHT, DOMAIN, PROTOCOL, ...) #FREIGHT, #DOMAIN, #PROTOCOL

#define OBNAME(IOF, FREIGHT, DOMAIN, PROTO, ...) \
	_pycap_##FREIGHT##_##DOMAIN##_##PROTO
#define OBNAME_BIND(IOF, FREIGHT, DOMAIN, PROTO, ...) \
	_pycap_##FREIGHT##_##DOMAIN##_##PROTO##_bind
#define ALLOCFNAME(IOF, FREIGHT, DOMAIN, PROTO, ...) \
	_talloc_##FREIGHT##_##DOMAIN##_##PROTO
#define ALLOCFNAME_BIND(IOF, FREIGHT, DOMAIN, PROTO, ...) \
	_talloc_##FREIGHT##_##DOMAIN##_##PROTO##_bind
#define ALLOC(IOF, ...) (alloc##IOF)
#define TYP(IOF, FREIGHT, ...) FREIGHT##type
#define PARAM_FAMILY(IOF, FREIGHT, DOMAIN, ...) DOMAIN##_pf
#define DOMAIN_STR(IOF, FREIGHT, DOMAIN, ...) #DOMAIN
#define PARAM_STORAGE(PARAM, IOF, FREIGHT, DOMAIN, ...) DOMAIN##_addr_t PARAM
#define PARAM_CLEAR(IOF, FREIGHT, DOMAIN, ...) DOMAIN##_clear
#define CONVERTER(IOF, FREIGHT, DOMAIN, ...) DOMAIN##_from_object

#define _jra_udp_ip4_init(rob) \
	((struct Datagrams *) PyTuple_GET_ITEM(rob, 0))->addrlen = \
	((struct Datagrams *) PyTuple_GET_ITEM(rob, 1))->addrlen = sizeof(ip4_addr_t)

#define _jra_udp_ip6_init(rob) \
	((struct Datagrams *) PyTuple_GET_ITEM(rob, 0))->addrlen = \
	((struct Datagrams *) PyTuple_GET_ITEM(rob, 1))->addrlen = sizeof(ip6_addr_t)

#define FIRST_TWO(IOF, F1, F2, ...) #F1, #F2
#define FIRST_TWO_IRI(IOF, F1, F2, ...) #F1 "://" #F2

#define FIRST_THREE(IOF, F1, F2, F3, ...) #F1, #F2, #F3
#define FIRST_THREE_IRI(IOF, F1, F2, F3, ...) #F1 "://" #F2 "/" #F3
#define FIRST_THREE_IRI_PORT(IOF, F1, F2, F3, ...) #F1 "://" #F2 ":" #F3

#define _TCPIP_PARAMS SOCK_STREAM, IPPROTO_TCP
#define _UDPIP_PARAMS SOCK_DGRAM, IPPROTO_UDP
#define _LOCAL_PARAMS PF_LOCAL, SOCK_STREAM, 0

/*
	# These defines are for the nasty X-macro generated transit allocation functions used
	# by Junction.rallocate(). There are a number of combinations, so grouping the
	# the similar operations helps to save some redundant code.

	# Cover each combination referenced by the capsule xmacro.
*/
#define ports_init_sockets_ip4_DEFAULT(P, x) \
	ports_listen(P[0], ip4_pf, (if_addr_ref_t) &x, sizeof(x))
#define ports_init_sockets_ip6_DEFAULT(P, x) \
	ports_listen(P[0], ip6_pf, (if_addr_ref_t) &x, sizeof(x))
#define ports_init_sockets_local_DEFAULT(P, x) \
	ports_listen(P[0], local_pf, (if_addr_ref_t) &x, sizeof(x))

#define ports_init_octets_local(P, x) \
	ports_connect(P[0], _LOCAL_PARAMS, (if_addr_ref_t) &x, sizeof(x))

#define ports_init_datagrams_ip4_udp(P, x) \
	ports_bind(P[0], ip4_pf, _UDPIP_PARAMS, (if_addr_ref_t) &x, sizeof(x))
#define ports_init_datagrams_ip6_udp(P, x) \
	ports_bind(P[0], ip6_pf, _UDPIP_PARAMS, (if_addr_ref_t) &x, sizeof(x))

#define ports_init_octets_ip4_tcp(P, x) \
	ports_connect(P[0], ip4_pf, _TCPIP_PARAMS, (if_addr_ref_t) &x, sizeof(x))
#define ports_init_octets_ip6_tcp(P, x) \
	ports_connect(P[0], ip6_pf, _TCPIP_PARAMS, (if_addr_ref_t) &x, sizeof(x))

#define ports_init_octets_ip4_udp(P, x) \
	ports_connect(P[0], ip4_pf, _UDPIP_PARAMS, (if_addr_ref_t) &x, sizeof(x))
#define ports_init_octets_ip6_udp(P, x) \
	ports_connect(P[0], ip6_pf, _UDPIP_PARAMS, (if_addr_ref_t) &x, sizeof(x))

#define ports_init_octets_ip4_tcp_bind(P, ...) \
	ports_bind_connect(P, ip4_pf, _TCPIP_PARAMS, __VA_ARGS__)
#define ports_init_octets_ip6_tcp_bind(P, ...) \
	ports_bind_connect(P, ip6_pf, _TCPIP_PARAMS, __VA_ARGS__)

#define ports_init_octets_ip4_udp_bind(P, ...) \
	ports_bind_connect(P, ip4_pf, _UDPIP_PARAMS, __VA_ARGS__)
#define ports_init_octets_ip6_udp_bind(P, ...) \
	ports_bind_connect(P, ip6_pf, _UDPIP_PARAMS, __VA_ARGS__)

#define ports_init_octets_file(f, P, x) do { \
	ports_open(P[0], x.fa_path, f); \
	if (P[0]->type == kt_file || P[0]->type == kt_device) { \
		Channel_XQualify(((Channel) rob), teq_transfer); \
		Channel_SetControl(((Channel) rob), ctl_requeue); \
	} \
} while(0)

#define ports_init_octets_file_read(...) \
	ports_init_octets_file(O_CREAT|O_RDONLY|O_CLOEXEC, __VA_ARGS__)
#define ports_init_octets_file_overwrite(...) \
	ports_init_octets_file(O_CREAT|O_WRONLY|O_CLOEXEC, __VA_ARGS__)
#define ports_init_octets_file_append(...) \
	ports_init_octets_file(O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC, __VA_ARGS__)

#define ports_init_acquire_socket(P, x) \
	do { P[0]->point = x; ports_identify_socket(P[0]); } while(0)
#define ports_init_acquire_input(P, x) \
	do { P[0]->point = x; ports_identify_input(P[0]); } while(0)
#define ports_init_acquire_output(P, x) \
	do { P[0]->point = x; ports_identify_output(P[0]); } while(0)

#define ports_init_octets_acquire_socket ports_init_acquire_socket
#define ports_init_sockets_acquire_socket ports_init_acquire_socket

#define ports_init_ports_acquire_socket ports_init_acquire_socket
#define ports_init_ports_acquire_DEFAULT ports_init_acquire_socket
#define ports_init_ports_spawn_bidirectional(P, x) ports_socketpair(P)

#define ports_init_octets_acquire_input  ports_init_acquire_input
#define ports_init_octets_acquire_output ports_init_acquire_output

#define ports_init_octets_spawn_bidirectional(P, x)  ports_socketpair(P)
#define ports_init_octets_spawn_unidirectional(P, x) ports_pipe(P)

#define ports_init_octets_local_DEFAULT ports_init_octets_local
#define ports_init_octets_ip4_DEFAULT ports_init_octets_ip4_tcp
#define ports_init_octets_ip6_DEFAULT ports_init_octets_ip6_tcp
#define ports_init_sockets_acquire_DEFAULT ports_init_acquire_socket
#define ports_init_datagrams_ip4_DEFAULT ports_init_datagrams_ip4_udp
#define ports_init_datagrams_ip6_DEFAULT ports_init_datagrams_ip6_udp

#define INITPORTS(IOF, FREIGHT, DOMAIN, PROTO, ...) \
	ports_init_##FREIGHT##_##DOMAIN##_##PROTO
#define INITPORTS_BIND(IOF, FREIGHT, DOMAIN, PROTO, ...) \
	ports_init_##FREIGHT##_##DOMAIN##_##PROTO##_bind

/* Generate the allocation functions. */
#define X(...) \
static PyObj \
ALLOCFNAME(__VA_ARGS__)(PyObj J, PyObj param) \
{ \
	const PyObj typ = TYP(__VA_ARGS__); \
	Port p[2] = {NULL,NULL}; \
	PyObj rob = NULL; \
	\
	PARAM_STORAGE(port_param, __VA_ARGS__) = PARAM_CLEAR(__VA_ARGS__); \
	\
	if (!CONVERTER(__VA_ARGS__)(param, (void *) &port_param)) \
		return(NULL); \
	rob = ALLOC(__VA_ARGS__)(typ, typ, p); \
	if (rob == NULL) \
		return(NULL); \
	p[0]->freight = Type_GetInterface(typ)->ti_freight; \
	if (p[1] != NULL) p[1]->freight = Type_GetInterface(typ)->ti_freight; \
	INITPORTS(__VA_ARGS__)(p, port_param); \
	if (p[0]->cause == kc_pyalloc) p[0]->cause = kc_none; \
	if (p[1] != NULL && p[1]->cause == kc_pyalloc) p[1]->cause = kc_none; \
	if (typ == datagramstype) { \
		((Datagrams) PyTuple_GET_ITEM(rob, 0))->pf = \
		((Datagrams) PyTuple_GET_ITEM(rob, 1))->pf = PARAM_FAMILY(__VA_ARGS__); \
	} \
	return(rob); \
}

JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
JUNCTION_RESOURCE_ALLOCATION_SELECTION()
#undef X

/*
	# Variant supporting bind.
	# J.rallocate(('octets', 'ip4', 'tcp', 'bind'), (connect, bind))
*/
#define X(...) \
	static PyObj \
	ALLOCFNAME_BIND(__VA_ARGS__)(PyObj J, PyObj args) \
	{ \
		const PyObj typ = TYP(__VA_ARGS__); \
		Port p; \
		PyObj rob = NULL; \
		\
		PARAM_STORAGE(bind_param, __VA_ARGS__) = PARAM_CLEAR(__VA_ARGS__); \
		PARAM_STORAGE(port_param, __VA_ARGS__) = PARAM_CLEAR(__VA_ARGS__); \
		\
		if (!PyArg_ParseTuple(args, "O&O&", CONVERTER(__VA_ARGS__), &port_param, CONVERTER(__VA_ARGS__), &bind_param)) \
			return(NULL); \
		rob = ALLOC(__VA_ARGS__)(typ, typ, &p); \
		if (rob == NULL) \
			return(NULL); \
		p->freight = Type_GetInterface(typ)->ti_freight; \
		INITPORTS_BIND(__VA_ARGS__)(p, (if_addr_ref_t) &port_param, sizeof(port_param), (if_addr_ref_t) &bind_param, sizeof(bind_param)); \
		if (p->cause == kc_pyalloc) \
			p->cause = kc_none; \
		return(rob); \
	}

	JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
#undef X

static PyObj
_init_junction_rallocation(void)
{
	PyObj rob = NULL

	/* There is a number of allocation combinations possible,     */
	/* so macros are used to generate many initialiation actions. */

	#define X(...) , \
		OBNAME(__VA_ARGS__) = PyCapsule_New((void *) (& (ALLOCFNAME(__VA_ARGS__))), NULL, NULL)

		JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_SELECTION()
	#undef X

	#define X(...) , \
		OBNAME_BIND(__VA_ARGS__) = PyCapsule_New((void *) (& (ALLOCFNAME_BIND(__VA_ARGS__))), NULL, NULL)
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
	#undef X
	;

	#define X(...) if (OBNAME(__VA_ARGS__) == NULL) goto error;
		JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
		JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_SELECTION()
	#undef X

	#define X(...) if (OBNAME_BIND(__VA_ARGS__) == NULL) goto error;
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
	#undef X

	rob = Py_BuildValue(
		"{"

	#define X(...) "(ss)O"
		JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
	#undef X

	#define X(...) "(sss)O"
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_SELECTION()
	#undef X

	#define X(...) "(ssss)O"
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
	#undef X

	#define X(...) "sO"
		JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
		JUNCTION_RESOURCE_ALLOCATION_SELECTION()
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
	#undef X

		"}"

	#define X(...) , FIRST_TWO(__VA_ARGS__), OBNAME(__VA_ARGS__)
		JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
	#undef X

	#define X(...) , FIRST_THREE(__VA_ARGS__), OBNAME(__VA_ARGS__)
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_SELECTION()
	#undef X

	#define X(...) , FIRST_THREE(__VA_ARGS__), "bind", OBNAME_BIND(__VA_ARGS__)
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
	#undef X

	#define X(...) , FIRST_TWO_IRI(__VA_ARGS__), OBNAME(__VA_ARGS__)
		JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
	#undef X

	#define X(...) , FIRST_THREE_IRI(__VA_ARGS__), OBNAME(__VA_ARGS__)
		JUNCTION_RESOURCE_ALLOCATION_SELECTION()
	#undef X

	#define X(...) , FIRST_THREE_IRI_PORT(__VA_ARGS__), OBNAME(__VA_ARGS__)
		JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
		JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
	#undef X

	);

	error:
	{
		#define X(...) Py_DECREF(OBNAME(__VA_ARGS__));
			JUNCTION_RESOURCE_ALLOCATION_DEFAULTS()
			JUNCTION_RESOURCE_ALLOCATION_PROTOCOL()
			JUNCTION_RESOURCE_ALLOCATION_SELECTION()
		#undef X
		#define X(...) Py_DECREF(OBNAME_BIND(__VA_ARGS__));
			JUNCTION_RESOURCE_ALLOCATION_BIND_PROTOCOL()
		#undef X
	}

	return(rob);
}

/**
	# No functions. Only types.
*/
#define MODULE_FUNCTIONS()

#include <fault/python/module.h>

INIT(PyDoc_STR("Kernel based Traffic implementation.\n"))
{
	PyObj mod = NULL;

	if (new_array == NULL)
	{
		/*
			# For Sockets rallocate.
		*/
		new_array = _init_intarray();
		if (new_array == NULL)
			return(NULL);
	}

	CREATE_MODULE(&mod);

	if (mod == NULL)
		return(NULL);
	else
	{
		polarity_objects[0] = PyLong_FromLong(1);
		polarity_objects[1] = PyLong_FromLong(-1);

		if (polarity_objects[0] == NULL || polarity_objects[1] == NULL)
		{
			Py_XDECREF(polarity_objects[0]);
			Py_XDECREF(polarity_objects[1]);
			polarity_objects[0] = NULL;
			polarity_objects[1] = NULL;
			goto error;
		}
	}

	#if FV_INJECTIONS()
		/*
			# Need this to help with the skip condition in the tests.
		*/
		#ifdef F_SETNOSIGPIPE
			PyModule_AddIntConstant(mod, "F_SETNOSIGPIPE", 1);
		#else
			PyModule_AddIntConstant(mod, "F_SETNOSIGPIPE", 0);
		#endif
	#endif

	if (PyType_Ready(&(jxi_type)))
		goto error;

	/*
		# Initialize Channel types.
	*/
	#define ID(NAME, IGNORED) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(mod, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		TRANSIT_TYPES()
		PY_TYPES()
	#undef ID

	/*
		# Provides mapping for Junction.rallocate()
		# Must be initialized after types are Ready'd.
	*/
	_jra_map = _init_junction_rallocation();
	if (_jra_map == NULL)
		goto error;

	/**
		# Setup exception instances.
	*/
	{
		PyExc_ChannelionViolation = PyErr_NewException(PYTHON_MODULE_PATH("ChannelionViolation"), NULL, NULL);
		if (PyExc_ChannelionViolation == NULL)
			goto error;

		if (PyModule_AddObject(mod, "ChannelionViolation", PyExc_ChannelionViolation) < 0)
		{
			Py_DECREF(PyExc_ChannelionViolation);
			PyExc_ChannelionViolation = NULL;
			goto error;
		}
	}

	return(mod);

	error:
	{
		DROP_MODULE(mod);
		return(NULL);
	}
}
