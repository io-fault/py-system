#ifndef _SYSTEM_IO_PORT_H_included_
#define _SYSTEM_IO_PORT_H_included_

/**
	# Type used to represent file descriptors.
*/
typedef int kpoint_t;

/**
	# Type used to represent recoreded errno's.
*/
typedef int kerror_t;

/**
	# Datatype for representing
*/
typedef uint8_t kcall_t; /* the contrived kcall type */

/**
	# Preprocessor macro listing of syscalls used.
*/
#define KCALLS() \
	KC(pyalloc) KC(none) \
	KC(leak) KC(shatter) KC(eof) KC(void) KC(identify) \
	KC(kqueue) \
	KC(kevent) \
	\
	KC(read) KC(write) \
	KC(send) KC(recv) \
	\
	KC(recvfrom) KC(sendto) \
	KC(sendmsg) KC(recvmsg) \
	\
	KC(setsockopt) KC(getsockopt) \
	KC(fcntl) KC(fstat) KC(isatty) \
	KC(getsockname) KC(getpeername) \
	\
	KC(socketpair) KC(pipe) \
	\
	KC(open) KC(close) \
	\
	KC(lseek) \
	KC(socket) KC(bind) \
	KC(connect) KC(shutdown) \
	KC(listen) KC(accept) \
	KC(dup) KC(dup2) \
	/* linux specific */ \
	KC(epoll_create) \
	KC(epoll_ctl) \
	KC(epoll_wait) \
	KC(eventfd) \
	KC(INVALID)

/**
	# Enum for &kcall_t.
*/
enum kcall {
	#define KC(x) kc_##x ,
		KCALLS()
	#undef KC
};

/**
	# Beware. Abstractions for the socket interfaces ahead.

	# There were too many hand written case statements in this code, so here's a ton
	# of macros helping to abstract the socket interfaces so we can write horribly confusing
	# template macros that will cause rage at the author of this source.

	# AF_LOCAL:
	# We generalize local addressing by declaring the socket file directory as the interface
	# and the file name as the port. This allows us to handle local addresses in a way that
	# is reasonably consistent with internet addresses.
*/
typedef struct sockaddr_storage any_addr_t;
typedef struct sockaddr if_addr_t;
typedef struct sockaddr * if_addr_ref_t;

#include "datagram.h"

#define kp_invalid (-1)
#define DEFAULT_BACKLOG 64

#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 64
#elif CONFIG_SYSCALL_RETRY < 8
	#undef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
	#warning CONFIG_SYSCALL_RETRY set to 16.
#endif

#ifndef CONFIG_OPEN_FLAGS
	#define CONFIG_OPEN_FLAGS 0
#endif

#ifdef __linux__
	/* For Enabling Accept Delay */
	#include <netinet/tcp.h>

	/* linux doesnt do sin_len so void out the operation */
	/* admittedly, sin_len is silly, but why introduce incompat... */
	#define SETADDRLEN(TARGET, VALUE)
#else
	/* some systems want/need this field to be filled out */
	#define SETADDRLEN(TARGET, VALUE) TARGET = VALUE
#endif

/**
	# These are the same on OS X and probably other platforms.
	# Traffic treats them the same, so be sure to handle platforms
	# that have distinct values for EAGAIN/EWOULDBLOCK.
*/
#ifdef EWOULDBLOCK
	#if EAGAIN == EWOULDBLOCK
		#define AGAIN EAGAIN
	#else
		#define AGAIN EAGAIN: case EWOULDBLOCK
	#endif
#else
	#define AGAIN EAGAIN
#endif

#if defined(__linux__)
	/* epoll */
	#include <sys/epoll.h>
	#include <sys/eventfd.h>
	#define EVMECH_EPOLL
	typedef struct epoll_event kevent_t;
#else
	/* kqueue */
	#define EVMECH_KQUEUE

	#include <sys/event.h>
	typedef struct kevent kevent_t; /* kernel event description */
	#define KQ_FILTERS() \
		FILTER(EVFILT_USER) \
		FILTER(EVFILT_READ) \
		FILTER(EVFILT_WRITE) \
		FILTER(EVFILT_PROC) \
		FILTER(EVFILT_VNODE) \
		FILTER(EVFILT_AIO) \
		FILTER(EVFILT_SIGNAL) \
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
#endif

#define SIGNED_MAX(T) (const unsigned T) ((~((unsigned T) 0)) >> 1)
#define SIGNED_MIN(T) (const signed T) ~(SIGNED_MAX(T))

/**
	# Freight types conveyed by a Channel.
*/
typedef enum freight {
	f_void = 0,   /* 'v' */
	f_channels,   /* 't' */
	f_octets,     /* 'o' */
	f_datagrams,  /* 'G' */
	f_sockets,    /* 'S' */
	f_ports,      /* 'P' */
	f_wolves,     /* 'w' */
} freight_t;

/**
	# local identifiers for the particular types of file descriptors.
	# Primarily used in cases where the channel supports multiple kinds.
*/
typedef enum ktype {
	kt_unknown = 0,
	kt_socket,     /* local or otherwise */
	kt_pipe,       /* anonymous pipe */
	kt_fifo,       /* named pipe */
	kt_device,     /* "regular" file */
	kt_tty,        /* "regular" file */
	kt_file,       /* "regular" file */
	kt_kqueue,     /* a kqueue file descriptor */
	kt_bad,        /* bad file descriptor */
} ktype_t;

