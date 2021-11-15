"""
# System thread primitives.

# /create/
	# Create a new thread and run the given callable inside of it.
# /amutex/
	# Allocate a synchronization primitive.
# /identify/
	# Acquire an identifier to the executing thread.
"""
import sys
import typing
import types
import signal
import _thread

from . import runtime

create = _thread.start_new_thread
amutex = _thread.allocate_lock
identify = _thread.get_ident

class Sever(BaseException):
	"""
	# Exception used to signal thread kills.
	"""
	__kill__ = True

def snapshot(tids:typing.Sequence[int]) -> typing.Sequence[typing.Tuple[int, types.FrameType]]:
	"""
	# Select a set of threads from the same snapshot of frames.
	"""
	frames = sys._current_frames()
	return [(x, frames[x]) for x in tids]

def interrupt(tid, exception=None,
		setexc=runtime.interrupt,
		pthread_kill=signal.pthread_kill
	):
	"""
	# Raise the given exception in the thread with the given identifier, &tid.

	# The thread being interrupted will be signalled after the exception has been set.
	# This helps ensure that system calls will not stop the exception from being raised
	# in order to kill the thread.

	# ! WARNING:
		# Cases where usage is appropriate is rare. Managing the interruption
		# of threads in this fashion is only appropriate in certain applications.

	# [ Parameters ]

	# /tid/
		# The thread's low-level identifier to interrupt.
	# /exception/
		# The exception that is raised in the thread.
	"""
	global Sever

	r =  setexc(tid, exception or Sever)
	pthread_kill(tid, 0) # interrupt system call if any.

	return r

def frame(tid:int) -> types.FrameType:
	"""
	# Select the frame of the thread's identifier.

	# [ Parameters ]
	# /tid/
		# Identifier of the thread returned by &create_thread or &identify_thread.
		# Returns &None when the thread is not running.
	"""
	global sys
	return sys._current_frames().get(tid)

class Transition(object):
	"""
	# A synchronization mechanism used to perform a single transfer between threads.
	# Alternatively described as a queue with a transfer limit of one item.
	"""
	__slots__ = ('mutex', 'message')

	def __init__(self, mutex=amutex):
		self.message = None
		mtx = mutex()
		mtx.acquire()
		self.mutex = mtx

	def __iter__(self):
		return (self.commit, self.transfer).__iter__()

	def commit(self):
		"""
		# Commit to the transition. If the object
		# hasn't been placed, block until it is.

		# A RuntimeError will be raised upon multiple invocations of commit.
		"""
		mutex = self.mutex

		with mutex: # If error, Already transitioned.
			self.mutex = None
			message = self.message
			self.message = None
			return message

	def transfer(self, message):
		"""
		# Send &message to the receiving thread.
		"""
		self.message = message
		self.mutex.release() # If error, Already transferred.
