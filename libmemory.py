"""
Memory management classes for implementing usage constraints
in parts of processes.
"""

class Memory(bytearray):
	"""
	bytearray subclass supporting weak-references and
	identifier hashing for memory-free signalling.
	"""
	try:
		import resource
		pagesize = resource.getpagesize()
		del resource
	except ImportError:
		pagesize = 1024*4

	__slots__ = ('__weakref__',)

	def __hash__(self, id=id):
		return id(self)

class MemoryContext(object):
	"""
	Memory pool that uses weakref's and context managers to reclaim memory.
	"""
	Memory = Memory # bytearray subclass with weakref support
	Reference = weakref.ref # for reclaiming memory

	@classmethod
	def from_mib(typ, size):
		"""
		Construct a Reservoir from the given number of Mebibytes desired.
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
		Memory units currently in use.
		"""
		return len(self._allocated)

	@property
	def available(self):
		"""
		Number of memory units available.
		"""
		return len(self.segments)

	def allocate(self):
		"""
		Allocate a unit of memory for use. When Python references to the memory object
		no longer exist, another unit will be added.
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
		Explicitly add an object to the available segments.
		"""
		self.segments.extend(event)

class Segments(object):
	"""
	Iterate over the slices of an active memory map;
	Weak references of the slices are held to track when
	its appropriate to close the memory map.
	"""

	from mmap import mmap as MemoryMap
	from mmap import ACCESS_READ as ACCESS_MODE

	@classmethod
	def open(Class, path):
		f = open(path, 'rb')
		fd = f.fileno()
		s = Class(Class.MemoryMap(fd, 0, access=Class.ACCESS_MODE))
		del f
		return s

	def __init__(self, memory, start = 0, stop = None, size = 1024*4):
		self.range = (start, stop if stop is not None else len(memory), size)
		self.memory = memory
		self.weaks = weakref.WeakSet()

	def __del__(self):
		# The delete method is used as its
		# the precise functionality that is needed here.
		#
		# It is unusual that the weak set will ever be empty
		# when del is called, but if it is, it's a shortcut.

		if self.weaks is not None:
			if len(self.weaks) > 0:
				# Add references
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
		self.count -= 1
		if self.count == 0:
			# this should trigger del's second stage.
			del self.finals[:]
			del self.finals

	def __iter__(self, memoryview=memoryview):
		view = memoryview(self.memory)
		i = iter(range(*self.range))

		start = next(i)
		stop = None

		for stop in i:
			vslice = view[start:stop]
			self.weaks.add(vslice)
			yield vslice
			start = stop
		else:
			# use the last stop to start the final
			vslice = view[stop:stop+self.range[-1]]
			self.weaks.add(vslice)
			yield vslice