/**
	# Channel status codes used to signal the reaction to the success or failure of a system call.
*/
typedef enum io_status {
	io_stop,
	io_flow,
	io_terminate
} io_status_t;

/**
	# Kernel Port (file descriptor) structure representation.

	# This structure exists because of sockets.
	# It was the best way to allow the Channels to synchronize their release
	# of the file descriptor resource. Arguably, a bitmap could be used as well,
	# but this easier and provides utility beyond that necessity. (port specific introspection)
*/
struct Port {
	OBJSYS_HEAD()

	kpoint_t point;
	kerror_t error;

	uint8_t cause;   /* kcall_t */
	uint8_t type;    /* ktype_t */
	uint8_t freight; /* freight_t */
	uint8_t latches; /* latches */

	OBJSYS_TAIL()
};

typedef struct Port * Port;

/**
	# I/O operation pointer used by ChannelType class instances to
	# specify the read and write operation.
*/
typedef io_status_t (*io_op_t)(Port port, uint32_t *consumed, void *resource, uint32_t quantity);

/**
	# It is critical that latches are zeroed the moment
	# an EBADF is seen. It possible that the process
	# will allocate a descriptor with the same port.id (fileno).
	# If latches is non-zero, a Channel will run unlatch in the future
	# performing some state change to a shattered port.
*/
#define Port_SetError(P, ERROR, SCAUSE) do { \
	P->cause = SCAUSE; \
	P->error = ERROR; \
	if (P->error == EBADF) P->latches = 0; \
} while (0)

#define Port_NoteError(p, scause) do { \
	Port_SetError(p, errno, scause); \
	errno = 0; \
} while (0)

/**
	# Currently only used by socketpair()
*/
#define Ports_NoteError(p1, p2, scause) do { \
	p2->cause = p1->cause = scause; \
	p2->error = p1->error = errno; \
	errno = 0; \
	if (p1->error == EBADF) \
		p2->latches = p1->latches = 0; \
} while (0)

char freight_charcode(freight_t);
const char * freight_identifier(freight_t);
char * ktype_string(ktype_t);

int port_open(Port p, char *path, int oflags);
int port_seek(Port p, off_t off, int whence);
int port_identify_type(Port p);

int ports_identify_socket(Port p);
int ports_identify_input(Port p);
int ports_identify_output(Port p);
int ports_clone_input(Port p, kpoint_t reader);
int ports_clone_output(Port p, kpoint_t writer);
int ports_clone_pair(Port p[], kpoint_t reader, kpoint_t writer);
int ports_open(Port p, char *path, int oflags);
int ports_pipe(Port p[]);
int ports_socketpair(Port p[]);
int ports_listen(Port p, int domain, if_addr_ref_t interface, size_t interface_size);
int ports_bind(Port p, int domain, int socktype, int proto, if_addr_ref_t endpoint, size_t endpoint_size);
int ports_bind_connect(Port p, int domain, int socktype, int proto, if_addr_ref_t endpoint, size_t endpoint_size, if_addr_ref_t interface, size_t ifsize);
int ports_connect(Port p, int domain, int socktype, int proto, if_addr_ref_t endpoint, size_t endpoint_size);

int port_getpeername(Port p, if_addr_ref_t addr, socklen_t *addrlen);
int port_getsockname(Port p, if_addr_ref_t addr, socklen_t *addrlen);
int port_set_socket_option(Port p, int option, int setting);
int port_socket(Port p, int domain, int socktype, int protocol);
int port_socketpair(Port p1, Port p2);
int port_pipe(Port p1, Port p2);
int port_dup(Port p, int fd);
int port_bind(Port p, if_addr_ref_t addr, socklen_t addrlen);

#ifdef EVMECH_EPOLL
	int port_epoll_create(Port p);
	int port_epoll_ctl(Port epp, int op, Port t, kevent_t *ke);
	int port_epoll_wait(Port p, int *out, kevent_t *ke, int nevents, int timeout);
#else
	int port_kqueue(Port p);
	int port_kevent(Port p, int retry, int *out, kevent_t *changes, int nchanges, kevent_t *events, int nevents, const struct timespec *timeout);
#endif

int port_connect(Port p, if_addr_ref_t addr, socklen_t addrlen);
int port_listen(Port p, int backlog);

int port_init_socket(Port p);
int port_init_listening_socket(Port p);

#ifdef F_SETNOSIGPIPE
	int port_nosigpipe(Port p);
#endif

int port_noblocking(Port p);

void port_unlatch(Port p, int8_t times);

io_status_t port_input_octets(Port p, uint32_t *consumed, char *buf, uint32_t size);
io_status_t port_output_octets(Port p, uint32_t *consumed, char *buf, uint32_t size);

io_status_t port_input_ports(Port p, uint32_t *consumed, int *buf, uint32_t quantity);
io_status_t port_output_ports(Port p, uint32_t *consumed, int *buf, uint32_t quantity);

io_status_t port_input_sockets(Port p, uint32_t *consumed, int *buf, uint32_t size);

io_status_t port_input_datagrams(Port p, uint32_t *consumed, struct Datagram *dg, uint32_t quantity);
io_status_t port_output_datagrams(Port p, uint32_t *consumed, struct Datagram *dg, uint32_t quantity);
#endif
