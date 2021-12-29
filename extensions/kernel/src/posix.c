/**
	// Minor abstractions for posix system calls.
	// Primarily supporting vectorized kport_t functions with limited retry attempts.
*/
#include <string.h>
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
#include <sys/param.h>

#include <fault/libc.h>
#include <fault/symbols.h>
#include <kcore.h>

#define POSIX_SYSCALL(errsig, rptr, call, ARGS) \
	RETRY_POSIX_SYSCALL: *(rptr) = call ARGS
#define RETRY_INIT(N) int _RETRY_STATE_##N = CONFIG_SYSCALL_RETRY
#define RETRY(N) do { \
	if (_RETRY_STATE_##N > 0) { \
		errno = 0; \
		--_RETRY_STATE_##N; \
		goto RETRY_##N; \
	} \
} while(0)

#ifndef CONFIG_SOCKET_TRANSFER_LIMIT
	#define CONFIG_SOCKET_TRANSFER_LIMIT 1
#endif

struct kp_message {
	struct iovec iov;
	struct msghdr mh;
	union {
		char buf[CMSG_SPACE(sizeof(kport_t) * CONFIG_SOCKET_TRANSFER_LIMIT)];
		struct cmsghdr _align;
	} ab;
};

STATIC(struct cmsghdr *)
_init_kp_message(struct kp_message *m, char *buf, int bufsize)
{
	int i;
	struct cmsghdr *cmsg;

	memset(m, 0, sizeof(struct kp_message));
	memset(&(m->mh), 0, sizeof(m->mh));

	m->mh.msg_name = NULL;
	m->mh.msg_namelen = 0;
	m->mh.msg_flags = 0;

	m->iov.iov_base = buf;
	m->iov.iov_len = bufsize;

	m->mh.msg_iov = &m->iov;
	m->mh.msg_iovlen = 1;

	m->mh.msg_control = &m->ab.buf;
	m->mh.msg_controllen = CMSG_LEN(sizeof(kport_t) * CONFIG_SOCKET_TRANSFER_LIMIT);

	/**
		// Configure message to duplicate the file descriptors
		// held in CMSG_DATA(cmsg).
	*/
	cmsg = CMSG_FIRSTHDR(&(m->mh));
	cmsg->cmsg_len = CMSG_LEN(sizeof(kport_t) * CONFIG_SOCKET_TRANSFER_LIMIT);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;

	for (i = 0; i < CONFIG_SOCKET_TRANSFER_LIMIT; ++i)
	{
		((int *) CMSG_DATA(cmsg))[i] = -1;
	}

	return(cmsg);
}

CONCEAL(int)
kp_chfd(kport_t kp, int op, int delta)
{
	int flags = fcntl(kp, F_GETFD, 0);

	if (flags == -1)
		return(-1);

	switch (op)
	{
		case -1:
			flags = flags & ~delta;
		break;

		case 0:
			flags = flags ^ delta;
		break;

		case 1:
			flags = flags | delta;
		break;
	}

	return(fcntl(kp, F_SETFD, flags));
}

CONCEAL(int)
kp_chfl(kport_t kp, int op, int delta)
{
	int flags = fcntl(kp, F_GETFL, 0);

	if (flags == -1)
		return(-1);

	switch (op)
	{
		case -1:
			flags = flags & ~delta;
		break;

		case 0:
			flags = flags ^ delta;
		break;

		case 1:
			flags = flags | delta;
		break;
	}

	return(fcntl(kp, F_SETFL, flags));
}

CONCEAL(int)
kp_receive(kport_t kp, kport_t buf[], uint32_t size)
{
	RETRY_INIT(POSIX_SYSCALL);
	int i, r;

	struct cmsghdr *cmsg;
	char _iovec_buf[8] = {0,};
	struct kp_message iom;

	for (i = 0; i < size; ++i)
	{
		buf[i] = -1;
		cmsg = _init_kp_message(&iom, _iovec_buf, 1);

		POSIX_SYSCALL(-1, &r, recvmsg, (kp, &(iom.mh), 0));

		if (r >= 0)
		{
			buf[i] = ((int *) CMSG_DATA(cmsg))[0];
		}
		else
		{
			switch (errno)
			{
				case EAGAIN:
					errno = 0;
					return(i);
				break;

				case EINTR:
					RETRY(POSIX_SYSCALL);
				default:
					return(-i);
				break;
			}
		}
	}

	return(i);
}

