/**
	// Various high-level interfaces for working with the system's network and netdb.
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
#include <fault/python/injection.h>

#include <endpoint.h>
#include <kcore.h>

/*
	// Local endpoint functions and macros.
*/
extern PyTypeObject EndpointType;
int nw_socket_type(const char *);
Endpoint endpoint_create(int, int, if_addr_ref_t, socklen_t);
Endpoint endpoint_copy(PyObj);

#define Endpoint_Check(E) (PyObject_IsInstance(E, (PyObj) &EndpointType))

/*
	// Manage retry state for limiting the number of times we'll accept EINTR.
*/
#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
#endif
#define _RETRY_STATE _avail_retries
#define RETRY_STATE_INIT int _RETRY_STATE = CONFIG_SYSCALL_RETRY
#define LIMITED_RETRY() \
	do { \
		if (_RETRY_STATE > 0) { \
			errno = 0; \
			--_RETRY_STATE; \
			goto RETRY_SYSCALL; \
		} \
	} while(0);
#define UNLIMITED_RETRY() errno = 0; goto RETRY_SYSCALL;

/**
	// The failure structure prefers to have a name with the code.
*/
#define X_EAI_ERRORS() \
	X_EAI(EAI_AGAIN) \
	X_EAI(EAI_BADFLAGS) \
	X_EAI(EAI_FAIL) \
	X_EAI(EAI_FAMILY) \
	X_EAI(EAI_MEMORY) \
	X_EAI(EAI_NONAME) \
	X_EAI(EAI_SERVICE) \
	X_EAI(EAI_SOCKTYPE) \
	X_EAI(EAI_SYSTEM) \
	X_EAI(EAI_OVERFLOW)

