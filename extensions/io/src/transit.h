/**
	# Includes Junction definition and accessors as Transits
	# and Junctions are co-dependent.
*/

/**
	# TransitInterface methods for a given type
	# Attached to object system's type; not individual instances.
*/
struct TransitInterface {
	io_op_t io[2];
	freight_t ti_freight;
	uint16_t ti_unit;
};

typedef struct TransitInterface *TransitInterface;

/**
	# TransitInterface is used internally by Junctions to perform transfers.
	# tif.* are the interfaces used by Junction [I/O] cycles.
	# Whereas typ.* are the interfaces used by Python.
*/
typedef struct TransitPyTypeObject {
	PyTypeObject typ;
	struct TransitInterface *tif;
} TransitPyTypeObject;

#define Type_GetInterface(TYP) (((TransitPyTypeObject *) TYP)->tif)
#define Transit_GetInterface(OB) Type_GetInterface(Py_TYPE(OB))

/*
	# Transit type declarations.
*/
TransitPyTypeObject
	TransitType,
	JunctionType,
	OctetsType,
	SocketsType,
	PortsType;

/**
	# Start of all Transit structures.
*/
#define Transit_HEAD \
	PyObject_HEAD \
	Port port;          /* Port for Kernel communication */ \
	Junction junction;  /* transit controller */ \
	PyObj link;         /* User storage usually used by callbacks. */ \
	Transit prev, next; /* transit ring; all transit in traffic */ \
	Transit lltransfer; /* linked list pointer to next evented Transit */ \
	\
	transfer_window_t window; /* The area of the resource that was transferred. */ \
	union transit_choice choice; /* Junction or Memory Transit */ \
	\
	/* state flags */ \
	uint8_t delta;  /* delta to apply to the state; new internal equals (GIL) */ \
	/* */ \
	uint8_t state;  /* bit map of equals (event qualifications) */ \
	/* */ \
	uint8_t events; /* bit map of produced events */

/**
	# Generalized events used by all transits
*/
typedef enum {
	/* No event */
	/* Events emitted by Transits */
	tev_terminate = 0,  /* transit was terminated */
	tev_transfer,       /* transfer occurred */
	tev_terminal_,
} transit_event_t;

/**
	# event qualifications

	# Pairs of qualifications must occur in order to produce an event.
	# (Kernel Qualification and Process Qualification)
*/
enum transit_equal {
	teq_terminate = 0, /* terminated noted/requested */
	teq_transfer,      /* transfer potential noted */
	teq_terminal_,
} transit_equal_t;

enum transit_control {
	ctl_polarity = 0, /* direction of the transit */
	ctl_force,        /* force a transfer to occur */
	ctl_requeue,      /* requeue the Transit */
	ctl_connect,      /* connect to kqueue */
};

/* Used to manage the current transfer window of a Transit */
typedef uint32_t transfer_window_t[2];
const static uint32_t transfer_window_limit = (0-1);

/**
	# Regular Transit or Junction (collection of active transits)
*/
union transit_choice {
	struct {
		PyObj resource;
		Py_buffer view;
	} buffer;

	struct {
		kevent_t *kevents;
		uint8_t will_wait;
		Py_ssize_t ntransits; /* volume */
		uint32_t ntransfers;

		#ifdef EVMECH_EPOLL
			int efd;
			int wfd;
			uint8_t haswrites;
		#endif
	} junction;
};

#define TS_INTERNAL 0
#define TS_EXTERNAL 1
#define TS_CONTROLS 2

#define Transit_IAddress(id) (1 << ((TS_INTERNAL * teq_terminal_) + id))
#define Transit_XAddress(id) (1 << ((TS_EXTERNAL * teq_terminal_) + id))
#define Transit_CAddress(id) (1 << ((TS_CONTROLS * teq_terminal_) + id))

/*
	# Used to detect event availability
*/
#define Transit_Termination (Transit_IAddress(teq_terminate) | Transit_XAddress(teq_terminate))
#define Transit_Transferrence (Transit_IAddress(teq_transfer) | Transit_XAddress(teq_transfer))

