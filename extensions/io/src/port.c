/**
	// Abstraction for kernel ports (file descriptors).
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

#include "port.h"

#define errpf(...) fprintf(stderr, __VA_ARGS__)

/*
	// Manage retry state for limiting the number of times we'll accept EINTR.
*/
#define _RETRY_STATE _avail_retries
#define RETRY_STATE_INIT int _RETRY_STATE = CONFIG_SYSCALL_RETRY
#define LIMITED_RETRY() do { \
	if (_RETRY_STATE > 0) { \
		errno = 0; \
		--_RETRY_STATE; \
		goto RETRY_SYSCALL; \
	} \
} while(0);
#define UNLIMITED_RETRY() errno = 0; goto RETRY_SYSCALL;

/*
	// It seems unlikely that users will be sending dozens of file descriptors(Ports), so
	// keep the internal buffer size limited.

	// WARNING: Changing this *REQUIRES* code changes. Currently, the implementation does not
	// properly handle messages containing more than file descriptor.
*/
#ifndef CONFIG_TRAFFIC_PORTS_PER_CALL
	#define CONFIG_TRAFFIC_PORTS_PER_CALL 1
#endif

int
socket_receive_buffer(kport_t kp)
{
	int size = -1;
	socklen_t ssize = sizeof(size);
	getsockopt(kp, SOL_SOCKET, SO_RCVBUF, &size, &ssize);
	return(size);
}

int
socket_send_buffer(kport_t kp)
{
	int size = -1;
	socklen_t ssize = sizeof(size);
	getsockopt(kp, SOL_SOCKET, SO_SNDBUF, &size, &ssize);
	return(size);
}

int
port_open(Port p, char *path, int oflags)
{
	RETRY_STATE_INIT;
	kport_t kp;

	RETRY_SYSCALL:
	{
		ERRNO_RECEPTACLE(kp_invalid, &kp, open, path, oflags|CONFIG_OPEN_FLAGS, 0777);

		if (kp < 0)
		{
			switch (errno)
			{
				/*
					// Invalidate point to avoid future close().
				*/
				case AGAIN:
				case EINTR:
					LIMITED_RETRY()
				default:
					Port_NoteError(p, kc_open);
					return(1);
				break;
			}
		}
		p->point = kp;

		return(0);
	}
}

int
port_seek(Port p, off_t off, int whence)
{
	RETRY_STATE_INIT;
	int r;
	kport_t kp = p->point;

	RETRY_SYSCALL:
	{
		ERRNO_RECEPTACLE(-1, &r, lseek, kp, off, whence);

		if (r < 0)
		{
			switch (errno)
			{
				case AGAIN:
				case EINTR:
					LIMITED_RETRY()
				default:
					Port_NoteError(p, kc_lseek);
					return(1);
				break;
			}
		}

		return(0);
	}
}

static ktype_t
map_st_mode(mode_t mode)
{
	switch (mode & S_IFMT)
	{
		case S_IFSOCK:
			return(kt_socket);
		break;
		case S_IFIFO:
			return(kt_fifo);
		break;

		case S_IFCHR:
		case S_IFBLK:
			return(kt_device);
		case S_IFDIR:
		case S_IFLNK:
		case S_IFREG:
			return(kt_file);
		break;

		default:
			return(kt_pipe); /* assume pipe? */
		break;
	}

	return(kt_unknown);
}

int
port_identify_type(Port p)
{
	RETRY_STATE_INIT;
	int r;
	kport_t kp = p->point;

	struct stat st;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, fstat, kp, &st);

	if (r < 0)
	{
		switch (errno)
		{
			case EIO:
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				/*
					// Port will be closed by the usual mechanism.
				*/
				Port_NoteError(p, kc_fstat);
				return(1);
			break;
		}
	}

	p->type = map_st_mode(st.st_mode);

	#ifdef __MACH__
		/* kqueue is broken for on darwin for tty */
		if (isatty(kp))
		{
			errno = -1;
			Port_NoteError(p, kc_isatty);
			return(1);
		}
	#endif

	return(0);
}

int
port_getpeername(Port p, if_addr_ref_t addr, socklen_t *addrlen)
{
	RETRY_STATE_INIT;
	int r;
	kport_t kp = p->point;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, getpeername, kp, addr, addrlen);

	if (r < 0)
	{
		switch (errno)
		{
			/*
				// Invalidate point to avoid future close().
			*/
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				/*
					// Port will be closed by the usual mechanism.
				*/
				Port_NoteError(p, kc_getpeername);
				return(1);
			break;
		}
	}

	return(0);
}

