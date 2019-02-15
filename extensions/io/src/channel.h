/**
	# Includes Junction definition and accessors as Channels
	# and Junctions are co-dependent.
*/

/**
	# ChannelInterface methods for a given type
	# Attached to object system's type; not individual instances.
*/
struct ChannelInterface {
	io_op_t io[2];
	freight_t ti_freight;
	uint16_t ti_unit;
};

typedef struct ChannelInterface *ChannelInterface;

/**
	# ChannelInterface is used internally by Junctions to perform transfers.
	# tif.* are the interfaces used by Junction [I/O] cycles.
	# Whereas typ.* are the interfaces used by Python.
*/
typedef struct ChannelPyTypeObject {
	PyTypeObject typ;
	struct ChannelInterface *tif;
} ChannelPyTypeObject;

#define Type_GetInterface(TYP) (((ChannelPyTypeObject *) TYP)->tif)
#define Channel_GetInterface(OB) Type_GetInterface(Py_TYPE(OB))

/*
	# Channel type declarations.
*/
ChannelPyTypeObject
	ChannelType,
	JunctionType,
	OctetsType,
	SocketsType,
	PortsType;

/**
	# Start of all Channel structures.
*/
#define Channel_HEAD \
	PyObject_HEAD \
	Port port;          /* Port for Kernel communication */ \
	Junction junction;  /* transit controller */ \
	PyObj link;         /* User storage usually used by callbacks. */ \
	Channel prev, next; /* transit ring; all transit in traffic */ \
	Channel lltransfer; /* linked list pointer to next evented Channel */ \
	\
	transfer_window_t window; /* The area of the resource that was transferred. */ \
	union transit_choice choice; /* Junction or Memory Channel */ \
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
	/* Events emitted by Channels */
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
	ctl_requeue,      /* requeue the Channel */
	ctl_connect,      /* connect to kqueue */
};

/* Used to manage the current transfer window of a Channel */
typedef uint32_t transfer_window_t[2];
const static uint32_t transfer_window_limit = (0-1);

/**
	# Regular Channel or Junction (collection of active transits)
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

#define Channel_IAddress(id) (1 << ((TS_INTERNAL * teq_terminal_) + id))
#define Channel_XAddress(id) (1 << ((TS_EXTERNAL * teq_terminal_) + id))
#define Channel_CAddress(id) (1 << ((TS_CONTROLS * teq_terminal_) + id))

/*
	# Used to detect event availability
*/
#define Channel_Termination (Channel_IAddress(teq_terminate) | Channel_XAddress(teq_terminate))
#define Channel_Transferrence (Channel_IAddress(teq_transfer) | Channel_XAddress(teq_transfer))

/*
	# Delta flags are copied to the state and cleared. There is never a reason to unset
*/
#define Channel_DQualify(t, id) (Channel_GetDelta(t) |= Channel_IAddress(id))
#define Channel_DControl(t, id) (Channel_GetDelta(t) |= Channel_CAddress(id))
#define Channel_DQualified(t, id) (Channel_GetDelta(t) & Channel_IAddress(id))

#define Channel_IQualified(t, id) (Channel_State(t) & Channel_IAddress(id))
#define Channel_IQualify(t, id) (Channel_State(t) |= Channel_IAddress(id))
#define Channel_INQualify(t, id) (Channel_State(t) &= ~Channel_IAddress(id))
#define Channel_XQualified(t, id) (Channel_XAddress(id) & Channel_State(t))
#define Channel_XQualify(t, id) (Channel_State(t) |= Channel_XAddress(id))
#define Channel_XNQualify(t, id) (Channel_State(t) &= ~Channel_XAddress(id))

#define Channel_GetControl(t, id) (Channel_State(t) & Channel_CAddress(id))
#define Channel_SetControl(t, id) (Channel_State(t) |= Channel_CAddress(id))
#define Channel_NulControl(t, id) (Channel_State(t) &= ~Channel_CAddress(id))

#define Channel_Terminated(t) ((Channel_State(t) & Channel_Termination) == Channel_Termination)
#define Channel_Terminating(t) ( \
	(Channel_GetDelta(t) & Channel_IAddress(teq_terminate)) || \
	Channel_IQualified(t, teq_terminate) || \
	Channel_XQualified(t, teq_terminate) \
)

#define Channel_GetWindow(t) (t->window)

#define Channel_GetWindowStart(t) (Channel_GetWindow(t)[0])
#define Channel_GetWindowStop(t) (Channel_GetWindow(t)[1])
#define Channel_SetWindowStart(t, x) (Channel_GetWindowStart(t) = x)
#define Channel_SetWindowStop(t, y) (Channel_GetWindowStop(t) = y)
#define Channel_SetWindow(t, x, y) do { Channel_SetWindowStart(t, x); Channel_SetWindowStop(t, y); } while(0)
#define Channel_WindowLimit(t) (window_maximum - Channel_GetWindowStop(t))
#define Channel_WindowIsEmpty(t) (Channel_GetWindowStart(t) == Channel_GetWindowStop(t))
#define Channel_ClearWindow(t) do { Channel_GetWindowStart(t) = 0; Channel_GetWindowStop(t) = 0; } while(0)
#define Channel_NarrowWindow(t, count) (Channel_GetWindowStart(t) += count)
#define Channel_ExpandWindow(t, count) (Channel_GetWindowStop(t) += count)
#define Channel_CollapseWindow(t) (Channel_SetWindowStart(t, Channel_GetWindowStop(t)))

