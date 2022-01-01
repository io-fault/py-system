/**
	// Invariant maintenance for preprocessor lists.
*/
#define channeltype   ((PyObject *) &ChannelType)
#define arraytype     ((PyObject *) &ArrayType)
#define octetstype    ((PyObject *) &OctetsType)
#define datagramstype ((PyObject *) &DatagramsType)
#define porttype      ((PyObject *) &PortType)
#define endpointtype  ((PyObject *) &EndpointType)

#define CHANNEL_TYPES() \
	ID(Channel, void) \
	ID(Array, channels) \
	ID(Octets, octets) \
	ID(Datagrams, datagrams) \

#define PY_TYPES() \
	ID(Port, ...) \
	ID(DatagramArray, ...)

#define ID(x, y) typedef struct x * x;
	typedef struct Channel *Channel;
	typedef struct Datagrams *Datagrams;
	typedef struct Array *Array;
	PY_TYPES()
#undef ID

#define ID(x, y) extern PyTypeObject x##Type;
	PY_TYPES()
#undef ID

