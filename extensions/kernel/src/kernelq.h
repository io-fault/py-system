/**
	// kernelq interfaces
*/
#ifndef _SYSTEM_KERNEL_KERNELQ_H_included_
#define _SYSTEM_KERNEL_KERNELQ_H_included_

#ifndef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
#elif CONFIG_STATIC_KEVENTS < 8
	#undef CONFIG_STATIC_KEVENTS
	#define CONFIG_STATIC_KEVENTS 16
	#warning nope.
#endif

#ifndef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
#elif CONFIG_SYSCALL_RETRY < 8
	#undef CONFIG_SYSCALL_RETRY
	#define CONFIG_SYSCALL_RETRY 16
	#warning nope.
#endif

/**
	// Limited retry state.
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

typedef struct kevent kevent_t; /* kernel event description */
typedef int kport_t; /* file descriptor */
typedef int kerror_t; /* kerror error identifier (errno) */

typedef struct KernelQueue *KernelQueue;
struct KernelQueue {
	/* Storage for objects referenced by the kernel. */
	PyObj kq_references;
	PyObj kq_cancellations;

	kport_t kq_root;
	unsigned int kq_event_position, kq_event_count;
	kevent_t kq_array[CONFIG_STATIC_KEVENTS];
};

int kernelq_initialize(KernelQueue);
int kernelq_interrupt(KernelQueue);
int kernelq_close(KernelQueue);
void kernelq_clear(KernelQueue);
int kernelq_traverse(KernelQueue, PyObj, visitproc, void *);
PyObj kernelq_process_watch(KernelQueue, pid_t, void *);
PyObj kernelq_process_ignore(KernelQueue, pid_t, void *);
int kernelq_recur(KernelQueue, int, unsigned long, PyObj);
int kernelq_defer(KernelQueue, int, unsigned long, PyObj);

int kernelq_enqueue(KernelQueue, long, long);
PyObj kernelq_transition(KernelQueue);
#endif