#define Channel_GetResource(t)           ((t)->choice.buffer.resource)
#define Channel_HasResource(t)           (Channel_GetResource(t) != NULL)
#define Channel_SetResource(t, r)        (Channel_GetResource(t) = r)
#define Channel_GetResourceView(t)       (&((t)->choice.buffer.view))
#define Channel_GetResourceBuffer(t)     ((t)->choice.buffer.view.buf)
#define Channel_GetResourceSize(t)       ((t)->choice.buffer.view.len)

/*
	# If internal termination is requested do it or
	# If external termination occurred, but only when there is no xtransfer.
*/
#define Channel_ShouldTerminate(t) (Channel_State(t) & Channel_Termination)
#define Channel_ShouldTransfer(t) ((Channel_State(t) & Channel_Transferrence) == Channel_Transferrence)
#define Channel_EventState(t) (Channel_ShouldTerminate(t) || Channel_ShouldTransfer(t))

/*
	# This condition does not refer to Terminating in order to allow
	# futile Channels to connect and emit their termination through
	# the event channel. (One code path to handle failures)
*/
#define Channel_ShouldXConnect(t) (Channel_GetControl(t, ctl_connect))

#define Channel_GetPort(t)           (t->port)
#define Channel_SetPort(t, p)        (Channel_GetPort(t) = p)
#define Channel_GetLink(t)           (t->link)
#define Channel_SetLink(t, l)        (Channel_GetLink(t) = l)

#define Port_Latch(p, polarity)  (polarity == p_output ? (p->latches >> 4) : (p->latches & (1|2|4|8)))
#define Channel_PortError(t)         (Channel_GetPort(t)->error != 0)
#define Channel_PortLatched(t)       Port_Latch(Channel_GetPort(t), Channel_Polarity(t))

#define Channel_GetKPoint(t)         (Channel_GetPort(t)->point)
#define Channel_SetKPoint(t, kpoint) (Channel_GetKPoint(t) = kpoint)
#define Channel_GetKError(t)         (Channel_GetPort(t)->error)
#define Channel_SetKError(t, kerror) (Channel_GetKError(t) = kerror)
#define Channel_GetKCall(t)          (Channel_GetPort(t)->cause)
#define Channel_SetKCall(t, kcall)   (Channel_GetKCall(t) = kcall)
#define Channel_GetKType(t)          (Channel_GetPort(t)->type)
#define Channel_SetKType(t, ktype)   (Channel_GetKType(t) = ktype)

#define Channel_GetJunction(t)            (t->junction)
#define Channel_GetJunctionPort(t)        (Channel_GetPort(Channel_GetJunction(t)))
#define Channel_SetJunction(t, J)         (Channel_GetJunction(t) = J)
#define Channel_Attached(t)               (t->prev != NULL)

#define Channel_HasEvent(t, TEV)          (Channel_GetEvents(t) & (1 << TEV))
#define Channel_NoteEvent(t, TEV)         (Channel_GetEvents(t) |= (1 << TEV))
#define Channel_RemoveEvent(t, TEV)       (Channel_GetEvents(t) &= ~(1 << TEV))
#define Channel_ClearEvents(t)            Channel_SetEvents(t, 0)

#define Channel_State(t)                  (t->state)
#define Channel_StateMerge(t, change)     (Channel_State(t) |= change)

#define Channel_GetEvents(t)              (t->events)
#define Channel_GetDelta(t)               (t->delta)
#define Channel_SetEvents(t, VAL)         (Channel_GetEvents(t)=VAL)
#define Channel_SetDelta(t, VAL)          (Channel_GetDelta(t)=VAL)

#define Channel_ClearDelta(t)             (Channel_GetDelta(t)=0)
#define Channel_InCycle(t)                (Channel_GetJunction(t)->lltransfer != NULL)

/*
	# Representation Polarity
*/
#define Channel_Polarity(t)      (Channel_GetControl(t, ctl_polarity) ? p_input : p_output)
#define Channel_Receives(t)      (Channel_GetControl(t, ctl_polarity))
#define Channel_Sends(t)         (!Channel_Receives(t))

/*
	# Junction accessors.
*/
#define Junction_HasTransfers(J)         (Channel_GetNextTransfer(J) != (Channel) J)
#define Junction_ShouldWait(J)           (Junction_HasTransfers(J) ? 0 : 1)

#define Junction_GetKEvents(J)           (J->choice.junction.kevents)
#define Junction_SetKEvents(J, K)        (Junction_GetKEvents(J) = K)
#define Junction_GetKEventSlot(J, slot)  (&(Junction_GetKEvents(J)[slot]))

/*
	# Junction uses its window for keeping track of the size of kevent array and the
	# relative position within that array for collection and modification.
*/
#define Junction_ResetWindow(t) do { Channel_GetWindowStart(t) = 0; } while(0)
#define Junction_NCollected Channel_GetWindowStart
#define Junction_SetNCollected Channel_SetWindowStart
#define Junction_NChanges(t) Channel_GetWindowStart(t)
#define Junction_MaxCollected(t) Channel_WindowIsEmpty(t)

#ifdef EVMECH_EPOLL
#define Junction_ConsumeKEventSlot(t)
#else
#define Junction_ConsumeKEventSlot(t) Channel_NarrowWindow(t, 1)
#endif

#define Junction_GetTransferCount(J) (J->choice.junction.ntransfers)
#define Junction_IncrementTransferCount(t) (++ Junction_GetTransferCount(t))
#define Junction_ResetTransferCount(t) (Junction_GetTransferCount(t) = 0)

/**
	# Base Channel structure.
*/
struct Channel {
	Channel_HEAD
};

/**
	# The Junction structure is equivalent to &Channel.
	# It is provided for distinction.
*/
struct Junction {
	Channel_HEAD
};