/*
	# Delta flags are copied to the state and cleared. There is never a reason to unset
*/
#define Transit_DQualify(t, id) (Transit_GetDelta(t) |= Transit_IAddress(id))
#define Transit_DControl(t, id) (Transit_GetDelta(t) |= Transit_CAddress(id))
#define Transit_DQualified(t, id) (Transit_GetDelta(t) & Transit_IAddress(id))

#define Transit_IQualified(t, id) (Transit_State(t) & Transit_IAddress(id))
#define Transit_IQualify(t, id) (Transit_State(t) |= Transit_IAddress(id))
#define Transit_INQualify(t, id) (Transit_State(t) &= ~Transit_IAddress(id))
#define Transit_XQualified(t, id) (Transit_XAddress(id) & Transit_State(t))
#define Transit_XQualify(t, id) (Transit_State(t) |= Transit_XAddress(id))
#define Transit_XNQualify(t, id) (Transit_State(t) &= ~Transit_XAddress(id))

#define Transit_GetControl(t, id) (Transit_State(t) & Transit_CAddress(id))
#define Transit_SetControl(t, id) (Transit_State(t) |= Transit_CAddress(id))
#define Transit_NulControl(t, id) (Transit_State(t) &= ~Transit_CAddress(id))

#define Transit_Terminated(t) ((Transit_State(t) & Transit_Termination) == Transit_Termination)
#define Transit_Terminating(t) ( \
	(Transit_GetDelta(t) & Transit_IAddress(teq_terminate)) || \
	Transit_IQualified(t, teq_terminate) || \
	Transit_XQualified(t, teq_terminate) \
)

#define Transit_GetWindow(t) (t->window)

#define Transit_GetWindowStart(t) (Transit_GetWindow(t)[0])
#define Transit_GetWindowStop(t) (Transit_GetWindow(t)[1])
#define Transit_SetWindowStart(t, x) (Transit_GetWindowStart(t) = x)
#define Transit_SetWindowStop(t, y) (Transit_GetWindowStop(t) = y)
#define Transit_SetWindow(t, x, y) do { Transit_SetWindowStart(t, x); Transit_SetWindowStop(t, y); } while(0)
#define Transit_WindowLimit(t) (window_maximum - Transit_GetWindowStop(t))
#define Transit_WindowIsEmpty(t) (Transit_GetWindowStart(t) == Transit_GetWindowStop(t))
#define Transit_ClearWindow(t) do { Transit_GetWindowStart(t) = 0; Transit_GetWindowStop(t) = 0; } while(0)
#define Transit_NarrowWindow(t, count) (Transit_GetWindowStart(t) += count)
#define Transit_ExpandWindow(t, count) (Transit_GetWindowStop(t) += count)
#define Transit_CollapseWindow(t) (Transit_SetWindowStart(t, Transit_GetWindowStop(t)))

#define Transit_GetResource(t)           ((t)->choice.buffer.resource)
#define Transit_HasResource(t)           (Transit_GetResource(t) != NULL)
#define Transit_SetResource(t, r)        (Transit_GetResource(t) = r)
#define Transit_GetResourceView(t)       (&((t)->choice.buffer.view))
#define Transit_GetResourceBuffer(t)     ((t)->choice.buffer.view.buf)
#define Transit_GetResourceSize(t)       ((t)->choice.buffer.view.len)

/*
	# If internal termination is requested do it or
	# If external termination occurred, but only when there is no xtransfer.
*/
#define Transit_ShouldTerminate(t) (Transit_State(t) & Transit_Termination)
#define Transit_ShouldTransfer(t) ((Transit_State(t) & Transit_Transferrence) == Transit_Transferrence)
#define Transit_EventState(t) (Transit_ShouldTerminate(t) || Transit_ShouldTransfer(t))

/*
	# This condition does not refer to Terminating in order to allow
	# futile Transits to connect and emit their termination through
	# the event channel. (One code path to handle failures)
*/
#define Transit_ShouldXConnect(t) (Transit_GetControl(t, ctl_connect))

#define Transit_GetPort(t)           (t->port)
#define Transit_SetPort(t, p)        (Transit_GetPort(t) = p)
#define Transit_GetLink(t)           (t->link)
#define Transit_SetLink(t, l)        (Transit_GetLink(t) = l)