int
port_getsockname(Port p, if_addr_ref_t addr, socklen_t *addrlen)
{
	RETRY_STATE_INIT;
	int r;
	kport_t kp = p->point;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, getsockname, kp, addr, addrlen);

	if (r < 0)
	{
		switch (errno)
		{
			/*
				// Invalidate point to avoid future close().
			*/
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				/*
					// Port will be closed by the usual mechanism.
				*/
				Port_NoteError(p, kc_getsockname);
				return(1);
			break;
		}
	}

	return(0);
}

int
port_set_socket_option(Port p, int option, int setting)
{
	RETRY_STATE_INIT;
	int r;
	socklen_t outsize = sizeof(setting);

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, setsockopt, p->point, SOL_SOCKET, option, &setting, sizeof(setting));

	if (r)
	{
		switch(errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
				/*
					// Just give up for set socket option.
					// Setting an error here may or may not be a good idea,
					// be optimistic about it not being critical.
				*/
			break;

			default:
				Port_NoteError(p, kc_setsockopt);
				return(1);
			break;
		}
	}

	return(0);
}

int
port_socket(Port p, int domain, int socktype, int protocol)
{
	RETRY_STATE_INIT;
	int r;
	kport_t kp;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(kp_invalid, &kp, socket, domain, socktype, protocol);

	if (kp < 0)
	{
		switch (errno)
		{
			/*
				// Invalidate point to avoid future close().
			*/
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_socket);
				return(1);
			break;
		}
	}

	p->point = kp;
	p->type = kt_socket;
	return(0);
}

int
port_socketpair(Port p1, Port p2)
{
	RETRY_STATE_INIT;
	int fdv[2] = {-1, -1};
	int r;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, socketpair, AF_LOCAL, SOCK_STREAM, 0, fdv);

	if (r)
	{
		switch (errno)
		{
			/*
				// Invalidate point to avoid future close().
			*/
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Ports_NoteError(p1, p2, kc_socketpair);
				return(1);
			break;
		}
	}

	p1->point = fdv[0];
	p2->point = fdv[1];
	p1->type = kt_socket;
	p2->type = kt_socket;

	return(0);
}

int
port_pipe(Port p1, Port p2)
{
	RETRY_STATE_INIT;
	int fdv[2] = {-1, -1};
	int r;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, pipe, fdv);

	if (r)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Ports_NoteError(p1, p2, kc_pipe);
				return(1);
			break;
		}
	}

	p1->point = fdv[0];
	p2->point = fdv[1];

	p1->type = kt_pipe;
	p2->type = kt_pipe;

	return(0);
}

int
port_dup(Port p, int fd)
{
	RETRY_STATE_INIT;
	kport_t kp;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(kp_invalid, &kp, dup, fd);

	if (kp < 0)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_dup);
				return(1);
			break;
		}
	}

	p->point = kp;
	return(0);
}

int
port_bind(Port p, if_addr_ref_t addr, socklen_t addrlen)
{
	RETRY_STATE_INIT;
	kport_t kp = p->point;
	int r;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, bind, kp, addr, addrlen);

	if (r)
	{
		switch (errno)
		{
			/*
				// Invalidate point to avoid future close().
			*/
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_bind);
				return(1);
			break;
		}
	}

	return(0);
}

#ifdef EVMECH_EPOLL
int
port_epoll_create(Port p)
{
	RETRY_STATE_INIT;
	int fd = -1;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &fd, epoll_create1, EPOLL_CLOEXEC);

	if (fd < 0)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, epoll_create);
				return(1);
			break;
		}
	}

	p->point = fd;
	return(0);
}

int
port_epoll_ctl(Port epp, int op, Port t, kevent_t *ke)
{
	RETRY_STATE_INIT;
	int r = -1;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, epoll_ctl, epp->point, op, t->point, ke);

	if (r != 0)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(t, epoll_ctl);
				return(1);
			break;
		}
	}

	return(0);
}

/* wait is always collecting, so no retry limit. */
int
port_epoll_wait(Port p, int *out, kevent_t *ke, int nevents, int timeout)
{
	RETRY_STATE_INIT;
	int r = -1;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, epoll_wait, p->point, ke, nevents, timeout);

	if (r >= 0)
	{
		*out = r;
	}
	else
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				*out = 0;
				Port_NoteError(p, epoll_wait);
				return(1);
			break;
		}
	}

	return(0);
}

