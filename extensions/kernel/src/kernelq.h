/**
	// kernelq interfaces
	// Abstraction for kqueue and epoll.
*/
#ifndef _SYSTEM_KERNEL_KERNELQ_H_included_
#define _SYSTEM_KERNEL_KERNELQ_H_included_

kport_t fs_event_open(const char *, enum EventType);

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

typedef struct KernelQueue *KernelQueue;
struct KernelQueue {
	PyObj kq_references; /* dictionary */
	PyObj kq_cancellations; /* list of cancelled links */

	/**
		// kport_t kq_root; // epoll and kqueue
	*/
	KQ_FRAGMENT

	unsigned int kq_event_position, kq_event_count;
	kevent_t kq_array[CONFIG_STATIC_KEVENTS];
};

int kernelq_reference_update(KernelQueue, Link, PyObj *);
int kernelq_reference_delete(KernelQueue, Event);
int kernelq_cyclic_event(KernelQueue, int, Link, kevent_t *);

int kernelq_identify(kevent_t *, event_t *);
int kernelq_delta(KernelQueue, int, kport_t, kevent_t *);
int kernelq_interrupt_accept(KernelQueue);
int kernelq_schedule(KernelQueue, int, Link);
PyObj kernelq_cancel(KernelQueue, Event);

int kernelq_initialize(KernelQueue);
int kernelq_receive(KernelQueue, long, long);
int kernelq_interrupt(KernelQueue);
int kernelq_transition(KernelQueue, TaskQueue);
int kernelq_close(KernelQueue);

void kernelq_clear(KernelQueue);
int kernelq_traverse(KernelQueue, visitproc, void *);
#endif
