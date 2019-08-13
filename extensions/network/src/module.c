/**
	// Various high-level interfaces for working with the system's network and netdb.
	// Currently this only includes getaddrinfo interfaces.

	// Address resolution only supports PF_INET, PF_INET6, and PF_LOCAL.
	// The transport protocols returned are not directly exposed; the socket type
	// is expected to unamibguously imply the protocol. This is not always
	// coherent.
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

#include "endpoint.h"

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

				rob = Py_BuildValue("OO", Py_None, addrlist);
				Py_DECREF(addrlist);

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

	rob = Py_BuildValue("sO", (info->ai_canonname?info->ai_canonname:""), addrlist);

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
	// Resolve transport selectors for the given host and service.
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
	// Find service interfaces using GAI.
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

static PyObj
nw_service_endpoint(Endpoint ep, int backlog)
{
	PyObj rob;
	int fd = -1, err = 0;

	Py_BEGIN_ALLOW_THREADS
	{
		fd = socket(Endpoint_GetFamily(ep), SOCK_STREAM, ep->transport);

		if (fd == -1)
			err = 1;
		else if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
		{
			close(fd);
			err = 1;
		}
		else if (bind(fd, Endpoint_GetAddress(ep), Endpoint_GetLength(ep)))
		{
			close(fd);
			err = 1;
		}
		else if (listen(fd, backlog))
		{
			close(fd);
			err = 1;
		}
	}
	Py_END_ALLOW_THREADS

	if (err)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	rob = PyLong_FromLong(fd);
	if (rob == NULL)
		close(fd);

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

static PyObj
nw_connect_endpoint(Endpoint ep)
{
	PyObj rob;
	int fd = -1, err = 0;

	Py_BEGIN_ALLOW_THREADS
	{
		fd = socket(Endpoint_GetFamily(ep), ep->type, ep->transport);

		if (fd == -1)
			err = 1;
		else if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
		{
			close(fd);
			err = 1;
		}
		else if (i_connect(fd, Endpoint_GetAddress(ep), Endpoint_GetLength(ep)))
		{
			close(fd);
			err = 1;
		}
	}
	Py_END_ALLOW_THREADS

	if (err)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	rob = PyLong_FromLong(fd);
	if (rob == NULL)
		close(fd);

	return(rob);
}

static PyObj
nw_connect(PyObj module, PyObj args)
{
	PyObj ob;

	if (!PyArg_ParseTuple(args, "O", &ob))
		return(NULL);

	if (Endpoint_Check(ob) < 0)
		return(NULL);

	return(nw_connect_endpoint((Endpoint) ob));
}

static PyObj
nw_service(PyObj module, PyObj args, PyObj kw)
{
	PyObj ob;
	static char *kwlist[] = {"endpoint", "backlog", NULL};
	int backlog = 16;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O|i", &kwlist, &ob, &backlog))
		return(NULL);

	if (Endpoint_Check(ob) < 0)
		return(NULL);

	return(nw_service_endpoint((Endpoint) ob, backlog));
}

/*
	// Works with statically defined types.
*/
struct EndpointAPI _ep_apis = {&EndpointType, endpoint_create};

#define PYTHON_TYPES() \
	ID(Endpoint)

#define MODULE_FUNCTIONS() \
	PYMETHOD( \
		select_transports, nw_select_transports_gai, METH_VARARGS, \
			"Resolve the transport of the given host and service using (system/manual)`getaddrinfo`.") \
	PYMETHOD( \
		select_interfaces, nw_select_interfaces_gai, METH_VARARGS, \
			"Identify the interfaces to use for the service using (system/manual)`getaddrinfo`.") \
	PYMETHOD( \
		connect, nw_connect, METH_VARARGS, \
			"Connect new sockets using the given endpoints.") \
	PYMETHOD( \
		service, nw_service, METH_VARARGS|METH_KEYWORDS, \
			"Create a listening socket using the given endpoint as the interface.") \

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

	api_ob = PyCapsule_New(&_ep_apis, PYTHON_MODULE_PATH("_api"), NULL);
	if (api_ob == NULL)
		return(-1);

	if (PyModule_AddObject(module, "_api", api_ob))
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
