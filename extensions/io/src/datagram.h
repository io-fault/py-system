/**
	// Datagram data. The inner structure of Datagrams' resource allocations.
*/

/**
	// A single datagram transmitted by &.kernel.Datagrams channels.
*/
struct Datagram {
	uint32_t gramspace, gramsize;
	socklen_t addrlen;
	if_addr_ref_t addr[0];
	char gram[0];
};

#define DatagramHeaderSize (sizeof(struct Datagram))

#define DatagramGetAddress(DG) ((if_addr_ref_t) (((char *) DG) + DatagramHeaderSize))
#define DatagramGetData(DG) (((char *) DG) + DatagramHeaderSize + DG->addrlen)

#define DatagramGetSize(DG) DG->gramsize
#define DatagramSetSize(DG, SIZE) (DatagramGetSize(DG) = SIZE)
#define DatagramGetSpace(DG) DG->gramspace
#define DatagramSetSpace(DG, SIZE) (DatagramGetSpace(DG) = SIZE)
#define DatagramGetAddressLength(DG) DG->addrlen
#define DatagramCalculateUnit(dgspace, addrlen) (dgspace + addrlen + DatagramHeaderSize)

/**
	// Total space of the datagram including header data.
*/
#define DatagramGetArea(DG) \
	(DatagramGetSpace(DG) + DatagramGetAddressLength(DG) + DatagramHeaderSize)

/**
	// First check that there is enough space for a regular Datagram, then make sure that the
	// space and address length don't exceed the available SIZE: The size must be greater than
	// the size minus the header, space, and address length.
*/
#define DatagramIsValid(DG, SIZE) ( \
	(SIZE >= DatagramHeaderSize) && (SIZE >= DatagramGetArea(DG)) && \
	(((SIZE - DatagramGetAddressLength(DG)) - DatagramHeaderSize) - DatagramGetSpace(DG)) < SIZE \
)

#define DatagramNext(DG) \
	((struct Datagram *) (DatagramGetData(DG) + (DatagramGetSpace(DG))))
