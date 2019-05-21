/**
	# Various high-level interfaces for working with the system's network and netdb.
	# Currently this only includes getaddrinfo interfaces.

	# Address resolution only supports PF_INET, PF_INET6, and PF_LOCAL.
	# The transport protocols returned are not directly exposed; the socket type
	# is expected to unamibguously imply the protocol. This is not always
	# coherent.
*/
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

static const char *
transport_type_string(const char *stream_type, int socktype)
{
	switch (socktype)
	{
		case SOCK_STREAM:
			return(stream_type);
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
}

static PyObj
interpret_address_record(const char *stream_type, int socktype, struct addrinfo *x)
{
	char addr_buf[1024];
	char port_buf[8];
	const char *port;
	const char *ts, *proto;

	switch (x->ai_family)
	{
		/**
			# Currently, it is likely that PF_LOCAL would only occur unnaturally.
		*/
		#ifdef PF_LOCAL
			case PF_LOCAL:
			{
				PyObj addr_ob, port_ob, rob;
				char *i, *ls;
				struct sockaddr_un *la = (struct sockaddr_un *) x->ai_addr;

				for (i = la->sun_path; i != '\0'; ++i)
				{
					if (*i == '/')
						ls = i;
				}

				memcpy((void *) addr_buf, la->sun_path, (i - la->sun_path));
				((char *)addr_buf)[ls - la->sun_path] = '\0';
				port = ls;

				addr_ob = PyUnicode_DecodeFSDefault(addr_buf);
				if (addr_ob == NULL)
					return(NULL);

				port_ob = PyUnicode_DecodeFSDefault(port_buf);
				if (port_ob == NULL)
				{
					Py_DECREF(addr_ob);
					return(NULL);
				}

				proto = transport_type_string(stream_type, x->ai_socktype);
				rob = Py_BuildValue("ssOO", proto, "local", addr_ob, port_ob);
				Py_DECREF(addr_ob);
				Py_DECREF(port_ob);

				return(rob);
			}
			break;
		#endif

		case PF_INET:
		{
			uint16_t p;
			struct sockaddr_in *la = (struct sockaddr_in *) x->ai_addr;

			if (inet_ntop(x->ai_family, &(la->sin_addr), addr_buf, INET_ADDRSTRLEN) == NULL)
			{
				PyErr_SetFromErrno(PyExc_OSError);
				return(NULL);
			}

			p = ntohs(la->sin_port);
			snprintf(port_buf, sizeof(port_buf), "%hu", p);
			port = port_buf;

			ts = "ip4";
		}
		break;

		case PF_INET6:
		{
			uint16_t p;
			struct sockaddr_in6 *la = (struct sockaddr_in6 *) x->ai_addr;

			if (inet_ntop(x->ai_family, &(la->sin6_addr), addr_buf, sizeof(addr_buf)) == NULL)
			{
				PyErr_SetFromErrno(PyExc_OSError);
				return(NULL);
			}

			p = ntohs(la->sin6_port);
			snprintf(port_buf, sizeof(port_buf), "%hu", p);
			port = port_buf;

			ts = "ip6";
		}
		break;

		default:
		{
			snprintf(addr_buf, sizeof(addr_buf), "%s", "");
			port = "";

			ts = "unknown";
		}
	}

	proto = transport_type_string(stream_type, x->ai_socktype);
	return(Py_BuildValue("ssss", proto, ts, addr_buf, port));
}

static void
set_gai_error(int code)
{
	/**
		# XXX: This exception will be specialized in the future and more data exposed.
	*/
	PyErr_Format(PyExc_Exception, "%s", gai_strerror(code));
}

/**
	# Convert transport type to POSIX socket type.
*/
static int
nw_socket_type(const char *transfertype)
{
	if (strcmp("datagrams", transfertype) == 0)
		return(SOCK_DGRAM);
	#ifdef SOCK_SEQPACKET
		else if (strcmp("packets", transfertype) == 0)
			return(SOCK_SEQPACKET);
	#endif
	else if (strcmp("octets", transfertype) == 0)
		return(SOCK_STREAM);
	else if (strcmp("sockets", transfertype) == 0)
		return(SOCK_STREAM);

	return(-1);
}

static PyObj
nw_getaddrinfo(const char *stream_type, const char *namestr, const char *portstr, int socktype, int flags)
{
	PyObj addrlist, rob;

	int r;
	struct addrinfo hints = {0}, *info, *i;

	hints.ai_family = AF_UNSPEC;
	hints.ai_protocol = 0;
	hints.ai_socktype = socktype;
	hints.ai_flags = flags;

	r = getaddrinfo(namestr, portstr ? portstr : "", &hints, &info);
	if (r != 0)
	{
		switch (r)
		{
			case EAI_AGAIN:
			{
				/*
					# Try again signal.
				*/
				Py_RETURN_NONE;
			}
			break;

			default:
			{
				set_gai_error(r);
				return(NULL);
			}
			break;
		}
	}

	addrlist = PyList_New(0);
	if (addrlist == NULL)
		return(NULL);

	for (i = info; i != NULL; i = i->ai_next)
	{
		PyObj x;

		x = interpret_address_record(stream_type, hints.ai_socktype, i);
		if (x == NULL)
			goto error;

		PyList_Append(addrlist, x);
		Py_DECREF(x);
		if (PyErr_Occurred())
			goto error;
	}

	if (info->ai_canonname != NULL)
		rob = Py_BuildValue("sO", info->ai_canonname, addrlist);
	else
		rob = Py_BuildValue("OO", Py_None, addrlist);

	freeaddrinfo(info);
	Py_DECREF(addrlist);

	return(rob);

	error:
	{
		Py_XDECREF(addrlist);
		freeaddrinfo(info);
		return(NULL);
	}
}

/**
	# Resolve transport selectors for the given host and service.
*/
static PyObj
nw_select_transports_gai(PyObj mod, PyObj args)
{
	const char *namestr;
	const char *portstr = NULL;
	const char *transferstr = NULL;
	int socktype;

	if (!PyArg_ParseTuple(args, "z|zz", &namestr, &portstr, &transferstr))
		return(NULL);

	if (transferstr != NULL)
	{
		socktype = nw_socket_type(transferstr);
		if (socktype == -1)
		{
			PyErr_SetString(PyExc_ValueError, "unknown or unsupported transfer type");
			return(NULL);
		}
	}
	else
		socktype = SOCK_STREAM;

	return(nw_getaddrinfo("octets", namestr, portstr, socktype, AI_CANONNAME|AI_ADDRCONFIG));
}

/**
	# Find service interfaces using GAI.
*/
static PyObj
nw_select_interfaces_gai(PyObj mod, PyObj args)
{
	const char *portstr = NULL;
	const char *transferstr = NULL;
	int socktype;

	if (!PyArg_ParseTuple(args, "|zz", &portstr, &transferstr))
		return(NULL);

	if (transferstr != NULL)
	{
		socktype = nw_socket_type(transferstr);
		if (socktype == -1)
		{
			PyErr_SetString(PyExc_ValueError, "unknown or unsupported transfer type");
			return(NULL);
		}
	}
	else
		socktype = SOCK_STREAM;

	return(nw_getaddrinfo("sockets", NULL, portstr, socktype, AI_CANONNAME|AI_PASSIVE|AI_ADDRCONFIG));
}

#define PYTHON_TYPES()
#define MODULE_FUNCTIONS() \
	PYMETHOD( \
		select_transports, nw_select_transports_gai, METH_VARARGS, \
			"Resolve the transport of the given host and service using (system/manual)`getaddrinfo`.") \
	PYMETHOD( \
		select_interfaces, nw_select_interfaces_gai, METH_VARARGS, \
			"Identify the interfaces to use for the service using (system/manual)`getaddrinfo`.") \

#include <fault/python/module.h>
INIT(PyDoc_STR("System network interfaces.\n"))
{
	PyObj mod = NULL;

	CREATE_MODULE(&mod);
	if (mod == NULL)
		return(NULL);

	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(mod, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		PYTHON_TYPES()
	#undef ID

	return(mod);

	error:
	{
		DROP_MODULE(mod);
		return(NULL);
	}
}
