#include <sys/epoll.h>
#include <sys/types.h>

#include <sys/signalfd.h>
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

/**
	// Level triggered for receive, edge for writes.
*/
#define EPOLL_FLAGS EPOLLRDHUP

#define AEV_CREATE EPOLL_CTL_ADD
#define AEV_DELETE EPOLL_CTL_DEL
#define AEV_UPDATE EPOLL_CTL_MOD

/**
	// Support for handling cyclic dispatch requirements.
*/
#define AEV_CYCLIC(KEV) !((KEV)->events & EPOLLONESHOT)
#define AEV_CYCLIC_ENABLE(KEV) ((KEV)->events &= ~EPOLLONESHOT)
#define AEV_CYCLIC_DISABLE(KEV) ((KEV)->events |= EPOLLONESHOT)

#define AEV_LINK(KEV) ((Link) (KEV)->data.ptr)

#define KQ_FRAGMENT kport_t kq_root, kq_eventfd_interrupt;
typedef struct epoll_event kevent_t;
