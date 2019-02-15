/*
 * Handy for macros working with lowercase names.
 */
#define transittype   ((PyObject *) &TransitType)
#define junctiontype  ((PyObject *) &JunctionType)
#define octetstype    ((PyObject *) &OctetsType)
#define socketstype   ((PyObject *) &SocketsType)
#define portstype     ((PyObject *) &PortsType)
#define datagramstype ((PyObject *) &DatagramsType)
#define porttype      ((PyObject *) &PortType)
#define endpointtype  ((PyObject *) &EndpointType)

#define TRANSIT_TYPES() \
	ID(Transit, void) \
	ID(Junction, transits) \
	ID(Octets, octets) \
	ID(Sockets, sockets) \
	ID(Ports, ports) \
	ID(Datagrams, datagrams) \

#define PY_TYPES() \
	ID(Port, ...) \
	ID(Endpoint, ...) \
	ID(DatagramArray, ...)

#define ID(x, y) typedef struct x * x;
	typedef struct Transit *Transit;
	typedef struct Datagrams *Datagrams;
	typedef struct Junction *Junction;
	PY_TYPES()
#undef ID

#define ID(x, y) PyTypeObject x##Type;
	PY_TYPES()
#undef ID