#else

int
port_kqueue(Port p)
{
	RETRY_STATE_INIT;
	int fd = -1;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &fd, kqueue);

	if (fd < 0)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_kqueue);
				return(1);
			break;
		}
	}

	p->point = fd;
	return(0);
}

int
port_kevent(Port p, int retry, int *out, kevent_t *changes, int nchanges, kevent_t *events, int nevents, const struct timespec *timeout)
{
	RETRY_STATE_INIT;
	int r = -1;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, kevent, p->point, changes, nchanges, events, nevents, timeout);

	if (r >= 0)
	{
		/*
			// EV_ERROR is used in cases where kevent(2) fails after it already processed
			// some events. In these cases, the EV_ERROR flag is used to note the case.
		*/
		if (r > 0 && events[r-1].flags & EV_ERROR)
		{
			--r;
			*out = r;
			/*
				// XXX: Set error from EV_ERROR?
			*/
		}
		else
			*out = r;
	}
	else
	{
		*out = 0;

		switch (errno)
		{
			case AGAIN:
			case EINTR:
				/*
					// The caller can designate whether or not retry will occur.
				*/
				switch (retry)
				{
					case -1:
						UNLIMITED_RETRY();
					break;

					case 1:
						LIMITED_RETRY();
					break;
				}
			case ENOMEM:
				LIMITED_RETRY();

			default:
				Port_NoteError(p, kc_kevent);
				return(1);
			break;
		}
	}

	return(0);
}

#endif

int
port_connect(Port p, if_addr_ref_t addr, socklen_t addrlen)
{
	RETRY_STATE_INIT;
	kport_t kp = p->point;
	int r;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, connect, kp, addr, addrlen);

	if (r)
	{
		switch (errno)
		{
			/* Non-Errors */
			case EINPROGRESS:
			case EISCONN:
				/*
					// It is connecting or ... it is already connected?
				*/
				errno = 0;
			break;

			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_connect);
				return(1);
			break;
		}
	}

	return(0);
}

int
port_listen(Port p, int backlog)
{
	RETRY_STATE_INIT;
	kport_t kp = p->point;
	int r = -1;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, listen, kp, backlog);

	if (r)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_listen);
				return(1);
			break;
		}
	}

	return(0);
}

/**
	// These are technically optional, so clear errno and do not report failures.
	// XXX: socket option warnings may be useful.
**/
static void
init_socket(kport_t kp)
{
	static int true_indicator = 1;

	#if 0
		/*
			// Covered by fcntl().
		*/
		const static struct timeval tv = {0,0,};

		setsockopt(kp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(kp, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	#endif

	#ifdef TCP_NODELAY
		/*
			// By default. However, we should expose an interface to turn it back on.
		*/
		setsockopt(kp, IPPROTO_TCP, TCP_NODELAY, &true_indicator, sizeof(true_indicator));
	#endif

	#ifdef SO_KEEPALIVE
		setsockopt(kp, SOL_SOCKET, SO_KEEPALIVE, &true_indicator, sizeof(true_indicator));
	#endif

	/*
		// Inline out of band data. Apparently, some implementations can stall in the
		// presenece of unhandled OOB data.
	*/
	#ifdef SO_OOBINLINE
		setsockopt(kp, SOL_SOCKET, SO_OOBINLINE, &true_indicator, sizeof(true_indicator));
	#endif

	#ifndef F_SETNOSIGPIPE
		#ifdef SO_NOSIGPIPE
			setsockopt(kp, SOL_SOCKET, SO_NOSIGPIPE, &true_indicator, sizeof(true_indicator));
		#endif
	#endif
}

/**
	// setsockopt's and run init_kpoint
**/
static void
init_listening_socket(kport_t kp)
{
	#ifdef SO_ACCEPTFILTER
	{
		struct accept_filter_arg afa = {0,};

		strcpy(afa.af_name, "dataready");
		setsockopt(kp, SOL_SOCKET, SO_ACCEPTFILTER, &afa, sizeof(afa));
	}
	#else
		#ifdef TCP_DEFER_ACCEPT
		{
			int timeout = 1;
			setsockopt(kp, SOL_TCP, TCP_DEFER_ACCEPT, &timeout, sizeof(int));
		}
		#else
			#warning system does not support accept filters
		#endif
	#endif
}

int
port_init_socket(Port p)
{
	init_socket(p->point);
	errno = 0;
	return(0);
}

int
port_init_listening_socket(Port p)
{
	init_listening_socket(p->point);
	errno = 0;
	return(0);
}

#ifdef F_SETNOSIGPIPE
	int
	port_nosigpipe(Port p)
	{
		RETRY_STATE_INIT;
		kport_t kp = p->point;
		int r;

		RETRY_SYSCALL:
		ERRNO_RECEPTACLE(-1, &r, fcntl, kp, F_SETNOSIGPIPE, 1);

		if (r)
		{
			switch (errno)
			{
				case AGAIN:
				case EINTR:
					LIMITED_RETRY()
				default:
					Port_NoteError(p, kc_fcntl);
					return(1);
				break;
			}
		}

		return(0);
	}
#endif

int
port_noblocking(Port p)
{
	RETRY_STATE_INIT;
	kport_t kp = p->point;
	int r;

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, fcntl, kp, F_SETFL, O_NONBLOCK);

	if (r)
	{
		switch (errno)
		{
			case AGAIN:
			case EINTR:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_fcntl);
				return(1);
			break;
		}
	}

	return(0);
}

