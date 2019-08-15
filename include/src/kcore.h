#ifndef _SYSTEM_KCORE_H_included_
#define _SYSTEM_KCORE_H_included_

/**
	// Type used to represent file descriptors.
*/
typedef int kport_t;
#define kp_invalid (-1)

/**
	// Type used to explicitly designate an errno code.
*/
typedef int kerror_t;

/**
	// Enumeration type.
*/
typedef int8_t kcall_t;

/**
	// Preprocessor macro listing of syscalls used by extensions.
	// Some are not actual libc calls.

	// Generally intended for internal use.
*/
#define KCALLS(...) \
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
	// Enum for &kcall_t.
*/
enum kcall {
	#define KC(x) kc_##x ,
		KCALLS()
	#undef KC
};

#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
#endif

/**
	// Handle possibility of EWOULDBLOCK being distinct from EAGAIN.
*/
#ifdef EWOULDBLOCK
	#if EAGAIN == EWOULDBLOCK
		#define case_AGAIN() case EAGAIN:
	#else
		#define case_AGAIN() case EAGAIN: case EWOULDBLOCK:
	#endif
#else
	#define case_AGAIN() case EAGAIN:
#endif

#endif
/* kcore.h */
