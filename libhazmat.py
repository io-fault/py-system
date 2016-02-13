"""
Lower-level interfaces to managing queues, processes, processing threads, and memory.

libhazmat is, in part, system's &threading implementation along with
other primitives and tools used by forking processes. However, its structure is vastly
different from &threading as the class structure is unnecessary for libhazmat's
relatively lower-lever applications. The primitives provided mirror many of the
&_thread library's functions with minimal or no differences.

The memory management interfaces exist to support a memory pool for applications
that need to enforce limits in particular areas. Python's weak references are used
to allow simple chunk reclaims when all the references are destroyed.

The process management interfaces exist to abstract the operating system's.

The queue managemnent interfaces provide queue implementations and containment tools for
interleaving processing chains for cooperative areas of an application.
"""

import os
import sys
import collections
import operator
import signal
import weakref
import _thread

from . import core
from . import kernel

processing_structure = ('kernel', 'group', 'process', 'thread',)
memory_structure = ('bytes',)

##
# Threading Primitives.
##

#: Mutex. Acquire a low-level lock primitive used for the synchronization of threads.
create_knot = _thread.allocate_lock

#: Return the low-level identity of the thread.
identify_thread = _thread.get_ident

#: Create a new thread.
create_thread = _thread.start_new_thread

#: Get the frame of a particular thread.
select_thread = lambda x: sys._current_frames().get(x)

#: Get the frames of a set of threads.
def select_fabric(tids):
	snapshot = sys._current_frames()
	r = [
		(x, snapshot[x]) for x in tids
	]
	return r

#: Kill a thread at the *Python* level.
def cut_thread(tid, exception = core.Sever, setexc=kernel.interrupt, pthread_kill=signal.pthread_kill):
	"""
	Raise the given exception in the thread with the given identifier.

	! WARNING:
		Cases where usage is appropriate is rare.

	[ Parameters ]

	/tid
		The thread's low-level identifier to interrupt.
	/exception
		The exception that is raised in the thread.
	"""
	r =  setexc(tid, exc)
	pthread_kill(tid, signal.SIGINT)
	return r

def pull_thread(callable, *args):
	"""
	Returns reference to the &Contained result.

	Execute a callable in a new thread returning a (callable) reference to
	the contained result. When the returned object is called, a mutex will
	be used in order to synchronize access to the result.

	pull_thread is best used in situations where concurrent processing should
	occur while waiting on another result, ideally a system call that will
	allow the dispatching-thread to run.

	In &threading terms, this is equivalent to creating and running a new thread
	and joining it back into calling thread.
	"""
	t = Transition()
	create_thread(t.relay, callable, *args)
	return t.commit

def chain(iterable, initial, contain = core.contain):
	"""
	Given an iterable of generators, send or throw the Contained result into the
	next, returning the last container result.

	In the case of a ContainedRaise, throw the actual exception into the
	generator, not Containment. Chain discard the Container leaving
	little possibility of another open mangling the traceback.

	! NOTE:
		The `initial` container given should not be used again.

	[ Parameters ]
	chain(iterable, initial)

	/&iterable
		The generators to connect. &collection.Iterable
	/&initial
		The initial Container to give to the first generator.
		&Container
	"""

	param = initial

	for generator in iterable:
		why = param[0]
		if param.failed:
			param = contain(generator.throw, why.__class__, why, why.__traceback__)
		else:
			param = contain(generator.send, why)

	return param

##
# Process Related mappings
##

class Delivery(object):
	"""
	A reference to the delivery of a Container.

	Delivery objects allow for latent assignments of the "container" to be
	delivered and the latent "endpoint" that it is to be delivered to.
	Latent being defined at an arbitrary time in the future.

	Delivery should be used in cases where completion of an operation
	depends on a subsequent event. The event is received by an object
	controlled or known by the called function, but not the caller.

	! NOTE:
		Connecting deliveries with Joins is a powerful combination.
	"""
	__slots__ = ('callback', 'container')

	def __init__(self):
		self.callback = None
		self.container = None

	def send(self, container):
		"""
		Deliver the given arguments to the designated endpoint.
		If there is no endpoint, hold onto the arguments until a corresponding
		&endpoint call is performed allowing delivery to be committed.
		"""
		self.container = container
		if self.callback is not None:
			self.commit()

	def endpoint(self, callback):
		"""
		Specify the given &callback as the designated endpoint.
		If there is no package, hold onto the &callback until a corresponding
		&send call is performed allowing delivery to be committed.
		"""
		self.callback = callback
		if self.container is not None:
			self.commit()

	def commit(self):
		return self.callback(self.container)