#define Port_Latch(p, polarity)  (polarity == p_output ? (p->latches >> 4) : (p->latches & (1|2|4|8)))
#define Transit_PortError(t)         (Transit_GetPort(t)->error != 0)
#define Transit_PortLatched(t)       Port_Latch(Transit_GetPort(t), Transit_Polarity(t))

#define Transit_GetKPoint(t)         (Transit_GetPort(t)->point)
#define Transit_SetKPoint(t, kpoint) (Transit_GetKPoint(t) = kpoint)
#define Transit_GetKError(t)         (Transit_GetPort(t)->error)
#define Transit_SetKError(t, kerror) (Transit_GetKError(t) = kerror)
#define Transit_GetKCall(t)          (Transit_GetPort(t)->cause)
#define Transit_SetKCall(t, kcall)   (Transit_GetKCall(t) = kcall)
#define Transit_GetKType(t)          (Transit_GetPort(t)->type)
#define Transit_SetKType(t, ktype)   (Transit_GetKType(t) = ktype)

#define Transit_GetJunction(t)            (t->junction)
#define Transit_GetJunctionPort(t)        (Transit_GetPort(Transit_GetJunction(t)))
#define Transit_SetJunction(t, J)         (Transit_GetJunction(t) = J)
#define Transit_Attached(t)               (t->prev != NULL)

#define Transit_HasEvent(t, TEV)          (Transit_GetEvents(t) & (1 << TEV))
#define Transit_NoteEvent(t, TEV)         (Transit_GetEvents(t) |= (1 << TEV))
#define Transit_RemoveEvent(t, TEV)       (Transit_GetEvents(t) &= ~(1 << TEV))
#define Transit_ClearEvents(t)            Transit_SetEvents(t, 0)

#define Transit_State(t)                  (t->state)
#define Transit_StateMerge(t, change)     (Transit_State(t) |= change)

#define Transit_GetEvents(t)              (t->events)
#define Transit_GetDelta(t)               (t->delta)
#define Transit_SetEvents(t, VAL)         (Transit_GetEvents(t)=VAL)
#define Transit_SetDelta(t, VAL)          (Transit_GetDelta(t)=VAL)

#define Transit_ClearDelta(t)             (Transit_GetDelta(t)=0)
#define Transit_InCycle(t)                (Transit_GetJunction(t)->lltransfer != NULL)

/*
	# Representation Polarity
*/
#define Transit_Polarity(t)      (Transit_GetControl(t, ctl_polarity) ? p_input : p_output)
#define Transit_Receives(t)      (Transit_GetControl(t, ctl_polarity))
#define Transit_Sends(t)         (!Transit_Receives(t))

/*
	# Junction accessors.
*/
#define Junction_HasTransfers(J)         (Transit_GetNextTransfer(J) != (Transit) J)
#define Junction_ShouldWait(J)           (Junction_HasTransfers(J) ? 0 : 1)

#define Junction_GetKEvents(J)           (J->choice.junction.kevents)
#define Junction_SetKEvents(J, K)        (Junction_GetKEvents(J) = K)
#define Junction_GetKEventSlot(J, slot)  (&(Junction_GetKEvents(J)[slot]))

/*
	# Junction uses its window for keeping track of the size of kevent array and the
	# relative position within that array for collection and modification.
*/
#define Junction_ResetWindow(t) do { Transit_GetWindowStart(t) = 0; } while(0)
#define Junction_NCollected Transit_GetWindowStart
#define Junction_SetNCollected Transit_SetWindowStart
#define Junction_NChanges(t) Transit_GetWindowStart(t)
#define Junction_MaxCollected(t) Transit_WindowIsEmpty(t)

#ifdef EVMECH_EPOLL
#define Junction_ConsumeKEventSlot(t)
#else
#define Junction_ConsumeKEventSlot(t) Transit_NarrowWindow(t, 1)
#endif

#define Junction_GetTransferCount(J) (J->choice.junction.ntransfers)
#define Junction_IncrementTransferCount(t) (++ Junction_GetTransferCount(t))
#define Junction_ResetTransferCount(t) (Junction_GetTransferCount(t) = 0)

/**
	# Base Transit structure.
*/
struct Transit {
	Transit_HEAD
};

/**
	# The Junction structure is equivalent to &Transit.
	# It is provided for distinction.
*/
struct Junction {
	Transit_HEAD
};