#if F_TRACE(latches)
	#define pbin(d,x) \
		fprintf(stderr, "%s%d%d%d%d   %d%d%d%d\n", d, \
			(x & 1<<7) ? 1 : 0, (x & 1<<6) ? 1 : 0, (x & 1<<5) ? 1 : 0, (x & 1<<4) ? 1 : 0, \
			(x & 1<<3) ? 1 : 0, (x & 1<<2) ? 1 : 0, (x & 1<<1) ? 1 : 0, (x & 1) ? 1 : 0 \
		);
#else
	#define pbin(d,x)
#endif

/**
	// Method to "close" a port.
	// Manages the reference counts and the effect of the close.
**/
void
port_unlatch(Port p, int8_t times)
{
	const uint8_t lo = (1|2|4|8);
	const uint8_t hi = lo << 4;
	unsigned int attempts = 0;

	/* Counts for sending channels are stored at 1 << 4, reading at 0 << 0 */
	if (times == 0)
	{
		/* Skip shutdown process. */
		#if F_TRACE(latches)
			errpf("NUKE LATCHES!\n");
		#endif
		p->latches = 0;
	}
	else
	{
		uint8_t current;
		int direction;

		pbin("BEF: ", p->latches)

		if (times < 0)
		{
			current = (p->latches >> 4);

			if (current == 0)
			{
				switch (p->cause)
				{
					case kc_leak:
					case kc_shatter:
						return;
					break;

					default:
						PyErr_WarnFormat(PyExc_ResourceWarning, 0,
							"port was already unlatched");
						#if F_TRACE(assert_latched)
							assert(1);
						#endif
						return;
					break;
				}
			}

			pbin(" -1: ", current)

			current -= (uint8_t) (-times);
			direction = SHUT_WR;
			p->latches = (current << 4) | (p->latches & lo);
		}
		else
		{
			current = (p->latches & lo);

			pbin(" +1: ", current);

			if (current == 0)
			{
				switch (p->cause)
				{
					case kc_leak:
					case kc_shatter:
						return;
					break;

					default:
						PyErr_WarnFormat(PyExc_ResourceWarning, 0,
							"port was already unlatched");
						#if F_TRACE(assert_latched)
							assert(1);
						#endif
						return;
					break;
				}
			}

			current -= (uint8_t) times;
			direction = SHUT_RD;
			p->latches = (current) | (p->latches & hi);
		}

		pbin("AFT: ", p->latches);

		if (current == 0)
		{
			switch (p->type)
			{
				case kt_socket:
					switch (p->freight)
					{
						case f_ports:
							/* Only transmitted over sockets. */
						case f_octets:
							shutdown(p->point, direction);
							#if F_TRACE(termination)
								errpf("SHUTDOWN: %s %d, %s\n",
									freight_identifier(p->freight),
									p->point, direction == SHUT_WR ? "writes" : "reads");
							#endif
							errno = 0;
							/* XXX: emit warning */
						break;
						default:
						break;
					}
				break;

				default:
					/* No action. */
				break;
			}
		}
	}

	if (p->latches != 0)
	{
		/*
			// Still latched or invalid.
		*/
		return;
	}
	else
	{
		RETRY_STATE_INIT;
		int r;

		RETRY_SYSCALL:
		ERRNO_RECEPTACLE(-1, &r, close, p->point);

		if (r < 0)
		{
			switch (errno)
			{
				case EINTR:
					LIMITED_RETRY()
				break;

				case EPIPE: /* XXX: Apparently, linux throws this. Assume EIO? */
				case EIO:
				default:
					/* XXX: Throw warning. */
					errno = 0;
				break;
			}
		}
	}

	#if F_TRACE(termination)
		errpf("CLOSE: %s %d\n", freight_identifier(p->freight), p->point);
	#endif
}

