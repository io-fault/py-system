"""
libfork is used to manage main thread control and ordered process forks.

Arguably, libcontrol is also an appropriate name, but the purpose of the module's
content orbits the management of the forking process in general.
"""
import sys
import os
import signal
import functools
import contextlib

from . import system
from . import libhazmat

#: Declaration of main thread :py:func:`.control`
__control_lock__ = libhazmat.create_knot()

#: Call to identify if :py:func:`.control` is managing the main thread.
controlled = __control_lock__.locked

#: Maintained process identifier object. Do not change or delete.
current_process_id = os.getpid()

#: Currently identified parent process.
parent_process_id = os.getppid()

#: Intercontext Fork Lock
__fork_knot__ = libhazmat.create_knot()

#: Add callables to be dispatched in the parent *before* a fork occurs.
#: Useful for creating context independent parent-child connections.
fork_prepare_callset = set()

#: Add callables to be dispatched in the parent after a fork call is performed.
#: Each object in the set will be called with one parameter, the process id of the child.
fork_parent_callset = set()

#: Add callables to be dispatched in the child after a fork call is performed.
fork_child_callset = set()

def trip(signo = signal.SIGUSR2):
	"""
	Trip the main thread by sending the process a SIGUSR2 signal in order to cause any
	running system call to exit early. Used in conjunction with
	:py:func:`.system.interject`.
	"""
	os.kill(current_process_id, signo)

def clear_atexit_callbacks(pid = None):
	"""
	In cases where there may be process dependent callbacks, add this to the
	:py:attr:`.fork_child_callset` to clear the callbacks.
	"""
	if 'atexit' in sys.modules:
		# It's somewhat uncommon to retain the forked process image,
		# so Python just leaves atexit alone. In the context of a nucleus
		# managed process, we anticipate that it will exit normally and
		# fire the atexit callbacks which will be redundant with the parent.
		try:
			sys.modules['atexit']._clear()
		except:
			# XXX: Warning
			pass

##
# These are invoked by AddPendingCall.
def _after_fork_parent(child_pid, partial = functools.partial):
	if not __fork_knot__.locked():
		# Don't perform related duties unless the fork() was managed by libfork.
		return
	try:
		for after_fork_in_parent_task in fork_parent_callset:
			after_fork_in_parent_task(child_pid)
	finally:
		__fork_knot__.release()

def _after_fork_child():
	global parent_process_id, current_process_id
	parent_process_id = current_process_id
	current_process_id = os.getpid()

	if not __fork_knot__.locked():
		# Don't perform related duties unless the fork() was managed by libfork.
		return

	try:
		for after_fork_in_child_task in fork_parent_callset:
			after_fork_in_child_task(child_pid)
	finally:
		__fork_knot__.release()

class Execution(object):
	"""
	A handle to the execution of the process. This is used to manage custom invocations
	without interferring with the process global data in the :py:mod:`sys` module.

	libfork also uses Execution instances to manage the exit status to use with the
	program. Normally, an instance is given to the primary process
	:py:class:`.lib.Context`. The procedures may then adjust the exit code to suite their
	needs.

	By default, the status is the highest status possible, `255`.
	"""
	__slots__ = ('path', 'arguments', 'exit_status')

	def __init__(self, path, arguments, default_status = 255, executable = sys.executable):
		self.executable = executable
		self.path = path
		self.arguments = arguments
		self.exit_status = default_status

	@classmethod
	def default(cls):
		"""
		Create an execution instance from the information in the :py:mod:`sys` module.
		"""
		return cls(sys.path[0], sys.path[1:])

	def exit_by_status(self, status):
		"""
		Configure the exit status to use on exit.
		This can be overwritten.
		"""
		self.exit_status = status

class Control(BaseException):
	"""
	Process control exceptions for significant events.

	.. note:: This inherits from BaseException.
	"""
	__kill__ = None

	@classmethod
	def raised(Class, *args, **kw):
		raise Class(*args, **kw)

