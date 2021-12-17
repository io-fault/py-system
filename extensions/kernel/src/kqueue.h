/**
	// kqueue specific data structures for supporting &.kernel.Link.
*/
#ifndef _SYSTEM_KERNEL_KQUEUE_H_included_
#define _SYSTEM_KERNEL_KQUEUE_H_included_

#include <sys/event.h>

#define AEV_TRANSMITS(KEV) (KEV->filter & EVFILT_WRITE)

#define AEV_CREATE EV_ADD
#define AEV_DELETE EV_DELETE
#define AEV_UPDATE EV_ADD

#define AEV_CYCLIC(KEV) !((KEV)->flags & EV_ONESHOT)
#define AEV_CYCLIC_ENABLE(KEV) ((KEV)->flags &= ~EV_ONESHOT)
#define AEV_CYCLIC_DISABLE(KEV) ((KEV)->flags |= EV_ONESHOT)

#define AEV_LINK(KEV) ((Link) (KEV)->udata)
#define AEV_KPORT(KEV) (Event_GetKPort(AEV_LINK(KEV)->ln_event))

#define KQ_FRAGMENT kport_t kq_root;
typedef struct kevent kevent_t;

#ifndef NOTE_MSECONDS
	#define NOTE_MSECONDS 0
#endif

#ifndef EVFILT_SYSCOUNT
	#define EVFILT_SYSCOUNT 32
#endif

#ifdef EVFILT_EXCEPT
	#define KQ_EXCEPT_FILTERS() \
		KFILTER(EVFILT_EXCEPT, kport_t)
#else
	#define EVFILT_EXCEPT (-16 - EVFILT_SYSCOUNT)
	#define KQ_EXCEPT_FILTERS() \
		KFILTER(EVFILT_EXCEPT, kport_t)
#endif

#ifdef EVFILT_PROCDESC
	#define KQ_PROCDESC_FILTERS() \
		KFILTER(EVFILT_PROCDESC, kport_t)
#else
	#define EVFILT_PROCDESC (-4 - EVFILT_SYSCOUNT)
	#define KQ_PROCDESC_FILTERS() \
		KFILTER(EVFILT_PROCDESC, kport_t)
#endif

#ifdef EVFILT_USER
	#define KQ_USER_FILTERS() \
		KFILTER(EVFILT_USER, kport_t)

	#define EV_USER_SETUP(KEV) do { \
		(KEV)->flags = EV_ADD|EV_CLEAR; \
		(KEV)->filter = EVFILT_USER; \
	} while(0)

	#define EV_USER_TRIGGER(KEV) do { \
		(KEV)->filter = EVFILT_USER; \
		(KEV)->fflags |= NOTE_TRIGGER; \
	} while(0)
#else
	#define KQ_USER_FILTERS()

	#define EV_USER_SETUP(KEV) do { \
		(KEV)->filter = EVFILT_TIMER; \
		(KEV)->flags = EV_ADD|EV_CLEAR|EV_DISABLE|EV_DISPATCH; \
		(KEV)->fflags = 0; \
		(KEV)->data = 0; \
	} while(0)

	#define EV_USER_TRIGGER(KEV) do { \
		(KEV)->filter = EVFILT_TIMER; \
		(KEV)->flags &= ~EV_DISABLE; \
		(KEV)->flags |= EV_ENABLE; \
	} while(0)
#endif

#ifdef EVFILT_VNODE
	#define KQ_FILESYSTEM_FILTERS() \
		KFILTER(EVFILT_VNODE, kport_t)
#else
	#define EVFILT_VNODE (-32 - EVFILT_SYSCOUNT)
	#define KQ_FILESYSTEM_FILTERS() \
		KFILTER(EVFILT_VNODE, kport_t)
#endif

#ifdef EVFILT_MACHPORT
	/* Presume the machport_t is available as well. */
	#define KQ_MACHPORT_FILTERS() \
		KFILTER(EVFILT_MACHPORT, machport_t)
#else
	#define EVFILT_MACHPORT (-128 - EVFILT_SYSCOUNT)
	#define KQ_MACHPORT_FILTERS() \
		KFILTER(EVFILT_MACHPORT, kport_t)
#endif

#define KQ_FILTER_LIST() \
	KFILTER(EVFILT_TIMER, intptr_t) \
	KFILTER(EVFILT_SIGNAL, int) \
	KFILTER(EVFILT_PROC, pid_t) \
	KFILTER(EVFILT_READ, kport_t) \
	KFILTER(EVFILT_WRITE, kport_t) \
	KQ_EXCEPT_FILTERS() \
	KQ_USER_FILTERS() \
	KQ_PROCDESC_FILTERS() \
	KQ_FILESYSTEM_FILTERS()

#ifndef EV_OOBAND
	#define EV_OOBAND 0
#endif

#define KQ_FLAG_LIST() \
	FLAG(EV_ADD) \
	FLAG(EV_DELETE) \
	FLAG(EV_ONESHOT) \
	FLAG(EV_ENABLE) \
	FLAG(EV_DISABLE) \
	FLAG(EV_DISPATCH) \
	\
	FLAG(EV_OOBAND) \
	\
	FLAG(EV_RECEIPT) \
	FLAG(EV_CLEAR) \
	FLAG(EV_EOF) \
	FLAG(EV_ERROR)

#define KFILTER_ERROR(K) (((K)->flags & EV_ERROR) ? ((int) (K)->data) : 0)

#ifndef NOTE_CLOSE_WRITE
	#define NOTE_CLOSE_WRITE 0
#endif

#ifndef NOTE_OPEN
	#define NOTE_OPEN 0
#endif

#ifndef NOTE_CLOSE
	#define NOTE_CLOSE 0
#endif

#define KQ_VNODE_EVENT_LIST() \
	VN_EVENT(NOTE_OPEN) \
	VN_EVENT(NOTE_CLOSE_WRITE) \
	VN_EVENT(NOTE_CLOSE) \
	VN_EVENT(NOTE_DELETE) \
	VN_EVENT(NOTE_WRITE) \
	VN_EVENT(NOTE_EXTEND) \
	VN_EVENT(NOTE_ATTRIB) \
	VN_EVENT(NOTE_LINK) \
	VN_EVENT(NOTE_RENAME) \
	VN_EVENT(NOTE_REVOKE) \
	VN_EVENT(NOTE_FUNLOCK)

#define EVENT_FS_VOID_FLAGS NOTE_RENAME|NOTE_DELETE|NOTE_REVOKE
#define EVENT_FS_DELTA_FLAGS NOTE_WRITE|NOTE_EXTEND
#define EVENT_FS_STATUS_FLAGS EVENT_FS_VOID_FLAGS | EVENT_FS_DELTA_FLAGS
#endif