/* port i/o routines */
io_status_t
port_input_octets(Port p, uint32_t *consumed, char *buf, uint32_t size)
{
	RETRY_STATE_INIT;
	ssize_t r;
	size_t isize;

	continuation:
	isize = MIN(SIZE_T_MAX, ((size_t) size));

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, read, p->point, buf, isize);

	if (r >= 0)
	{
		*consumed += (uint32_t) r;
		size -= (uint32_t) r;

		/*
			// Stream until EWOULDBLOCK (EAGAIN).
		*/
		if (size > 0)
		{
			/*
				// *Unless* it's a file.
			*/
			if (p->type == kt_file)
			{
				/*
					// zero read. file?
				*/
				if (r < isize)
				{
					return(io_flow);
				}
			}

			/*
				// For some edge level trigger implementations, it's important
				// that we exhaust our buffer or trigger EWOULDBLOCK. If EWOULDBLOCK
				// is not triggered with epoll solutions, we won't see another event.
			*/
			if (r)
			{
				/*
					// Adjust buffer pointer.
				*/
				buf = buf + r;
				goto continuation;
			}
			else
			{
				/* A zero read where the type is not a file means EOF. */
				return(io_terminate);
			}
		}
	}
	else
	{
		switch (errno)
		{
			case EINTR:
				UNLIMITED_RETRY()
			break;

			case ENOTCONN:
			case AGAIN:
				/*
					// POSIX read(2)'s will throw EAGAIN on EOF of O_NONBLOCK fd's to REGULAR FILES.
					// Darwin doesn't do this and appears to ignore O_NONBLOCK on regular files.
				*/
				errno = 0;
				return(io_stop);
			break;

			case ENOBUFS:
			case ENOMEM:
				/* Possible transient error? */
				LIMITED_RETRY()
			case ETIMEDOUT:
				/* There may be some platform inconsistencies involving this errno... */
			default:
				/* fatal */
				Port_NoteError(p, kc_read);
				return(io_terminate);
			break;
		}
	}

	return(io_flow);
}

io_status_t
port_output_octets(Port p, uint32_t *consumed, char *buf, uint32_t size)
{
	RETRY_STATE_INIT;
	ssize_t r = 0;
	size_t isize = 0;

	continuation:

	/*
		// If the buffer is larger than our maximum write size, write the SIGNED_MAX,
		// but continue after subtraction in order to transfer to the entire buffer.
	*/
	isize = (size_t) MIN(SIZE_T_MAX, size);

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, write, p->point, buf, isize);

	if (r >= 0)
	{
		uint32_t xfer = (uint32_t) r;

		*consumed += xfer;
		size -= xfer;
		buf += (intptr_t) r;

		/*
			// For some edge level trigger implementations, it's important
			// that we exhaust our buffer or trigger EWOULDBLOCK. If EWOULDBLOCK
			// is not triggered with epoll solutions, we won't see another event.
		*/
		if (size > 0)
			goto continuation;
	}
	else
	{
		switch (errno)
		{
			case EINTR:
				UNLIMITED_RETRY()
			break;

			case ENOTCONN:
			case AGAIN:
				/*
					// POSIX read(2)'s will throw EAGAIN on EOF of O_NONBLOCK fd's to REGULAR FILES.
					// Darwin doesn't do this and appears to ignore O_NONBLOCK on regular files.
				*/
				errno = 0;
				return(io_stop);
			break;

			case ENOBUFS:
			case ENOMEM:
				/*
					// Possible transient error?
				*/
				LIMITED_RETRY()
			case ETIMEDOUT:
				/*
					// There may be some platform inconsistencies involving this errno...
				*/
			default:
				/* fatal */
				Port_NoteError(p, kc_write);
				return(io_terminate);
			break;
		}
	}

	return(io_flow);
}

/* Structures used by port_ read/write _ports *only*. */
struct _dm {
	struct iovec iov;
	struct msghdr mh;
	char mb[CMSG_SPACE(sizeof(int) * CONFIG_TRAFFIC_PORTS_PER_CALL)];
};