CONCEAL(int)
kp_transmit(kport_t kp, kport_t buf[], uint32_t size)
{
	RETRY_INIT(POSIX_SYSCALL);
	int i = 0, r;

	struct cmsghdr *cmsg;
	char _iovec_buf[8] = {'!',};
	struct kp_message iom;

	for (i = 0; i < size; ++i)
	{
		cmsg = _init_kp_message(&iom, _iovec_buf, 1);
		((int *) CMSG_DATA(cmsg))[0] = buf[i];

		POSIX_SYSCALL(-1, &r, sendmsg, (kp, &(iom.mh), 0));

		if (r < 0)
		{
			switch (errno)
			{
				case EAGAIN:
					errno = 0;
					return(i);
				break;

				case EINTR:
					RETRY(POSIX_SYSCALL);
				default:
					return(-i);
				break;
			}
		}
	}

	return(i);
}

/**
	// Regular socket accept from listening sockets.
*/
CONCEAL(int)
kp_accept(kport_t kp, kport_t buf[], uint32_t size)
{
	RETRY_INIT(POSIX_SYSCALL);
	int i, fd;

	for (i = 0; i < size; ++i)
	{
		fd = -1;

		POSIX_SYSCALL(-1, &fd, accept, (kp, NULL, 0));

		if (fd >= 0)
		{
			buf[i] = (kport_t) fd;
		}
		else
		{
			switch (errno)
			{
				case EAGAIN:
					errno = 0;
					return(i);
				break;

				case ECONNABORTED:
				/*
					// [ FreeBSD ]
					// Socket was closed while in the listening queue.
					// Treat as EINTR.
				*/

				case EINTR:
					RETRY(POSIX_SYSCALL);
				default:
					return(-i);
				break;
			}
		}
	}

	return(i);
}

/**
	// Allocate kport_t pairs capable of transmitting and receiving other kernel ports.
*/
CONCEAL(int)
kp_alloc_meta(kport_t kp, kport_t buf[], uint32_t size)
{
	RETRY_INIT(POSIX_SYSCALL);
	int st, i;

	for (i = 0; i < size/2; ++i)
	{
		int *b = &buf[i*2];

		st = -1;
		POSIX_SYSCALL(-1, &st, socketpair, (AF_LOCAL, SOCK_DGRAM, 0, b));
		if (st == -1)
		{
			switch (errno)
			{
				case EINTR:
					RETRY(POSIX_SYSCALL);
				default:
					return(-i);
				break;
			}
		}
	}

	return(i);
}

CONCEAL(int)
kp_alloc_bidirectional(kport_t kp, kport_t buf[], uint32_t size)
{
	RETRY_INIT(POSIX_SYSCALL);
	int st, i;

	for (i = 0; i < size/2; ++i)
	{
		int *b = &buf[i*2];

		st = -1;
		POSIX_SYSCALL(-1, &st, socketpair, (AF_LOCAL, SOCK_STREAM, 0, b));
		if (st == -1)
		{
			switch (errno)
			{
				case EINTR:
					RETRY(POSIX_SYSCALL);
				default:
					return(-i);
				break;
			}
		}
	}

	return(i);
}

CONCEAL(int)
kp_alloc_unidirectional(kport_t kp, kport_t buf[], uint32_t size)
{
	RETRY_INIT(POSIX_SYSCALL);
	int st, i;

	for (i = 0; i < size/2; ++i)
	{
		int *b = &buf[i*2];

		st = -1;
		POSIX_SYSCALL(-1, &st, pipe, (b));
		if (st == -1)
		{
			switch (errno)
			{
				case EINTR:
					RETRY(POSIX_SYSCALL);
				default:
					return(-i);
				break;
			}
		}
	}

	return(i);
}
