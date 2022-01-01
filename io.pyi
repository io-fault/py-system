"""
# I/O Array implementation for transferring data into and out of the process.
"""
from . import network

class Port(object)
	"""
	# Data structure holding the kport_t used by the Channel.
	# Maintains the error state and references.
	"""

	@property
	def transmit(self) -> int:
		"""
		# The file descriptor supporting transmission.
		"""

	@property
	def receive(self) -> int:
		"""
		# The file descriptor supporting reception.
		"""

	@property
	def error_code(self) -> int:
		"""
		# The error code (errno) associated with the port.
		"""

	@property
	def call(self) -> str:
		"""
		# The system call that returned the noted error.
		"""

	def shatter(self):
		"""
		# Close the file descriptors without performing any socket shutdown procedures.
		"""

	def leak(self):
		"""
		# Prohibit termination from closing or shutting down file descriptor.

		# For sockets, this affects both read and write.
		"""

	def raised(self):
		"""
		# Raise the &OSError corresponding to the noted error.
		"""

	def exception(self):
		"""
		# Return the &OSError corresponding to the noted error.
		"""

class Channel(object):
	"""
	# A unidirectional communication endpoint.
	"""

	@property
	def array(self) -> Array:
		"""
		# The &Array instance that acquired the &Channel.
		# &None if the channel has not been associated.
		"""

	@property
	def port(self) -> Port:
		"""
		# The &Port shared with another &Channel.
		"""

	@property
	def polarity(self) -> int:
		"""
		# The direction that transfers flow.

		# /`-1`/
			# Transmits: data flows out of the process via this channel.
		# /`+1`/
			# Receives: data flows into the process via this channel.
		"""

	@property
	def link(self):
		"""
		# The user defined resource for connecting transfers to their corresponding tasks.
		"""

	@property
	def terminated(self) -> bool:
		"""
		# Whether the channel has been terminated or not.
		"""

	@property
	def exhausted(self) -> bool:
		"""
		# Whether the channel's resource is no longer present.
		"""

	@property
	def resource(self):
		"""
		# The resource currently being transferred. Configured by &acquire.
		"""

	def terminate(self):
		"""
		# Issue termination instructions potentially closing the associated Port.
		"""

	def endpoint(self) -> network.Endpoint:
		"""
		# Constructed network endpoint identifying the target of transmissions.
		"""

	def acquire(self, resource):
		"""
		# Acquire a transmission resource that will facilitate sends or receives.
		"""

	def transfer(self):
		"""
		# The slice of the acquired resource that was transferred within the
		# open I/O transaction.
		"""

	def sizeof_transfer(self) -> int:
		"""
		# The size of the current transfer, zero if no transfer window is open.
		"""

class Array(Channel):
	"""
	# A set of &Channel instances being managed for I/O events.
	"""

	def void(self):
		"""
		# Destroy all kernel resources held indirectly by the Array and suppress termination
		# events that would normally be sent.
		"""

	def wait(self):
		"""
		# Wait for I/O events to occur and return a context manager providing
		# a window for collecting tranfer events.
		"""

class Octets(Channel):
	"""
	# Channel implementation facilitating the transfer of stream data.
	"""