static struct cmsghdr *
_init_dm(struct _dm *dm, char *buf, int bufsize)
{
	int i;
	struct cmsghdr *cmsg;
	bzero(dm, sizeof(struct _dm));

	memset(&(dm->mh), 0, sizeof(dm->mh));
	dm->mh.msg_name = NULL;
	dm->mh.msg_namelen = 0;
	dm->mh.msg_flags = 0;

	dm->iov.iov_base = buf;
	dm->iov.iov_len = bufsize;

	dm->mh.msg_iov = &dm->iov;
	dm->mh.msg_iovlen = 1;

	dm->mh.msg_control = &(dm->mb);
	dm->mh.msg_controllen = CMSG_LEN(sizeof(int) * CONFIG_TRAFFIC_PORTS_PER_CALL);

	cmsg = CMSG_FIRSTHDR(&(dm->mh));

	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * CONFIG_TRAFFIC_PORTS_PER_CALL);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;

	for (i = 0; i < CONFIG_TRAFFIC_PORTS_PER_CALL; ++i)
	{
		((int *) CMSG_DATA(cmsg))[i] = -1;
	}

	return(cmsg);
}

io_status_t
port_input_ports(Port p, uint32_t *consumed, int *buf, uint32_t quantity)
{
	RETRY_STATE_INIT;
	uint32_t limit = quantity / sizeof(int);

	struct cmsghdr *cmsg;
	struct _dm iom;
	char _iovec_buf[8] = {0,};

	int i = 0, r;

	while (i < limit)
	{
		cmsg = _init_dm(&iom, _iovec_buf, 1);

		RETRY_SYSCALL:
		ERRNO_RECEPTACLE(-1, &r, recvmsg, p->point, &(iom.mh), 0);

		if (r >= 0)
		{
			/*
				// Extract payload from the message, mh.
			*/
			buf[i] = ((int *) CMSG_DATA(cmsg))[0];
			++i;
		}
		else
		{
			switch (errno)
			{
				case EINTR:
					UNLIMITED_RETRY() /* Looking to trigger AGAIN */
				break;

				case AGAIN:
					*consumed += i * sizeof(int);
					return(io_stop);
				break;

				case ENOMEM:
					LIMITED_RETRY();
				default:
					*consumed += i * sizeof(int);
					Port_NoteError(p, kc_recvmsg);
					return(io_terminate);
				break;
			}
		}
	}

	*consumed += i * sizeof(int);
	return(io_flow);
}

io_status_t
port_output_ports(Port p, uint32_t *consumed, int *buf, uint32_t quantity)
{
	RETRY_STATE_INIT;
	int limit = quantity / sizeof(int);

	struct cmsghdr *cmsg;
	char _iovec_buf[8] = {'!',};
	struct _dm iom;

	int i = 0, r;

	while (i < limit)
	{
		cmsg = _init_dm(&iom, _iovec_buf, 1);
		/*
			// Deposit payload into the message, mh.
		*/
		((int *) CMSG_DATA(cmsg))[0] = buf[i];

		RETRY_SYSCALL:
		ERRNO_RECEPTACLE(-1, &r, sendmsg, p->point, &(iom.mh), 0);

		if (r >= 0)
		{
			++i;
		}
		else
		{
			switch (errno)
			{
				case EINTR:
					UNLIMITED_RETRY() /* Looking to trigger AGAIN */
				break;

				case AGAIN:
					*consumed += i * sizeof(int);
					return(io_stop);
				break;

				case ENOMEM:
					LIMITED_RETRY()
				default:
					Port_NoteError(p, kc_sendmsg);
					*consumed += i * sizeof(int);
					return(io_terminate);
				break;
			}
		}
	}

	*consumed += i * sizeof(int);
	return(io_flow);
}

