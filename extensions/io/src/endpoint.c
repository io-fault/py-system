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

#define errpf(...) fprintf(stderr, __VA_ARGS__)

#ifndef HAVE_STDINT_H
	/* relying on Python's checks */
	#include <stdint.h>
#endif

/**
	// ParseTuple converter for IPv4 addresses.
**/
int
ip4_from_object(PyObj ob, void *out)
{
	ip4_addr_t *ref = out;
	char *interface;
	unsigned short port;
	Py_ssize_t len = 0;
	int r;

	/*
		// Presume an (interface, port) pair.
	*/
	if (Py_TYPE(ob) == (PyTypeObject *) endpointtype)
	{
		Endpoint E = (Endpoint) ob;

		if (Endpoint_GetAddress(E)->ss_family != ip4_pf)
		{
			PyErr_SetString(PyExc_TypeError, "given endpoint is not an IPv4 endpoint");
			return(0);
		}

		memcpy(((char *) ref), ((char *) Endpoint_GetAddress(E)), Endpoint_GetLength(E));
		return(1);
	}
	else if (!PyArg_ParseTuple(ob, "s#H", &interface, &len, &port))
		return(0);

	ERRNO_RECEPTACLE(0, &r, inet_pton, ip4_pf, interface, (void *) &ip4_addr_field(ref));
	if (r < 1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	ip4_port_field(ref) = htons(port);

	/*
		// Allows receiver to detect that the struct was filled out.
	*/
	ref->sin_family = ip4_pf;
	ip4_init_length(ref);
	return(1);
}

/**
	// ParseTuple converter for IPv6 addresses.
**/
int
ip6_from_object(PyObj ob, void *out)
{
	ip6_addr_t *ref = out;
	char *interface;
	unsigned short port;
	Py_ssize_t len = 0;
	uint32_t flowinfo = 0;
	int r;

	/*
		// Assume an (interface, port|, flow). (scope_id?)
	*/
	if (Py_TYPE(ob) == (PyTypeObject *) endpointtype)
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
	else if (!PyArg_ParseTuple(ob, "s#H|I", &interface, &len, &port, &flowinfo))
		return(0);

	ERRNO_RECEPTACLE(0, &r, inet_pton, ip6_pf, interface, (void *) &ip6_addr_field(ref));
	if (r < 1)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(0);
	}

	ip6_port_field(ref) = htons(port);

	/*
		// Allows receiver to detect that the struct was filled out.
	*/
	ref->sin6_family = ip6_pf;
	ref->sin6_flowinfo = flowinfo;
	ip6_init_length(ref);
	return(1);
}

void
local_str(char *dst, size_t dstsize, local_addr_t *addr)
{
	int i, pos;
	strncpy(dst, local_addr_field(addr), dstsize);

	/* find the slash */
	pos = strlen(dst);
	for (i = pos; i > 0 && dst[i] != '/'; --i);

	/* dirname; nul-terminate *after* the slash */
	dst[i] = 0;
}

void
local_port(struct aport_t *port, size_t dstsize, local_addr_t *addr)
{
	int i, pos;
	char *buf = local_addr_field(addr);

	/* find the slash */
	pos = strlen(buf);
	for (i = pos; i > 0 && buf[i] != '/'; --i);

	/* basename; starts *after* the slash */
	strncpy(port->data.filename, &(buf[i+1]), NAME_MAX);
}

/**
	// ParseTuple converter for local file system sockets.
**/
int
local_from_object(PyObj ob, void *out)
{
	PyObj interface = NULL, port = NULL;
	local_addr_t *ref = out;

	if (Py_TYPE(ob) == (PyTypeObject *) endpointtype)
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
		if (!PyUnicode_FSConverter(ob, &interface))
			return(0);
	}
	else if (!PyArg_ParseTuple(ob, "O&|O&", PyUnicode_FSConverter, &interface, PyUnicode_FSConverter, &port))
	{
		Py_XDECREF(interface);
		Py_XDECREF(port);
		return(0);
	}

	if (port != NULL)
	{
		snprintf((char *) &(local_addr_field(ref)), sizeof(local_addr_field(ref)), "%s/%s",
			PyBytes_AS_STRING(interface), PyBytes_AS_STRING(port));
	}
	else
	{
		strncpy(local_addr_field(ref), PyBytes_AS_STRING(interface), sizeof(local_addr_field(ref)));
	}

	Py_DECREF(interface);

	/*
		// Allows receiver to detect that the struct was filled out.
	*/
	ref->sun_family = local_pf;
	local_init_length(ref);

	return(1);
}

/**
	// file being the domain (addressing); path_from_object is more appropriate.
**/
int
file_from_object(PyObj ob, void *out)
{
	file_addr_t *ref = out;
	PyObj path = NULL; /* bytes object */

	if (Py_TYPE(ob) == (PyTypeObject *) endpointtype)
	{
		Endpoint E = (Endpoint) ob;

		if (Endpoint_GetAddress(E)->ss_family != file_pf)
		{
			PyErr_SetString(PyExc_TypeError, "given endpoint is not a file endpoint");
			return(0);
		}

		memcpy(((char *) ref), ((char *) Endpoint_GetAddress(E)), Endpoint_GetLength(E));
		return(1);
	}
	else if (!PyUnicode_FSConverter(ob, &path))
		return(0);

	/*
		// PERF: unicode -> bytes -> char[]; could be improved.
	*/
	strncpy(ref->fa_path, PyBytes_AS_STRING(path), sizeof(ref->fa_path));
	Py_DECREF(path);

	ref->sa.sa_family = file_pf;

	return(1);
}

int
acquire_from_object(PyObj args, void *out)
{
	long fd;
	kport_t *kp = out;

	fd = PyLong_AsLong(args);
	if (PyErr_Occurred())
		return(0);

	*kp = fd;
	return(1);
}