class Panic(Control):
	"""
	An exception used to note the failure of a critical resource.

	Instances of this class are usually interjected into the main thread causing
	the process to immediately terminate.

	.. note:: This ultimately inherits from BaseException.
	"""
	__kill__ = True

	def interjection(self):
		raise self

class Interruption(Control):
	"""
	Similar to KeyboardInterrupt, but causes :py:func:`control` to exit with the signal.
	"""
	def __init__(self, type, signo = None):
		self.type = type
		self.signo = signo

	def __str__(self):
		return "[{2}] {0}({1})".format("S", str(self.signo), self.type)

	def raised(self):
		"""
		Register a libc-level atexit handler that will cause the process to exit with
		the configured signal.
		"""
		if self.signo in libhazmat.process_fatal_signals:
			# SIG_DFL causes process termination
			system.exit_by_signal(self.signo)
		raise self

	def interject(self, ij = system.interject):
		ij(self.raised)

	@classmethod
	def interrupt(Class, signo, frame):
		"""
		Signal handler that interjects an Interruption instance into the main thread.

		Default nucleus signal handler for SIGINT.
		"""
		if signo in libhazmat.process_fatal_signals:
			Class('signal', signo).interject()

	@staticmethod
	def void(signo, frame):
		"""
		Python-level signal handler that does nothing.

		When Interruption has set traps, a Context will respond to signals.
		"""
		pass

	@classmethod
	def filter(Class, *sigs, signal = signal.signal):
		"""
		Assign the void signal handler to the all of the given signal numbers.
		"""
		for x in sigs:
			signal(x, Class.void)

	@classmethod
	def catch(Class, *sigs, signal = signal.signal):
		"""
		Assign the void signal handler to the all of the given signal numbers.
		"""
		for x in sigs:
			signal(x, Class.interrupt)

	@classmethod
	@contextlib.contextmanager
	def trap(Class,
		signal = signal.signal,
		ign = signal.SIG_IGN,
		catches = (signal.SIGINT, signal.SIGTERM),
		filters = (signal.SIGUSR1, signal.SIGUSR2),
	):
		"""
		Signal handler for a root process.
		"""
		stored_signals = {}
		try:
			for sig in libhazmat.process_fatal_signals:
				# In Context situations, signals are read from kernel.Interface()
				stored_signals[sig] = signal(sig, ign)

			# these signals need a handler and are used to trip the main thread for interjections.
			Class.filter(*filters)
			Class.catch(*catches)
			yield None
		finally:
			# restore the signal handlers.
			# generally, this doesn't matter.
			for k, v in stored_signals.items():
				signal(k, v)

class Fork(Control):
	"""
	Fork the process with the express purpose of keeping the process image in order to
	divide the load of the process across its clones.

	Specifically, execute the callable at a pivot point replacing the existing stack.
	"""
	def __init__(self, controller, *args, **kw):
		self.controller = controller
		self.arguments = args
		self.keywords = kw

	def __str__(self):
		return "no nucleus.lib.Fork.trap() call in main thread"

	def pivot(self, T, fork = os.fork):
		pid = fork()
		# Unconditionally perform the transition, it doesn't matter.
		T.endpoint(ContainedReturn((pid,)))
		if pid == 0:
			# In the child, raise the Fork() exception
			# to trigger pivot's replacement functionality.
			raise self

	@classmethod
	def dispatch(Class, controller, *args, **kw):
		"""
		dispatch(controller, *args, **kw)

		:param controller: The object that will be called in the clone.
		:param args: Arguments given to the callable.
		:param kw: Keywords given to the callable.
		:returns: The child process' PID.
		:rtype: :py:class:`int`

		Execute the given callable with the given arguments in a child process. This causes
		:py:class:`Execution` to be raised in the main thread of the process. Given that
		:py:func:`pivot` was called to execute the program, the pivot function will catch the
		exception, in the child, and execute the replacement.
		"""
		__fork_knot__.acquire() # Released by atfork handler.

		# Transition is used because we're probably in a thread and we want to hold
		# until the fork occurs.

		T = libhazmat.Transition()
		system.interject(functools.partial(Class(controller, *args, **kw).pivot, T))

		trip()
		return T.commit()

	@classmethod
	def trap(Class, controller, *args, **kw):
		"""
		trap(callable, *args, **kw)

		Establish a point for substituting the process. Trap provides an
		exception trap for replacing the controlling stack. This is used by execute to
		perform safe fork operations that allow tear-down of process specific resources
		in a well defined manner.

		.. note::
			Due to the varying global process state that may exist in a given process, it
			is often better to start a new Python instance.
		"""
		while True:
			try:
				return controller(*args, **kw) # Process replacement point.
			except Class as exe:
				# Raised a Fork exception.
				# This is normally used by clone resources.

				# Replace existing locals and loop.
				controller = exe.controller
				args = exe.arguments
				kw = exe.keywords