/**
	// Accept Loop
**/
io_status_t
port_input_sockets(Port p, uint32_t *consumed, int *buf, uint32_t size)
{
	RETRY_STATE_INIT;
	uint32_t limit = (size / sizeof(int));
	int i, fd;

	/*
		// Accept loop. Read as many as possible.

		// On ignoring the address parameter:
		// Accepts sockets subsequently passed to a new Octets instance can interrogate
		// their endpoint() directly if necessary.
	*/

	for (i = 0; i < limit; ++i)
	{
		fd = -1;

		RETRY_SYSCALL:
		ERRNO_RECEPTACLE(-1, &fd, accept, p->point, NULL, 0);

		if (fd >= 0)
		{
			buf[i] = fd;
			*consumed += (1 * sizeof(int));
		}
		else
		{
			switch (errno)
			{
				case AGAIN:
					errno = 0;
					return(io_stop);
				break;

				case ECONNABORTED:
					/*
						// Accepted socket was closed before accept() returned. Retry.
					*/
				case EINTR:
					UNLIMITED_RETRY()
				break;

				case ENOMEM:
					LIMITED_RETRY()
				default:
					/*
						// Error out.
					*/
					Port_NoteError(p, kc_accept);
					return(io_terminate);
				break;
			}
		}
	}

	return(io_flow);
}

io_status_t
port_input_datagrams(Port p, uint32_t *consumed, struct Datagram *dg, uint32_t quantity)
{
	RETRY_STATE_INIT;
	int r;
	struct Datagram *current = dg;
	socklen_t len;

	continuation:
	if (!DatagramIsValid(current, quantity))
	{
		/*
			// Truncate the invalid remainder.
		*/
		*consumed = (((intptr_t) current) - ((intptr_t) dg)) + quantity;
		return(io_flow);
	}
	len = DatagramGetAddressLength(current);

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, recvfrom, p->point, DatagramGetData(current), DatagramGetSpace(current), 0, DatagramGetAddress(current), &len);

	if (r >= 0)
	{
		struct Datagram *next;

		DatagramSetSize(current, r);
		next = DatagramNext(current);

		quantity -= (((intptr_t) next) - ((intptr_t) current));
		current = next;

		if (quantity > 0)
			goto continuation;
	}
	else
	{
		switch (errno)
		{
			case EINTR:
				UNLIMITED_RETRY()
			case AGAIN:
				*consumed = ((intptr_t ) current) - ((intptr_t ) dg);
				return(io_stop);
			break;

			case ENOMEM:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_recvfrom);
				*consumed = ((intptr_t ) current) - ((intptr_t ) dg);
				return(io_terminate);
			break;
		}
	}

	*consumed = (((intptr_t) current) - ((intptr_t) dg)) + quantity;
	return(io_flow);
}

io_status_t
port_output_datagrams(Port p, uint32_t *consumed, struct Datagram *dg, uint32_t quantity)
{
	RETRY_STATE_INIT;
	ssize_t r;
	struct Datagram *current = dg;

	continuation:
	if (!DatagramIsValid(current, quantity))
	{
		/*
			// Truncate the invalid remainder.
		*/
		*consumed = (((intptr_t) current) - ((intptr_t) dg)) + quantity;
		return(io_flow);
	}

	RETRY_SYSCALL:
	ERRNO_RECEPTACLE(-1, &r, sendto, p->point, DatagramGetData(current), DatagramGetSpace(current), 0, DatagramGetAddress(current), DatagramGetAddressLength(current));

	if (r >= 0)
	{
		struct Datagram *next;

		next = DatagramNext(current);

		quantity -= ((intptr_t) next) - ((intptr_t) current);
		current = next;

		if (quantity > 0)
			goto continuation;
	}
	else
	{
		switch (errno)
		{
			case EINTR:
				UNLIMITED_RETRY()
			case AGAIN:
				*consumed = ((intptr_t ) current) - ((intptr_t ) dg);
				return(io_stop);
			break;

			case ENOMEM:
				LIMITED_RETRY()
			default:
				Port_NoteError(p, kc_sendto);
				*consumed = ((intptr_t ) current) - ((intptr_t ) dg);
				return(io_terminate);
			break;
		}
	}

	*consumed = (((intptr_t) current) - ((intptr_t) dg)) + quantity;
	return(io_flow);
}

/**
	// Sequence of Port operations (portS)

	// These functions respresent a series of operations of that
	// need to be successful in order for the port to function properly.
	// If one step errors out, jump to the exit. The port_* interface will
	// note the error on the port.
**/
int
ports_listen(Port p, int domain, if_addr_ref_t interface, size_t interface_size)
{
	/*
		// Specifically for binding listen sockets.
	*/
	if (port_socket(p, domain, SOCK_STREAM, 0))
		goto exit;

	if (port_noblocking(p))
		goto exit;

	if (port_bind(p, interface, interface_size))
		goto exit;

	if (port_listen(p, DEFAULT_BACKLOG))
		goto exit;

	return(0);

	exit:
	{
		return(1);
	}
}

