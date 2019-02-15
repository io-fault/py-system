[ Primary Types ]

/&module.c#Octets/
	Transfers "octets", bytes of data using read and write.
/&module.c#Datagrams/
	Transfers datagrams to hosts designated by the datagram.
/&module.c#Sockets/
	Transfers "sockets", file descriptors (listen/accept) for accepting
	connection.
/&module.c#Ports/
	Transfers file descriptors (libancillary for python).
	However, it is not functioning.
/&module.c#Junction/
	Transfers Transits that have events ready for processing (I/O).

[ Terminology ]

/`J`/
	(capital j) refers to the Junction instance.
/`t`/
	(lowercase t) refers to an arbitrary Transit instance.
/`o`/
	(lowercase o) refers to an arbitrary Octets instance.
/`S`/
	(capital S) refers to an arbitrary Sockets instance.
/`p`/
	(lowercase p) refers to an arbitrary Port instance.
/`P`/
	(capital P) refers to an arbitrary Ports (plural) instance.
/`D`/
	(capital d) refers to an arbitrary Datagrams instance.
/`w`/
	(lowercase w) refers to an arbitrary Wolves instance.

/`equals`/
	Refers to Event QUALificationS These are state flags that must be present in order for a
	particular event to occur.
/`polarity`/
	refers to the direction of flow wrt process perspective
/`qualify`/
	refers to setting a flag on 'equals'
/`nqualify`/
	refers to clearing a flag on 'equals'
/`qualified`/
	refers to testing for the presence of a flag on 'equals'
/`xqual`/
	refers to external event qualifications (kernel)
/`iqual`/
	refers to internal event qualifications (process)
/`ctl_*`/
	are control flags for signalling events listens, requeue, forced transfers.
