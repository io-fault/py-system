"""
# Memory management classes for implementing usage constraints
# in parts of processes.
"""
import os
import weakref
import collections

class Memory(bytearray):
	"""
	# bytearray subclass supporting weak-references and
	# identifier hashing for memory-free signalling.
	"""
	try:
		import resource
		pagesize = resource.getpagesize()
		del resource
	except ImportError:
		pagesize = 1024*4

	__slots__ = ('__weakref__',)

	def __hash__(self, id=id):
		"""
		# Object Identifier based hash for allowing indexing of allocated instances.
		"""
		return id(self)

class MemoryContext(object):
	"""
	# Memory pool that uses weakref's and context managers to reclaim memory.
	"""
	Memory = Memory # bytearray subclass with weakref support
	Reference = weakref.ref # for reclaiming memory

	@classmethod
	def from_mib(typ, size):
		"""
		# Construct a Reservoir from the given number of Mebibytes desired.
		"""
		pages, remainder = divmod((size * (2**20)), self.Memory.pagesize)
		if remainder:
			pages += 1
		return typ(pages)

	def __init__(self, capacity = 8, Queue = collections.deque):
		self.capacity = capacity
		self.block_size = 2
		self.allocate_size = self.blocksize * self.Memory.pagesize
		self.transfer_allocations = 3

		self.segments = Queue()
		self.requests = Queue()
		self.current = None
		self._allocated = set()

		for x in range(self.capacity):
			self.segments.append(self.Memory(self.Memory.pagesize))

	@property
	def used(self):
		"""
		# Memory units currently in use.
		"""
		return len(self._allocated)

	@property
	def available(self):
		"""
		# Number of memory units available.
		"""
		return len(self.segments)

	def allocate(self):
		"""
		# Allocate a unit of memory for use. When Python references to the memory object
		# no longer exist, another unit will be added.
		"""
		if not self.segments:
			raise RuntimeError("empty")
		mem = self.segments.popleft()
		self._allocated.add(self.Reference(mem, self.reclaim))
		return mem

	def reclaim(self, memory, len = len):
		# Executed by the weakref() when the Memory() instance
		# is no longer referenced.

		# Remove weakreference reference.
		self._allocated.discard(memory)
		# Expand the Pool to fill in the new vacancy
		self.segments.append(self.Memory(self.Memory.pagesize))

		# signal transfer possible when in demand?
		if self.requests:
			pass

	def acquire(self, event):
		"""
		# Explicitly add an object to the available segments.
		"""
		self.segments.extend(event)

class Segments(object):
	"""
	# Iterate over the slices of an active memory map;
	# Weak references of the slices are held to track when
	# its appropriate to close the memory map.
	"""

	from mmap import mmap as MemoryMap
	from mmap import ACCESS_READ as ACCESS_MODE

	@classmethod
	def open(Class, path):
		"""
		# Open the file at the given path in read-only mode and
		# create a &Segments providing a &MemoryMap interface
		# to the contents.
		"""

		fd = os.open(path, os.O_RDONLY)
		try:
			s = Class(Class.MemoryMap(fd, 0, access=Class.ACCESS_MODE))
		finally:
			os.close(fd)

		return s

	def __init__(self, memory:MemoryMap):
		"""
		# Initialize an instance using the given &memory. An instance
		# created by &MemoryMap.

		# [ Parameters ]
		# /memory/
			# The `mmap.mmap` instance defining the total memory region.
		"""
		self.memory = memory
		self.weaks = weakref.WeakSet()

	def __del__(self):
		"""
		# Manage the final stages of &Segments deallocation by transitioning
		# to a finalization process where &select methods can no longer occur,
		# and existing &memoryview's referencing &self, &weaks, are
		# used to construct a sequence of &weakref.finalize callbacks.

		# Once in the final stage, a count of outstanding &memoryview instances
		# is tracked and decremented with &decrement until there are no more references
		# to the &Segments allowing the file descriptor associated with &memory to be closed.
		"""
		# The delete method is used as its
		# the precise functionality that is needed here.
		# A two-stage deallocation procedure is used:

		if self.weaks is not None:
			if len(self.weaks) > 0:
				# Add references, bringing the Segments refcount back to positive.
				self.__iter__ = None
				self.finals = [
					weakref.finalize(x, self.decrement) for x in self.weaks
				]
				self.count = len(self.finals)
				self.weaks.clear()
				self.weaks = None
			else:
				# no references to the slices, close memory
				self.weaks.clear()
				self.weaks = None
				self.memory.close()
		else:
			# second del attempt, decrement hit zero.
			self.memory.close()

	def decrement(self):
		"""
		# Used internally by &__del__ to manage the deallocation process
		# when there are outstanding references to the memory mapped region.
		"""
		self.count -= 1
		if self.count == 0:
			# this should trigger del's second stage.
			del self.finals[:]
			del self.finals

	def select(self, start, stop, size, memoryview=memoryview, iter=iter, range=range):
		"""
		# Constructs an iterator to the parameterized range over the
		# &memory the &Segments instance was initialized with.
		"""
		add_weak = self.weaks.add # (__del__ triggered finalization)
		stop = stop if stop is not None else len(self.memory)

		view = memoryview(self.memory)
		i = iter(range(start, stop, size))

		start = next(i)
		stop = 0

		for stop in i:
			vslice = view[start:stop]
			add_weak(vslice)
			yield vslice
			start = stop
		else:
			# use the last stop to start the final
			vslice = view[stop:stop+size]
			add_weak(vslice)
			yield vslice

	def __iter__(self):
		"""
		# Return an iterator to the entire region in sixteen kilobyte sizes.
		"""
		return self.select(0, len(self.memory), 1024*16)