class Exit(SystemExit):
	"""
	Extension of SystemExit for interjection.
	"""
	def raised(self):
		raise self

	def interjection(self):
		"""
		"""
		# Transition is used because we're probably in a thread and we want to hold
		# until the fork occurs.
		system.interject(self.raised)
		trip()

def critical(callable, *args, **kw):
	"""
	A Callable used to trap exceptions and interject a :py:class:`Panic` instance caused by the
	original::

		from nucleus.lib import critical

		def fun():
			while True:
				# critical loop
				# any exception cause the process to terminate
				...

		critical(fun)
	"""
	try:
		return callable(*args, **kw)
	except BaseException as exc:
		ce = Panic("critical call raised exception")
		ce.__cause__ = exc
		if __control__:
			system.interject(ce.interjection)
			trip()
		else:
			raise ce

def protect(prepare = None, looptime = 16):
	"""
	Perpetually protect the main thread.
	Used by :py:func:`control` to hold the main thread in :py:meth:`Fork.trap`.
	"""
	import time
	global current_process_id, parent_process_id

	for x in init:
		x()
	del init

	while 1:
		time.sleep(looptime)

		# Check for parent process changes.
		newppid = os.getppid()
		if newppid != parent_process_id:
			# Emit a context signal to the process.
			parent_process_id = newppid
			os.kill(os.getpid(), libhazmat.process_signals['context'])

	# Relies on trip() and system.interject to manage the main thread's stack.
	raise Panic("infinite loop exited normally")

def control():
	"""
	Give control of the process over to the manager.

	A program that calls this is making an explicit declaration that signals should be
	defaulted and the main thread should be protected from uninterruptable calls to allow
	prompt exits. The given `manager` function is executed in the main thread.
	"""
	# Registers the atfork functions.
	system.initialize(sys.modules[__name__])

	with Interruption.trap(), __control_lock__:
		try:
			# signal.SIGINT
			Fork.trap(manager, init = init)

			# Not expecting trap() to return.
			raise RuntimeError("trap did not raise SystemExit or Interruption")
		except Interruption as e:
			sys.stderr.write("\nINTERRUPT: {0}\n".format(e.type))
			sys.stderr.flush()

def concurrently(controller, exe = Fork.dispatch):
	"""
	concurrently(controller)

	:param controller: The object to call.
	:type controller: :py:class:`collections.Callable`
	:returns: Reference dispatched result.
	:rtype: :py:class:`collections.Callable`

	Dispatch the given controller in a child process of a libfork controlled process.
	The returned object is a reference to the result that will block until the child
	process has written the response to a pipe.
	"""
	if not __control_lock__.locked():
		raise RuntimeError("main thread is not managed with libfork.control")

	rw = os.pipe()

	import io
	import pickle

	dump = pickle.dump
	load = pickle.load

	def exec(call = callable, rw = rw):
		write = io.open(rw[1], 'wb')
		os.close(rw[0])
		with write:
			try:
				result = call()
			except SystemExit:
				result = None
			dump(result, write)
		raise SystemExit(0)

	# child never returns
	pid = exe(exec)
	del exec

	os.close(rw[1])
	def read_child_result(read = io.open(rw[0], 'rb'), pid = pid):
		try:
			with read:
				return load(read)
		except EOFError:
			r = os.waitpid(pid)
			return None
		finally:
			os.waitpid(pid)

	return read_child_result