int
ports_bind(Port p, int domain, int socktype, int proto, if_addr_ref_t endpoint, size_t endpoint_size)
{
	/*
		// Primarily for datagram sockets.
	*/
	if (port_socket(p, domain, socktype, proto))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p))
			goto exit;
	#endif

	if (port_noblocking(p))
		goto exit;

	if (port_bind(p, endpoint, endpoint_size))
		goto exit;

	port_init_socket(p);

	return(0);

	exit:
	return(1);
}

int
ports_bind_connect(Port p, int domain, int socktype, int proto, if_addr_ref_t endpoint, size_t endpoint_size, if_addr_ref_t interface, size_t ifsize)
{
	if (port_socket(p, domain, socktype, proto))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p))
			goto exit;
	#endif

	if (port_noblocking(p))
		goto exit;

	if (port_bind(p, interface, ifsize))
		goto exit;

	if (port_connect(p, endpoint, endpoint_size))
		goto exit;

	port_init_socket(p);

	return(0);

	exit:
	return(1);
}

int
ports_connect(Port p,
	int domain, int socktype, int proto,
	if_addr_ref_t endpoint, size_t endpoint_size)
{
	if (port_socket(p, domain, socktype, proto))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p))
			goto exit;
	#endif

	if (port_noblocking(p))
		goto exit;

	if (port_connect(p, endpoint, endpoint_size))
		goto exit;

	port_init_socket(p);

	return(0);

	exit:
	return(1);
}

int
ports_pipe(Port p[])
{
	if (port_pipe(p[0], p[1]))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p[1]))
			goto exit;
	#endif

	return((port_noblocking(p[0]) << 1) | port_noblocking(p[1]));

	exit:
	return(1);
}

int
ports_socketpair(Port p[])
{
	if (port_socketpair(p[0], p[1]))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p[0]) | port_nosigpipe(p[1]))
			goto exit;
	#endif

	return((port_noblocking(p[0]) << 1) | port_noblocking(p[1]));

	exit:
	return(1);
}

int
ports_identify_socket(Port p)
{
	if (port_identify_type(p))
		goto exit;

	if (p->type != kt_socket)
	{
		errno = EBADF;
		Port_NoteError(p, kc_identify);
		goto exit;
	}

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p))
			goto exit;
	#endif

	if (port_noblocking(p))
		goto exit;

	port_init_socket(p);

	return(0);

	exit:
	return(1);
}

int
ports_identify_input(Port p)
{
	char buf[1] = {0,};
	uint32_t consumed = 0;

	if (port_identify_type(p))
		goto exit;

	/*
		// Reads don't cause EPIPE
	*/

	if (port_noblocking(p))
		goto exit;

	if (port_input_octets(p, &consumed, buf, 0) == io_terminate)
		goto exit;

	return(0);

	exit:
	return(1);
}

int
ports_identify_output(Port p)
{
	char buf[1] = {0,};
	uint32_t consumed = 0;

	if (port_identify_type(p))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p))
			goto exit;
	#endif

	if (port_noblocking(p))
		goto exit;

	if (port_output_octets(p, &consumed, buf, 0) == io_terminate)
		goto exit;

	return(0);

	exit:
	return(1);
}

int
ports_clone_input(Port p, kport_t reader)
{
	if (port_dup(p, reader))
		goto exit;

	if (ports_identify_input(p))
		goto exit;

	if (port_noblocking(p))
		goto exit;

	return(0);

	exit:
	return(1);
}

int
ports_clone_output(Port p, kport_t writer)
{
	if (port_dup(p, writer))
		goto exit;

	if (ports_identify_output(p))
		goto exit;

	#ifdef F_SETNOSIGPIPE
		if (port_nosigpipe(p))
			goto exit;
	#endif

	if (port_noblocking(p))
		goto exit;

	return(0);

	exit:
	{
		return(1);
	}
}

int
ports_clone_pair(Port p[], kport_t reader, kport_t writer)
{
	return(ports_clone_input(p[0], reader) << 1 | ports_clone_output(p[1], writer));
}

int
ports_open(Port p, char *path, int oflags)
{
	if (port_open(p, path, oflags))
		goto exit;

	if (port_noblocking(p))
		goto exit;

	if (port_identify_type(p))
		goto exit;

	switch (p->type)
	{
		case kt_file:
		{
			;
		}
		break;
		default:
			/* note not regular file? */
		break;
	}

	return(0);

	exit:
	return(1);
}
