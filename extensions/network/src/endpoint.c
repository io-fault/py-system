#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

#include <fault/libc.h>
#include <fault/python/environ.h>
#include <fault/python/injection.h>

#include <endpoint.h>

extern PyTypeObject EndpointType;

#ifndef HAVE_STDINT_H
	/* relying on Python's checks */
	#include <stdint.h>
#endif

/**
	// Retrieve the string identifier of the given socket type code, &socktype.

	// [ Parameters ]
	// /socktype/
		// The integer code of the socket type to resolve.

	// [ Returns ]
	// A constant string used by the Python interfaces to select a socket type.
*/
static const char *
transport_type_string(int socktype)
{
	switch (socktype)
	{
		case SOCK_STREAM:
			return("octets");
		break;

		case SOCK_DGRAM:
			return("datagrams");
		break;

		#ifdef SOCK_SEQPACKET
			case SOCK_SEQPACKET:
				return("packets");
			break;
		#endif

		default:
			return("unknown");
		break;
	}

	return("error");
}

/**
	// Identify the socket type code that is being referred to by the given string, &identifier.

	// [ Engineering ]
	// Use a real hash.
*/
int
nw_socket_type(const char *identifier)
{
	int max = 9;
	char hash = 0, *p;

	for (p=(char *)identifier; *p && max > 0; ++p)
	{
		hash ^= *p;
		--max;
	}

	switch(hash)
	{
		case ('o'^'c'^'t'^'e'^'t'^'s'):
			if (strcmp("octets", identifier) != 0)
				return(-1);
			return(SOCK_STREAM);
		break;

		case ('d'^'a'^'t'^'a'^'g'^'r'^'a'^'m'^'s'):
			if (strcmp("datagrams", identifier) != 0)
				return(-1);
			return(SOCK_DGRAM);
		break;

		#ifdef SOCK_RAW
			case ('r'^'a'^'w'):
				if (strcmp("raw", identifier) != 0)
					return(-1);
				return(SOCK_RAW);
			break;
		#endif

		#ifdef SOCK_SEQPACKETS
			case ('p'^'a'^'c'^'k'^'e'^'t'^'s'):
				if (strcmp("packets", identifier) != 0)
					return(-1);
				return(SOCK_SEQPACKETS);
			break;
		#endif

		/* for interface binds */
		case ('s'^'o'^'c'^'k'^'e'^'t'^'s'):
			if (strcmp("sockets", identifier) != 0)
				return(-1);
			return(SOCK_STREAM);
		break;

		default:
			return(-1);
		break;
	}

	return(-2);
}

static int
interpret_transport(PyObj ob, int *out)
{
	if (ob == Py_None || ob == NULL)
	{
		*out = 0;
		return(0);
	}
	else if (PyLong_Check(ob))
	{
		*out = PyLong_AsLong(ob);
		if (PyErr_Occurred())
			return(-2);
	}
	else if (PyUnicode_Check(ob))
	{
		PyObj name;

		if (!PyUnicode_FSConverter(ob, &name))
			return(-2);

		*out = getprotobyname(PyBytes_AS_STRING(name))->p_proto;
		Py_DECREF(name);
	}
	else
	{
		PyErr_SetString(PyExc_ValueError, "transport identifier is not recognized");
		return(-1);
	}

	return(0);
}

