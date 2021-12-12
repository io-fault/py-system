"""
# System kernel and Python runtime interfaces.
"""
import collections.abc

def close(fds:collections.abc.Iterable[int]) -> None:
	"""
	# Close the file descriptors produced by the iterable.
	"""

def retained(fds:collections.abc.Iterable[int]) -> None:
	"""
	# Preserve the given file descriptors across process image substitutions(exec).
	"""

def released(fds:collections.abc.Iterable[int]) -> None:
	"""
	# Configure the file descriptors to be released when the process is substituted(exec).
	"""

def hostname() -> str:
	"""
	# Retrieve the hostname of the system using gethostname(2).
	"""

def machine() -> (str, str):
	"""
	# Retrieve the system name and instruction architecture of the runtime using uname(2).
	"""

def clockticks() -> int:
	"""
	# Retrieve the (system/manual)`sysconf` value of (id)`SC_CLK_TCK`.
	"""

def signalexit(signo:int) -> None:
	"""
	# Configure an atexit call to force the process to exit via a signal.
	"""

def initialize():
	"""
	# Initialize the after fork callbacks.
	# Called once by &.process. Do not use directly.
	"""

class Event(object):
	"""
	# Event data union and scheduling identity.
	"""

	@property
	def type(self) -> str:
		"""
		# A string identifying the type of event.
		"""

	@property
	def port(self):
		"""
		# A file descriptor identifying the source of events.
		"""

	@property
	def correlation(self):
		"""
		# For I/O, a file descriptor identifying the cited opposition of &port.
		# May be `-1`.
		"""

	@property
	def input(self) -> int:
		"""
		# Position independent access to the file descriptor identifying the receive side.

		# For (id)`io-transmit` events, this is the &correlation.
		# For (id)`io-receive` events, this is the &port.
		"""

	@property
	def output(self) -> int:
		"""
		# Position independent access to the file descriptor identifying the transmit side.

		# For (id)`io-transmit` events, this is the &port.
		# For (id)`io-receive` events, this is the &correlation.
		"""

	@classmethod
	def meta_actuate(Class, reference=None):
		"""
		# Construct an event that will occur upon the actuation of the &Scheduler
		# instance that the operation was dispatched on.
		"""

	@classmethod
	def meta_terminate(Class, reference=None):
		"""
		# Construct an event that will occur upon the termination of the &Scheduler
		# instance that the operation was dispatched on.
		"""

	@classmethod
	def never(Class, reference):
		"""
		# Construct an event that is known to never occur.
		"""

	@classmethod
	def time(Class, nanosecond, microsecond=0, millisecond=0, second=0):
		"""
		# Construct an event that will occur after the specified duration occurs.
		"""

	@classmethod
	def process_exit(Class, pid, procfd=-1):
		"""
		# Construct an event identifier that will occur when process identified
		# by &pid exits. If &procfd is given on supporting systems, it will
		# be used instead of allocating one or using the &pid directly.
		"""

	@classmethod
	def process_signal(Class, signo, sigfd=-1):
		"""
		# Construct an event identifier that will occur when the host process
		# receives the cited signal. If &sigfd is given on supporting systems, it
		# will be used instead of allocating one or using the &signo directly.
		"""

	@classmethod
	def fs_status(Class, path:str, fileno=-1):
		"""
		# Construct an event identifier that will occur when the status of the
		# file identified by &path, or &fileno, has changed.
		"""

	@classmethod
	def fs_delta(Class, path:str, fileno=-1):
		"""
		# Construct an event identifier that will occur when the file identified
		# by &path, or &fileno, is modified.
		"""

	@classmethod
	def fs_void(Class, path:str, fileno=-1):
		"""
		# Construct an event identifier that will occur when the file identified
		# by &path, or &fileno, is deleted.
		"""

	@classmethod
	def io_transmit(Class, port:int, correlation:int=-1):
		"""
		# Event identifier that occurs when writes are possible
		# on the given &port file descriptor.
		"""

	@classmethod
	def io_receive(Class, port:int, correlation:int=-1):
		"""
		# Event identifier that occurs when reads are possible
		# on the given &port file descriptor.
		"""

class Link(object):
	"""
	# An operation record to be dispatched by a &Scheduler instance.
	"""

	def __new__(Class, event:Event, task:collections.abc.Callable, /, context=None):
		pass

	@property
	def context(self) -> object:
		"""
		# User storage slot intended to hold a reference to the object that created the &Link.
		"""

	@property
	def event(self) -> Event:
		"""
		# The original event that the &Link was planned using.
		"""

	@property
	def task(self) -> collections.abc.Callable:
		"""
		# The object that will be executed when the event occurs.
		"""

	@property
	def cyclic(self) -> bool:
		"""
		# Whether or not the operation was dispatched with the expectation of
		# multiple occurrences.
		"""

	@property
	def cancelled(self) -> bool:
		"""
		# Whether or not the operation has been removed from the scheduled set.
		"""

	@property
	def dispatched(self) -> bool:
		"""
		# Whether or not the operation has been scheduled.
		"""

	@property
	def executing(self) -> bool:
		"""
		# Whether or not the operation is currently executing.
		"""

class Scheduler(object):
	"""
	# Kernel event management and task queue.
	"""

	def operations(self) -> collections.abc.Sequence[Link]:
		"""
		# Construct a snapshot of the dispatched operations.
		"""

	def void(self):
		"""
		# Immediately destroy the resources held by the scheduler.
		"""

	@property
	def closed(self) -> bool:
		"""
		# Whether the scheduler has been closed.
		"""

	def close(self):
		"""
		# Close the scheduler causing events from the kernel to no longer be retrieved.
		"""

	def wait(self) -> int:
		"""
		# Wait for events from the source(operating system) and enqueue their associated
		# tasks for execution.

		# [ Returns ]
		# The count of events that were received from the kernel.
		"""

	def interrupt(self) -> bool:
		"""
		# Interrupt a blocking &wait call.
		# Nothing if there is no concurrent &wait call.

		# [ Returns ]
		# Whether or not a &wait call was interrupted.
		# &None if the &Scheduler instance is closed.
		"""

	def execute(self, trap) -> int:
		"""
		# Execute enqueued tasks, FIFO.
		# Concurrent calls with &wait are prohibited.

		# [ Parameters ]
		# /trap/
			# Callable performed when a task raises an exception.
			# &trap will be given the task and a normalized exception instance.

			# If &None, exceptions will be discarded.

		# [ Returns ]
		# The count of tasks that were executed.
		"""

	def dispatch(self, operation:Link):
		"""
		# Execute the operation when its associated event occurs.

		# [ Parameters ]
		# /operation/
			# The &Link instance to attach to kernel events.
		"""

	def cancel(self, operation:Link):
		"""
		# Remove the operation from the scheduled set.
		"""

	def enqueue(self, task:collections.abc.Callable):
		"""
		# Schedule the &task for immediate execution by &execute.
		"""
