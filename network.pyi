"""
# Socket interfaces for clients and servers.
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
	def address_family(self):
		"""
		# The system address family identifier.
		"""

	@property
	def address_type(self):
		"""
		# The type of addressing used to reference the endpoint.
		# One of `'ip6'`, `'ip4'`, `'local'`, or `None` if family is unknown.
		"""

	@property
	def address(self):
		"""
		# The address portion of the endpoint.
		"""

	@property
	def port(self):
		"""
		# The port of the endpoint as an &int.
		# &None if the endpoint has no port.
		"""

	@property
	def pair(self):
		"""
		# A newly constructed tuple consisting of the address and port attributes.
		"""

	@property
	def transport(self) -> int:
		"""
		# The system's transport protocol code that should be used to connect to the endpoint.
		"""

	@property
	def type(self) -> int:
		"""
		# The system's socket type code that should be used when allocating a socket.
		"""

def select_endpoints():
	"""
	# Resolve the &Endpoint set of the given host and service using (system/manual)`getaddrinfo`.
	"""

def select_interfaces():
	"""
	# Identify the interfaces to bind to for the service (system/manual)`getaddrinfo`.
	"""

def connect(endpoint):
	"""
	# Connect new sockets using the given endpoints.
	"""

def service(interface):
	"""
	# Create a listening socket using the given endpoint as the interface.
	"""

def bind(interface):
	"""
	# Create and bind a socket to an interface.
	"""
