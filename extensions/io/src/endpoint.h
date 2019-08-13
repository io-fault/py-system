#ifndef _SYSTEM_IO_ENDPOINT_H_included_
#define _SYSTEM_IO_ENDPOINT_H_included_

/**
	// Endpoint - Hold an arbitrary socket address.

	// The data is zero-length array so the object is varsized to
	// compensate for file system sockets rather large address storage.
*/
struct Endpoint {
	PyObject_VAR_HEAD
	socklen_t len;
	if_addr_t data[0];
	/* Use itemsize to allocate an endpoint of an appropriate size */
};

#define Endpoint_GetAddress(E) ((any_addr_t *) &((*E).data))
#define Endpoint_GetLength(E) (E->len)

#ifndef SOCK_MAXADDRLEN
	/* Assume */
	#define SOCK_MAXADDRLEN 255
#endif

/**
	// These port structures should only be allocated on the stack.
*/
typedef enum {
	aport_kind_numeric2,
	aport_kind_filename,
	aport_kind_none,
} aport_kind_t;

struct aport_t {
	union {
		uint16_t numeric2; /* two byte number */
		char filename[NAME_MAX];
	} data;
	aport_kind_t kind;
};

typedef struct sockaddr_in ip4_addr_t;
#define ip4_pf PF_INET
#define ip4_name "ip4"
#define ip4_addr_field(x) ((x)->sin_addr)
#define ip4_port_field(x) ((x)->sin_port)

#if !defined(__linux__)
	#define ip4_init_length(x) ((x)->sin_len = sizeof(ip4_addr_t))
#else
	#define ip4_init_length(x) do { ; } while(0)
#endif

#define ip4_port_kind aport_kind_numeric2
#define ip4_str(dst, dstsize, addr) inet_ntop(ip4_pf, &(ip4_addr_field(addr)), dst, dstsize)
#define ip4_port(dst, dstsize, addr) do { dst->data.numeric2 = ntohs(ip4_port_field(addr)); } while(0)
#define ip4_parse(dst, src) inet_pton(ip4_pf, (char *) src, dst)
#define ip4_addr_size (32 / 8)
#define ip4_if_any (INADDR_ANY)
#define ip4_if_loopback (INADDR_LOOPBACK)
#define ip4_casted(NAME, src) ip4_addr_t * NAME = (ip4_addr_t *) src
#define ip4_clear {0,}

typedef struct sockaddr_in6 ip6_addr_t;
#define ip6_pf PF_INET6
#define ip6_name "ip6"

#if !defined(__linux__)
	#define ip6_init_length(x) ((x)->sin6_len = sizeof(ip6_addr_t))
#else
	#define ip6_init_length(x) do { ; } while(0)
#endif

#define ip6_addr_field(x) ((x)->sin6_addr)
#define ip6_port_field(x) ((x)->sin6_port)
#define ip6_port_kind aport_kind_numeric2
#define ip6_str(dst, dstsize, addr) inet_ntop(ip6_pf, &(ip6_addr_field(addr)), dst, dstsize)
#define ip6_port(dst, dstsize, addr) do { dst->data.numeric2 = ntohs(ip6_port_field(addr)); } while(0)
#define ip6_parse(dst, src) inet_pton(ip6_pf, (char *) src, dst)
#define ip6_addr_size (128 / 8)
#define ip6_if_any (in6addr_any)
#define ip6_if_loopback (in6addr_loopback)
#define ip6_casted(NAME, src) ip6_addr_t * NAME = (ip6_addr_t *) src
#define ip6_clear {0,}

typedef struct sockaddr_un local_addr_t;
#define local_pf PF_LOCAL
#define local_name "local"

#if !defined(__linux__)
	#define local_init_length(x) ((x)->sun_len = sizeof(local_addr_t))
#else
	#define local_init_length(x) do { ; } while(0)
#endif

#define local_addr_field(x) ((x)->sun_path) /* xxx: dirname */
#define local_port_field(x) ((x)->sun_path) /* xxx: basename */
#define local_port_kind aport_kind_filename
#define local_addr_size -1
#define local_if_any ("/")
#define local_if_loopback ("/")
#define local_casted(NAME, src) local_addr_t * NAME = (local_addr_t *) src
#define local_clear {0,}
void local_str(char *dst, size_t dstsize, local_addr_t *addr);
void local_port(struct aport_t *port, size_t dstsize, local_addr_t *addr);

/**
	// File addresses are not usually passed around this way, so don't
	// worry too much about the waste. If it ever gets particularly
	// desirable to do so, however, we can leverage the VarSize object
	// more and make the fa_path variably sized.
*/
typedef struct {
	struct sockaddr sa;
	char fa_path[PATH_MAX];
} file_addr_t;

#define file_pf                       (SOCK_MAXADDRLEN - 1)
#define file_name                     "file"
#define file_addr_field(x)            ((x)->fa_path)
#define file_clear                    {0,}
#define file_port_kind                aport_kind_none
#define file_str(dst, dstsize, addr)  strncpy(dst, addr->fa_path, dstsize)
#define file_port(dst, dstsize, addr) do { ; } while(0)
#define file_parse(dst, src)          strncpy(dst, src, PATH_MAX)
#define file_casted(NAME, src)        file_addr_t * NAME = (file_addr_t *) src

/**
	// pseudo domains
*/
#define acquire_pf (SOCK_MAXADDRLEN - 2)
#define clone_pf  (SOCK_MAXADDRLEN - 3)
#define spawn_pf  (SOCK_MAXADDRLEN - 4)

#define acquire_clear -1
#define spawn_clear 0

#define spawn_from_object(...) 1

typedef char spawn_addr_t;
typedef kport_t acquire_addr_t;
typedef kport_t clone_addr_t;

int ip4_from_object(PyObj ob, void *out);
int ip6_from_object(PyObj ob, void *out);
int local_from_object(PyObj ob, void *out);
int file_from_object(PyObj ob, void *out);
int acquire_from_object(PyObj args, void *out);

#define ADDRESSING() \
	A(ip4) \
	A(ip6) \
	A(local) \
	A(file)

#define DATAGRAMS() \
	A(ip4) \
	A(ip6)

#endif
