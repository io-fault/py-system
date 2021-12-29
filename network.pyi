"""
# System types and calls for communicating with hosts identified by &Endpoint instances.

# While &.kernel has some socket interfaces, the high-level system calls here are exclusively
# functions of &Endpoint instances.
"""

class Endpoint(object):
	"""
	# The system's representation of a network communication endpoint.
	"""

	def replace(self, **kw):
		"""
		# Create a new endpoint with the given fields overwritten.
		"""

	@property
	def address(self) -> str|int:
		"""
		# The address that identifies the host being contacted.
		"""

	@property
	def port(self) -> str|int:
		"""
		# The port of the host that identifies the service to be contacted.
		"""

	@property
	def type(self) -> str:
		"""
		# The type of addressing used to identify the host.

		# One of `'ip6'`, `'ip4'`, `'local'`, or `None` if family is unknown.
		"""

	@property
	def pair(self):
		"""
		# Tuple consisting of the address and port attributes.
		"""

	@property
	def pf_code(self) -> int:
		"""
		# The integer identifier used by the system to identify the protocol family.
		"""

	@property
	def tp_code(self) -> int:
		"""
		# The integer identifier used by the system to identify the transport protocol.
		"""

	@property
	def st_code(self) -> int:
		"""
		# The integer identifier used by the system to identify the socket's type.
		"""

def select_endpoints(host:str, service:str|int):
	"""
	# Resolve the &Endpoint set of the given host and service using (system/manual)`getaddrinfo`.
	"""

def select_interfaces(host:str, service:str|int):
	"""
	# Identify the interfaces to bind to for the service (system/manual)`getaddrinfo`.
	"""

def connect(endpoint:Endpoint, interface:Endpoint=None):
	"""
	# Connect new sockets using the given endpoints.
	"""

def service(interface:Endpoint):
	"""
	# Create a listening socket using the given endpoint as the interface.
	"""

def bind(interface:Endpoint):
	"""
	# Create and bind a socket to an interface.
	"""
