#ifndef _SYSTEM_NETWORK_ENDPOINT_H_included_
#define _SYSTEM_NETWORK_ENDPOINT_H_included_

typedef struct sockaddr_storage any_addr_t;
typedef struct sockaddr if_addr_t;
typedef struct sockaddr * if_addr_ref_t;

/**
	// Endpoint - Hold an arbitrary socket address.

	// The data is zero-length array so the object is varsized to
	// compensate for file system sockets rather large address storage.
*/
struct Endpoint {
	PyObject_VAR_HEAD
	socklen_t len;
	if_addr_t data[0];
};

typedef struct Endpoint *Endpoint;

#define Endpoint_GetAddress(E) ((any_addr_t *) &((*E).data))
#define Endpoint_GetLength(E) (E->len)

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

int ip4_from_object(PyObj ob, void *out);
int ip6_from_object(PyObj ob, void *out);
int local_from_object(PyObj ob, void *out);

#define ADDRESSING() \
	A(ip4) \
	A(ip6) \
	A(local)

extern PyTypeObject EndpointType;
#endif