/**
	// Lookup the name of the EAI define that is associated with the given error code.

	// [ Parameters ]
	// /code/
		// The EAI error.

	// [ Returns ]
	// Constant string pointer.
*/
static const char *
error_name_gai(int code)
{
	switch (code)
	{
		#define X_EAI(X) \
			case X: \
				return(#X); \
			break;

			X_EAI_ERRORS()
		#undef X_EAI

		#ifdef EAI_BADHINTS
			case (EAI_BADHINTS):
				return("EAI_BADHINTS");
			break;
		#endif

		#ifdef EAI_ADDRFAMILY
			case (EAI_ADDRFAMILY):
				return("EAI_ADDRFAMILY");
			break;
		#endif

		#ifdef EAI_PROTOCOL
			case (EAI_PROTOCOL):
				return("EAI_PROTOCOL");
			break;
		#endif

		#ifdef EAI_NODATA
			case (EAI_NODATA):
				return("EAI_NODATA");
			break;
		#endif

		default:
			return("");
		break;
	}
}

static PyObj
construct_error(int code)
{
	char buf[16];
	PyObj rob = NULL;

	rob = PyList_New(1);
	if (rob == NULL)
		return(NULL);

	snprintf(buf, sizeof(buf), "%d", code);
	PyList_SET_ITEM(rob, 0, Py_BuildValue("ssss", "error", error_name_gai(code), buf, gai_strerror(code)));
	if (PyList_GET_ITEM(rob, 0) == NULL)
		goto error;

	return(rob);

	error:
	{
		Py_DECREF(rob);
		return(NULL);
	}
}

static PyObj
nw_getaddrinfo(const char *stream_type, const char *namestr, const char *servicestr, int socktype, int flags)
{
	PyObj addrlist, rob;

	int r;
	struct addrinfo hints = {0}, *info, *i;

	hints.ai_family = AF_UNSPEC;
	hints.ai_protocol = 0;
	hints.ai_socktype = socktype;
	hints.ai_flags = flags;

	r = getaddrinfo(namestr, servicestr ? servicestr : "", &hints, &info);
	if (r != 0)
	{
		switch (r)
		{
			case EAI_AGAIN:
			{
				/*
					// Try again signal.
				*/
				Py_RETURN_NONE;
			}
			break;

			default:
			{
				if (r == EAI_SYSTEM)
				{
					/*
						// Try again signal.
					*/
					switch (errno)
					{
						case EAGAIN:
						case EINTR:
							Py_RETURN_NONE;
						break;

						default:
							PyErr_SetFromErrno(PyExc_OSError);
							return(NULL);
						break;
					}
				}

				addrlist = construct_error(r);
				if (addrlist == NULL)
					return(NULL);

				rob = Py_BuildValue("ON", Py_None, addrlist);

				return(rob);
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

		x = (PyObj) endpoint_create(i->ai_socktype, i->ai_protocol, i->ai_addr, i->ai_addrlen);
		if (x == NULL)
			goto error;

		PyList_Append(addrlist, x);
		Py_DECREF(x);
		if (PyErr_Occurred())
			goto error;
	}

	rob = Py_BuildValue("sN", (info->ai_canonname?info->ai_canonname:""), addrlist);
	freeaddrinfo(info);

	return(rob);

	error:
	{
		Py_XDECREF(addrlist);
		freeaddrinfo(info);
		return(NULL);
	}
}

/**
	// Resolve transport selectors for the given host and service.
*/
static PyObj
nw_select_endpoints_gai(PyObj mod, PyObj args)
{
	const char *namestr;
	const char *servicestr = NULL;
	const char *transferstr = NULL;
	int socktype;

	if (!PyArg_ParseTuple(args, "z|zz", &namestr, &servicestr, &transferstr))
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

	return(nw_getaddrinfo("octets", namestr, servicestr, socktype, AI_CANONNAME|AI_ADDRCONFIG));
}

/**
	// Find service interfaces using GAI.
*/
static PyObj
nw_select_interfaces_gai(PyObj mod, PyObj args)
{
	const char *servicestr = NULL;
	const char *transferstr = NULL;
	const char *namestr = NULL;
	int socktype;

	if (!PyArg_ParseTuple(args, "|zzz", &servicestr, &transferstr, &namestr))
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

	return(nw_getaddrinfo("sockets", namestr, servicestr, socktype, AI_CANONNAME|AI_PASSIVE|AI_ADDRCONFIG));
}

static kport_t
bind_sequence(Endpoint ep)
{
	kcall_t kc;
	kport_t kp;
	kp = socket(Endpoint_GetFamily(ep), ep->type, ep->transport);

	if (kp == -1)
		return(-kc_socket);

	if (fcntl(kp, F_SETFL, O_NONBLOCK) == -1)
	{
		kc = kc_fcntl;
		goto error;
	}

	if (bind(kp, Endpoint_GetAddress(ep), Endpoint_GetLength(ep)))
	{
		kc = kc_bind;
		goto error;
	}

	return(kp);

	error:
	{
		close(kp);
	}

	return(-kc);
}

static PyObj
nw_bind_endpoint(Endpoint ep)
{
	PyObj rob;
	kport_t kp = -1;

	Py_BEGIN_ALLOW_THREADS
	{
		kp = bind_sequence(ep);
	}
	Py_END_ALLOW_THREADS

	if (kp < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	rob = PyLong_FromLong(kp);
	if (rob == NULL)
		close(kp);

	return(rob);
}

static kport_t
service_sequence(Endpoint ep, int backlog)
{
	kcall_t kc;
	kport_t kp;
	kp = socket(Endpoint_GetFamily(ep), SOCK_STREAM, ep->transport);

	if (kp == -1)
		return(-kc_socket);

	if (fcntl(kp, F_SETFL, O_NONBLOCK) == -1)
	{
		kc = kc_fcntl;
		goto error;
	}

	if (bind(kp, Endpoint_GetAddress(ep), Endpoint_GetLength(ep)))
	{
		kc = kc_bind;
		goto error;
	}

	if (listen(kp, backlog))
	{
		kc = kc_listen;
		goto error;
	}

	return(kp);

	error:
	{
		close(kp);
	}

	return(-kc);
}

static PyObj
nw_service_endpoint(Endpoint ep, int backlog)
{
	PyObj rob;
	kport_t kp = -1;

	Py_BEGIN_ALLOW_THREADS
	{
		kp = service_sequence(ep, backlog);
	}
	Py_END_ALLOW_THREADS

	if (kp < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	rob = PyLong_FromLong(kp);
	if (rob == NULL)
		close(kp);

	return(rob);
}

static int
i_connect(int fd, if_addr_ref_t addr, socklen_t addrlen)
{
	RETRY_STATE_INIT;
	int r;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, connect, fd, addr, addrlen);

	if (r)
	{
		switch (errno)
		{
			case EWOULDBLOCK:
			case EINPROGRESS:
			case EISCONN:
				errno = 0;
			break;

			case EINTR:
				LIMITED_RETRY()
			default:
				return(-1);
			break;
		}
	}

	return(0);
}

static kport_t
connect_sequence(Endpoint ep)
{
	kcall_t kc = 0;
	kport_t kp;

	kp = socket(Endpoint_GetFamily(ep), ep->type, ep->transport);

	if (kp == -1)
		return(-kc_socket);

	if (fcntl(kp, F_SETFL, O_NONBLOCK) == -1)
	{
		kc = kc_fcntl;
		goto error;
	}

	if (i_connect(kp, Endpoint_GetAddress(ep), Endpoint_GetLength(ep)))
	{
		kc = kc_connect;
		goto error;
	}

	return(kp);

	error:
	{
		close(kp);
	}

	return(-kc);
}

static PyObj
nw_connect_endpoint(Endpoint ep)
{
	PyObj rob;
	kport_t kp = -1;

	Py_BEGIN_ALLOW_THREADS
	{
		kp = connect_sequence(ep);
	}
	Py_END_ALLOW_THREADS

	if (kp < 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	rob = PyLong_FromLong(kp);
	if (rob == NULL)
		close(kp);

	return(rob);
}

/**
	// Python interface to POSIX connect.
*/
static PyObj
nw_connect(PyObj module, PyObj args, PyObj kw)
{
	PyObj ob;
	static char *kwlist[] = {"address", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O", kwlist, &ob))
		return(NULL);

	if (Endpoint_Check(ob) < 0)
		return(NULL);

	return(nw_connect_endpoint((Endpoint) ob));
}

/**
	// Python interface to POSIX bind and listen.
*/
static PyObj
nw_service(PyObj module, PyObj args, PyObj kw)
{
	PyObj ob;
	static char *kwlist[] = {"interface", "backlog", NULL};
	int backlog = 16;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", kwlist, &ob, &backlog))
		return(NULL);

	if (Endpoint_Check(ob) < 0)
		return(NULL);

	return(nw_service_endpoint((Endpoint) ob, backlog));
}

/**
	// Python interface to POSIX bind.
*/
static PyObj
nw_bind(PyObj module, PyObj args, PyObj kw)
{
	PyObj ob;
	static char *kwlist[] = {"interface", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O", kwlist, &ob))
		return(NULL);

	if (Endpoint_Check(ob) < 0)
		return(NULL);

	return(nw_bind_endpoint((Endpoint) ob));
}

/**
	// Capsule target providing access to &EndpointType creation and duplication.
*/
struct EndpointAPI _ep_apis = {
	&EndpointType,
	endpoint_create,
	endpoint_copy,
	ip4_from_object,
	ip6_from_object,
	local_from_object,
};

#define PYTHON_TYPES() \
	ID(Endpoint)

#define MODULE_FUNCTIONS() \
	PYMETHOD( \
		select_endpoints, nw_select_endpoints_gai, METH_VARARGS, \
			"Resolve the Endpoints of the given host and service using (system/manual)`getaddrinfo`.") \
	PYMETHOD( \
		select_interfaces, nw_select_interfaces_gai, METH_VARARGS, \
			"Identify the interfaces to bind to for the service using (system/manual)`getaddrinfo`.") \
	PYMETHOD( \
		connect, nw_connect, METH_VARARGS|METH_KEYWORDS, \
			"Connect new sockets using the given endpoints.") \
	PYMETHOD( \
		service, nw_service, METH_VARARGS|METH_KEYWORDS, \
			"Create a listening socket using the given endpoint as the interface.") \
	PYMETHOD( \
		bind, nw_bind, METH_VARARGS|METH_KEYWORDS, \
			"Create and bind a socket to an interface; usually for connection-less communication.") \

#include <fault/python/module.h>
INIT(module, 0, PyDoc_STR("System network interfaces.\n"))
{
	PyObj api_ob;

	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(module, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		PYTHON_TYPES()
	#undef ID

	api_ob = PyCapsule_New(&_ep_apis, "_endpoint_api", NULL);
	if (api_ob == NULL)
		return(-1);

	if (PyModule_AddObject(module, "_endpoint_api", api_ob))
	{
		Py_DECREF(api_ob);
		return(-1);
	}

	return(0);

	error:
	{
		return(-1);
	}
}