static int
interpret_type(PyObj ob, int *out)
{
	if (ob == Py_None || ob == NULL)
	{
		*out = SOCK_STREAM;
	}
	else if (PyLong_Check(ob))
	{
		*out = PyLong_AsLong(ob);
		if (PyErr_Occurred())
			return(-2);
	}
	else if (PyUnicode_Check(ob))
	{
		PyObj name;

		if (!PyUnicode_FSConverter(ob, &name))
			return(-2);

		*out = nw_socket_type(PyBytes_AS_STRING(name));
		Py_DECREF(name);
	}
	else
	{
		PyErr_SetString(PyExc_ValueError, "socket type identifier is not recognized");
		return(-1);
	}

	return(0);
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
get_port(any_addr_t *ss, struct aport_t *dst, size_t dstlen)
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
	// Convert the address referenced by &ss into a string written
	// into &dst.

	// [ Parameters ]
	// /ss/
		// Source address structure.
	// /dst/
		// Destination memory buffer for the address string
		// to be written to.
	// /dstlen/
		// Length of &dst string.

	// [ Returns ]
	// The &ss parameter is the destination of the address
	// described in &dst.
*/
static void
get_address(any_addr_t *ss, char *dst, size_t dstlen)
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

/**
	// ParseTuple converter for IPv4 addresses.
*/
int
ip4_from_object(PyObj ob, void *out)
{
	ip4_addr_t *ref = out;
	char *address;
	unsigned short port = 0;
	Py_ssize_t len = 0;
	int r;

	/*
		// Presume an (address, port) pair.
	*/
	if (Py_TYPE(ob) == (PyTypeObject *) &EndpointType)
	{
		Endpoint E = (Endpoint) ob;

		if (Endpoint_GetAddress(E)->ss_family != ip4_pf)
		{
			PyErr_SetString(PyExc_TypeError, "address is not an IPv4 endpoint");
			return(0);
		}

		memcpy(((char *) ref), ((char *) Endpoint_GetAddress(E)), Endpoint_GetLength(E));
		return(1);
	}
	else if (PyUnicode_Check(ob))
	{
		address = PyUnicode_AsUTF8AndSize(ob, NULL);
		if (address == NULL)
			return(0);
	}
	else if (PyArg_ParseTuple(ob, "s#H", &address, &len, &port))
	{
		ip4_port_field(ref) = htons(port);
	}
	else
		return(0); /* ParseTuple failure */

	ERRNO_RECEPTACLE(0, &r, inet_pton, ip4_pf, address, (void *) &ip4_addr_field(ref));
	if (r < 1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	/*
		// Allows receiver to detect that the struct was filled out.
	*/
	ref->sin_family = ip4_pf;
	ip4_init_length(ref);

	return(1);
}

/**
	// ParseTuple converter for IPv6 addresses.
*/
int
ip6_from_object(PyObj ob, void *out)
{
	ip6_addr_t *ref = out;
	char *address;
	unsigned short port = 0;
	Py_ssize_t len = 0;
	uint32_t flowinfo = 0;
	int r;

	/*
		// Assume an (address, port|, flow). (scope_id?)
	*/
	if (Py_TYPE(ob) == (PyTypeObject *) &EndpointType)
	{
		Endpoint E = (Endpoint) ob;

		if (Endpoint_GetAddress(E)->ss_family != ip6_pf)
		{
			PyErr_SetString(PyExc_TypeError, "given endpoint is not an IPv6 endpoint");
			return(0);
		}

		memcpy(((char *) ref), ((char *) Endpoint_GetAddress(E)), Endpoint_GetLength(E));
		return(1);
	}
	else if (PyUnicode_Check(ob))
	{
		address = PyUnicode_AsUTF8AndSize(ob, NULL);
		if (address == NULL)
			return(0);
	}
	else if (PyArg_ParseTuple(ob, "s#H|I", &address, &len, &port, &flowinfo))
	{
		ip6_port_field(ref) = htons(port);
	}
	else
		return(0); /* ParseTuple failure */

	ERRNO_RECEPTACLE(0, &r, inet_pton, ip6_pf, address, (void *) &ip6_addr_field(ref));
	if (r < 1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	/*
		// Allows receiver to detect that the struct was filled out.
	*/
	ref->sin6_family = ip6_pf;
	ref->sin6_flowinfo = flowinfo;
	ip6_init_length(ref);

	return(1);
}

static int
local_port_index(char *buf)
{
	int i, pos;

	pos = strlen(buf);
	for (i = pos; i > 0 && buf[i] != '/'; --i);

	return(i);
}

void
local_str(char *dst, size_t dstsize, local_addr_t *addr)
{
	int i, pos;
	strncpy(dst, local_addr_field(addr), dstsize);

	/* find the final slash */
	pos = strlen(dst);
	for (i = pos; i > 0 && dst[i] != '/'; --i);

	/* dirname; nul-terminate *after* the slash */
	if (dst[i] == '/')
		dst[i+1] = 0;
	else
		dst[i] = 0;
}

void
local_port(struct aport_t *port, size_t dstsize, local_addr_t *addr)
{
	int i, pos;
	char *buf = local_addr_field(addr);

	/* find the final slash */
	pos = strlen(buf);
	for (i = pos; i > 0 && buf[i] != '/'; --i);

	/* basename; starts *after* the slash */
	if (buf[i] == '/')
		strncpy(port->data.filename, &(buf[i+1]), NAME_MAX);
	else
		strncpy(port->data.filename, &(buf[i]), NAME_MAX);
}

/**
	// ParseTuple converter for local file system sockets.
*/
int
local_from_object(PyObj ob, void *out)
{
	PyObj address = NULL, port = NULL;
	local_addr_t *ref = out;

	if (Py_TYPE(ob) == (PyTypeObject *) &EndpointType)
	{
		Endpoint E = (Endpoint) ob;

		if (Endpoint_GetAddress(E)->ss_family != local_pf)
		{
			PyErr_SetString(PyExc_TypeError, "given endpoint is not a local endpoint");
			return(0);
		}

		memcpy(((char *) ref), ((char *) Endpoint_GetAddress(E)), Endpoint_GetLength(E));
		return(1);
	}
	else if (PyUnicode_Check(ob) || PyBytes_Check(ob))
	{
		if (!PyUnicode_FSConverter(ob, &address))
			return(0);
	}
	else if (!PyArg_ParseTuple(ob, "O&|O&", PyUnicode_FSConverter, &address, PyUnicode_FSConverter, &port))
	{
		Py_XDECREF(address);
		Py_XDECREF(port);
		return(0);
	}
	else
	{
		Py_XINCREF(address);
		Py_XINCREF(port);
	}

	if (port != NULL)
	{
		snprintf((char *) &(local_addr_field(ref)), sizeof(local_addr_field(ref)), "%s/%s",
			PyBytes_AS_STRING(address), PyBytes_AS_STRING(port));

		Py_DECREF(port);
	}
	else
	{
		strncpy(local_addr_field(ref), PyBytes_AS_STRING(address), sizeof(local_addr_field(ref)));
	}

	Py_DECREF(address);

	ref->sun_family = local_pf;
	local_init_length(ref);

	return(1);
}

PyObj
endpoint_copy(PyObj endpoint)
{
	PyObj rob;
	Py_ssize_t units = Py_SIZE(endpoint);
	Endpoint r, a;

	rob = Py_TYPE(endpoint)->tp_alloc(Py_TYPE(endpoint), units);
	if (rob == NULL)
		return(NULL);

	r = (Endpoint) rob;
	a = (Endpoint) endpoint;

	memcpy(Endpoint_GetAddress(r), Endpoint_GetAddress(a), Endpoint_GetLength(a));

	r->len = a->len;
	r->type = a->type;
	r->transport = a->transport;

	return(rob);
}

static PyObj
endpoint_replace(PyObj self, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"address", "port", "transport", "type", NULL};
	char *domain;
	PyObj rob;
	PyObj address = NULL, port = NULL, transport = NULL, type = NULL;
	Endpoint E;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|OOOO", kwlist, &address, &port, &transport, &type))
		return(NULL);

	rob = endpoint_copy(self);
	if (rob == NULL)
		return(NULL);

	E = (Endpoint) rob;

	if (address != NULL)
	{
		int r = 0;

		switch (Endpoint_GetFamily(E))
		{
			case ip4_pf:
				r = ip4_from_object(address, Endpoint_GetAddress(E));
			break;

			case ip6_pf:
				r = ip6_from_object(address, Endpoint_GetAddress(E));
			break;

			case local_pf:
				r = local_from_object(address, Endpoint_GetAddress(E));
			break;

			default:
			{
				PyErr_Format(PyExc_RuntimeError,
					"cannot interpret address for the endpoint's address family");
				goto error;
			}
			break;
		}

		if (r == 0)
			goto error;
	}

	if (port != NULL)
	{
		switch (Endpoint_GetFamily(E))
		{
			case ip4_pf:
			{
				ip4_addr_t *addr = Endpoint_GetAddress(E);
				ip4_port_field(addr) = htons(PyLong_AsLong(port));
			}
			break;

			case ip6_pf:
			{
				ip6_addr_t *addr = Endpoint_GetAddress(E);
				ip6_port_field(addr) = htons(PyLong_AsLong(port));
			}
			break;

			case local_pf:
			{
				int i, buflen;
				local_addr_t *addr = Endpoint_GetAddress(E);

				if (PyUnicode_Check(port))
				{
					port = PyUnicode_EncodeFSDefault(port);
				}
				else if (!PyBytes_Check(port))
				{
					PyErr_Format(PyExc_TypeError, "invalid port type for local endpoint");
					goto error;
				}
				else
					Py_INCREF(port);

				i = local_port_index(local_port_field(addr)) + 1;
				buflen = sizeof(addr->sun_path) - i;

				if (PyBytes_GET_SIZE(port) > (buflen-1))
				{
					PyErr_Format(PyExc_ValueError, "insufficient memory for port in local endpoint");
				}
				else
				{
					char *buf = &(addr->sun_path[i]);
					int portlen = strlen(buf);
					memcpy(buf, PyBytes_AS_STRING(port), PyBytes_GET_SIZE(port));
					addr->sun_path[i + PyBytes_GET_SIZE(port)] = '\0';
				}

				Py_DECREF(port);
			}
			break;

			default:
			{
				PyErr_Format(PyExc_RuntimeError,
					"cannot interpret port for the endpoint's address family");
				goto error;
			}
			break;
		}

		if (PyErr_Occurred())
			goto error;
	}

	if (transport != NULL && interpret_transport(transport, &(E->transport)))
		goto error;
	if (type != NULL && interpret_type(type, &(E->type)))
		goto error;

	return(rob);

	error:
	{
		Py_DECREF(rob);
		return(NULL);
	}
}

#define A(AF) static PyObj endpoint_new_##AF(PyTypeObject *, PyObj, PyObj);
	ADDRESSING()
#undef A

static PyMethodDef endpoint_methods[] = {
	{"replace",
		(PyCFunction) endpoint_replace,
		METH_VARARGS|METH_KEYWORDS,
		PyDoc_STR("Create a new endpoint with the given fields overwritten.")
	},

	#define A(AF) \
		{"from_" #AF, \
			(PyCFunction) endpoint_new_##AF , \
			METH_VARARGS|METH_KEYWORDS|METH_CLASS, \
			PyDoc_STR("Direct constructor for the address family identified by the method name.") \
		},

		ADDRESSING()
	#undef A

	{NULL,}
};

static PyObj
endpoint_get_address_family(PyObj self, void *_)
{
	Endpoint E = (Endpoint) self;
	return(PyLong_FromLong(Endpoint_GetAddress(E)->ss_family));
}

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
endpoint_get_address(PyObj self, void *_)
{
	char addrstr[1024];
	Endpoint E = (Endpoint) self;

	get_address(Endpoint_GetAddress(E), addrstr, sizeof(addrstr));
	return(PyUnicode_FromString(addrstr));
}

static PyObj
endpoint_get_port(PyObj self, void *_)
{
	Endpoint E = (Endpoint) self;
	struct aport_t port;

	switch (get_port(Endpoint_GetAddress(E), &port, sizeof(port)))
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

	get_address(Endpoint_GetAddress(E), buf, sizeof(buf));
	port.kind = get_port(Endpoint_GetAddress(E), &port, sizeof(port));

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
	{"address_family", endpoint_get_address_family, NULL,
		PyDoc_STR(
			"The system address family identifier.")
	},

	{"address_type", endpoint_get_address_type, NULL,
		PyDoc_STR(
			"The type of addressing used to reference the endpoint.\n"
			"One of `'ip6'`, `'ip4'`, `'local'`, or `None` if family is unknown.")
	},

	{"address", endpoint_get_address, NULL,
		PyDoc_STR("The address portion of the endpoint.")
	},

	{"port", endpoint_get_port, NULL,
		PyDoc_STR("The port of the endpoint as an &int. &None if none.")
	},

	{"pair", endpoint_get_pair, NULL,
		PyDoc_STR("A newly constructed tuple consisting of the address and port attributes.")
	},

	{NULL,},
};

static PyMemberDef endpoint_members[] = {
	{"transport", T_INT, offsetof(struct Endpoint, transport), READONLY,
		PyDoc_STR("The transport protocol that should be used when connecting.")
	},

	{"type", T_INT, offsetof(struct Endpoint, type), READONLY,
		PyDoc_STR("The socket type to allocate when connecting.")
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
				char *amb = (char *) Endpoint_GetAddress(a);
				char *bmb = (char *) Endpoint_GetAddress(b);
				if (memcmp(amb, bmb, Endpoint_GetLength(a)) == 0)
				{
					rob = Py_True;
					Py_INCREF(rob);
					break;
				}
			}

			if (op == Py_NE)
			{
				/*
					// Invert result.
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
	// String representation suitable for text based displays.
*/
static PyObj
endpoint_str(PyObj self)
{
	Endpoint E = (Endpoint) self;
	char buf[PATH_MAX];
	struct aport_t port;
	PyObj rob;

	get_address(Endpoint_GetAddress(E), buf, sizeof(buf));

	port.kind = get_port(Endpoint_GetAddress(E), &port, sizeof(port));
	switch (port.kind)
	{
		case aport_kind_numeric2:
			rob = PyUnicode_FromFormat("[%s]:%d", buf, (int) port.data.numeric2);
		break;

		case aport_kind_filename:
			rob = PyUnicode_FromFormat("%s%s", buf, port.data.filename);
		break;

		case aport_kind_none:
		default:
			rob = PyUnicode_FromFormat("%s", buf);
		break;
	}

	return(rob);
}

#define A(DOMAIN) \
static PyObj \
endpoint_new_internal_##DOMAIN(PyTypeObject *subtype, PyObj rep) \
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
	E->type = -1; \
	E->transport = -1; \
	\
	if (! (DOMAIN##_from_object(rep, (DOMAIN##_addr_t *) Endpoint_GetAddress(E)))) \
	{ \
		Py_DECREF(rob); \
		return(NULL); \
	} \
	E->len = addrlen; \
	\
	return(rob); \
} \
\
static PyObj \
endpoint_new_##DOMAIN(PyTypeObject *subtype, PyObj args, PyObj kw) \
{ \
	PyObj address, transport = NULL, type = NULL; \
	static char *kwlist[] = {"address", "transport", "type", NULL}; \
	Endpoint E; \
	\
	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|OO", kwlist, &address, &transport, &type)) \
		return(NULL); \
	\
	E = (Endpoint) endpoint_new_internal_##DOMAIN(subtype, address); \
	if (E == NULL) \
		return(NULL); \
	if (interpret_transport(transport, &(E->transport))) \
		goto error; \
	if (interpret_type(type, &(E->type))) \
		goto error; \
	\
	return((PyObj) E); \
	error: \
	{ \
		Py_DECREF(E); \
		return(NULL); \
	} \
}

ADDRESSING()
#undef A

Endpoint
endpoint_create(int type, int transport, if_addr_ref_t addr, socklen_t addrlen)
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
	E->type = type;
	E->transport = transport;

	return(E);

	#undef endpoint_alloc
}

static PyObj
endpoint_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"domain", "address", "transport", "type", NULL};
	char *domain;
	PyObj rob = NULL, address, transport = NULL, type = NULL;
	Endpoint E;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "sO|OO", kwlist, &domain, &address, &transport, &type))
		return(NULL);

	if (0) ;
	#define A(DOMAIN) \
		else if (strcmp(domain, #DOMAIN) == 0) \
			rob = endpoint_new_internal_##DOMAIN(subtype, address);
		ADDRESSING()
	#undef A
	else
	{
		PyErr_Format(PyExc_ValueError, "unknown address domain: %s", domain);
		return(NULL);
	}

	if (rob == NULL)
		return(NULL);

	E = (Endpoint) rob;
	if (interpret_transport(transport, &(E->transport)))
		goto error;
	if (interpret_type(type, &(E->type)))
		goto error;

	return(rob);

	error:
	{
		Py_DECREF(rob);
		return(NULL);
	}
}

PyDoc_STRVAR(endpoint_doc, "System endpoint type for binding and connecting sockets.");

PyTypeObject
EndpointType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Endpoint"), /* tp_name */
	sizeof(struct Endpoint),        /* tp_basicsize */
	sizeof(void *),                 /* tp_itemsize */
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
	endpoint_str,                   /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	NULL,                           /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,             /* tp_flags */
	endpoint_doc,                   /* tp_doc */
	NULL,                           /* tp_traverse */
	NULL,                           /* tp_clear */
	endpoint_richcompare,           /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	endpoint_methods,               /* tp_methods */
	endpoint_members,               /* tp_members */
	endpoint_getset,                /* tp_getset */
	NULL,                           /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	NULL,                           /* tp_alloc */
	endpoint_new,                   /* tp_new */
};