class Switch(object):
	"""
	Event driven mutex.

	A queue that manages callbacks that are invoked when a given
	callback reached the head of the queue.

	Using &create_knot in combination with join
	is a great way to manage continuations based on exclusive access.

	! WARNING:
		Do not use.
	"""
	__slots__ = ('_current', '_waiters',)

	def __init__(self, Queue = collections.deque):
		self._waiters = Queue()
		self._current = None

	def acquire(self, callback):
		"""
		Return boolean on whether or not it was **immediately** acquired.
		"""
		self._waiters.append(callback)
		# At this point, if there is a _current,
		# it's release()'s job to notify the next
		# owner.
		if self._current is None and self._waiters[0] is callback:
			self._current = self._waiters[0]
			self._current()
			return True
		return False

	def release(self):
		"""
		Returns boolean on whether or not the Switch was
		released **to another controller**.
		"""
		if self._current is not None:
			if not self._waiters:
				# not locked
				return False
			self._waiters.popleft()

			if self._waiters:
				# new owner
				self._current = self._waiters[0]
				self._current()
			else:
				self._current = None
		return True

	def locked(self):
		return self._current is not None

class EQueue(object):
	"""
	An event driven queue where references to readers and references
	to writers are passed in order to conflate queue event types.

	This implementation is *not* thread safe.

	Event driven queues have a remarkable capability of providing a
	means to communicate when the reader of the queue wants more.
	By using "NULL" items, a the source of items can identify that
	the receptor is looking to retrieve more information.

	! WARNING:
		Get and put operations are **not** thread safe.
	"""
	__slots__ = ('state', 'struct')

	def __init__(self, storage = None, Queue = collections.deque):
		# writers are appended right of the "center"
		# readers are prepended left of the "center"
		self.struct = Queue(storage) if storage is not None else Queue()

		# negative means waiting readers
		# positive means waiting writers
		self.state = len(self.struct)

	@property
	def backlog(self):
		return self.state

	def send(self, container):
		return self.put(container.self)

	def put(self, item):
		if self.state >= 0:
			self.state += 1
			self.struct.append(item)
		else:
			# have readers; they appendleft of zero
			target = self.struct.pop()
			self.state += 1
			target(item)
			return True
		return False

	def endpoint(self, callback):
		if callback is None:
			return
		if self.state <= 0:
			self.state -= 1
			self.struct.appendleft(callback)
		else:
			# have writers; they appendright of zero
			item = self.struct.popleft()
			self.state -= 1
			callback(item)
			return True
		return False
	get = endpoint

	def fasten(self, src):
		"""
		Fasten two &Queue instances together sending items from
		the source to the destination.

		! WARNING:
			Currently, there is no way to terminate a connection.

		Often, if possible, it is better to replace the destination queue with
		the source. This provides the desired functionality without the extra
		effort.
		"""
		def didget(x, dop = self.put, sop = src.get):
			dop(x)
			sop(didget) # enqueue the next transfer
		src.get(didget)

class XQueue(EQueue):
	"""
	Unbounded Blocking Queue for interthread communication.

	Puts do not wait. Similar to a barrier, but gets do when
	there are no items.
	"""
	__slots__ = EQueue.__slots__ + ('mutex',)

	def __init__(self, Mutex = create_knot):
		super().__init__()
		self.mutex = Mutex()

	def get(self):
		with self.mutex:
			if self.state > 0:
				# no waiting mode; no transition needed
				self.state -= 1
				return self.struct.popleft()
			# enqueue a transition callback
			T = Transition()
			super().get(T.endpoint)
		return T.commit()

	def put(self, item):
		with self.mutex:
			super().put(item)
