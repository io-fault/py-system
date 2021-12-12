/**
	// &.kernel.Link interfaces.
*/
#ifndef _SYSTEM_KERNEL_LINK_H_included_
#define _SYSTEM_KERNEL_LINK_H_included_

#define LinkFlag(LNF) (1 << lnf_##LNF)
#define Link_Get(LN, F) (LN->ln_flags & LinkFlag(F))
#define Link_Set(LN, F) (LN->ln_flags |= LinkFlag(F))
#define Link_Clear(LN, F) (LN->ln_flags &= ~LinkFlag(F))
#define Link_Closed(LN) (Link_Get(LN, closed))

#define LINK_FLAG_LIST() \
	LF(cancelled) \
	LF(dispatched) \
	LF(executing) \
	LF(cyclic)

/**
	// The set of control flags used to identify the Link's state.

	// /lnf_cancelled/
		// Whether or not the operation has been disconnected from events.
		// Resources associated with the event may still be open.
	// /lnf_dispatched/
		// Whether or not the operation has been scheduled with
		// &.kernel.Scheduler.dispatch.
	// /lnf_executing/
		// Whether or not the operation's task is being executed right now.
		// Primarily used as a safety to prevent unwanted recursion.
	// /lnf_cyclic/
		// Whether or not the task can be executed more than once.
*/
enum LinkFlags {
	lnf_void = -1,
	lnf_cancelled = 0,
	lnf_dispatched,
	lnf_executing,
	lnf_cyclic,
};

typedef struct Link *Link;
struct Link {
	PyObject_HEAD
	PyObj ln_context;
	Event ln_event;
	PyObj ln_task;

	uint32_t ln_flags;
};
extern PyTypeObject LinkType;

PyObj Link_Create(PyTypeObject *, PyObj, Event, PyObj);
#endif
