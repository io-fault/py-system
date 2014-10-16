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

from .kernel import Interface as Kernel
from . import system

from . import libhazmat

#: Declaration of main thread :py:func:`.control`
__control_lock__ = libhazmat.create_knot()

#: Protects superfluous interjections.
__interject_lock__ = libhazmat.create_knot()
__interject_lock__.acquire() # released in Fork.trap()

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
#: If :py:mod:`fork.lib` did not perform the :manpage:`fork(2)` operation,
#: these callables will *not* be ran.
fork_child_callset = set()

#: Initial set of callables to run. These are run whether or not the fork operation
#: was managed by :py:mod:`fork.lib`.
fork_child_cleanup = set()

def interject(main_thread_exec, signo = signal.SIGUSR2):
	"""
	Trip the main thread by sending the process a SIGUSR2 signal in order to cause any
	running system call to exit early. Used in conjunction with
	:py:func:`.system.interject`.
	"""
	__interject_lock__.acquire() # One interjection at a time.
	system.interject(main_thread_exec) # executed in main thread
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
def _after_fork_parent(child_pid):
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
	# Unconditionally update this data.
	parent_process_id = current_process_id
	current_process_id = os.getpid()

	if not __fork_knot__.locked():
		# Only perform cleanup tasks
		for after_fork_in_child_task in fork_child_cleanup:
			after_fork_in_child_task()
	else:
		try:
			for after_fork_in_child_task in fork_child_cleanup:
				after_fork_in_child_task()

			for after_fork_in_child_task in fork_child_callset:
				after_fork_in_child_task()
		finally:
			__fork_knot__.release()

class Exit(SystemExit):
	"""
	Extension of SystemExit for use with interjections.
	"""
	#: :py:class:`Execution` exit status code used to indicate that the
	#: process will exit using a signal during :manpage:`atexit(2)`.
	#: The calling process will *not* see this code. Internal indicator.
	exiting_by_signal_status = 254

	#: :py:class:`Execution` exit status code used to indicate that the
	#: the process failed to explicitly note status.
	exiting_by_default_status = 255

	def raised(self):
		raise self

class Execution(object):
	"""
	A handle to the execution of the process. This is used to manage custom invocations
	without interferring with the process global data in the :py:mod:`sys` module.

	libfork also uses Execution instances to manage the exit status to use with the
	program.

	By default, the status is the highest status possible, `255`.

	Execution instances have four fields:

	 context
	  None or a dictionary object describing any prior stack of commands or function paths
	  that led up to the existance of this object.

	 name
	  By default, the process name as acquired from sys.argv.

	 arguments
	  By default, the remaining arguments after the path from sys.argv. The arguments
	  given to the :c:func:`main` function of the program.

	 exit_status
	  The status code to exit the process with.
	"""
	__slots__ = ('context', 'name', 'arguments', 'exit_status')

	def __init__(self, name, arguments, context = None):
		self.context = context
		self.name = name
		self.arguments = arguments
		self.exit_status = Exit.exiting_by_default_status

	@classmethod
	def default(cls, **kw):
		"""
		Create an execution instance from the information in the :py:mod:`sys` module.
		"""
		return cls(sys.argv[0], sys.argv[1:], **kw)

	def set_exit_status(self, status):
		"""
		Configure the exit status to use on :py:meth:`.exit`. This can be overwritten.
		"""
		self.exit_status = status

	def exit(self, status = None):
		"""
		Interject the :py:class:`.Exit` instance and cause the process to exit with
		the status code configured on this :py:class:`.Execution` instance.
		"""
		interject(Exit(self.exit_status).raised)

class Control(BaseException):
	"""
	Process control exceptions for significant events.

	.. note:: This inherits from BaseException.
	"""
	__kill__ = None

	def raised(self):
		raise self

class Panic(Control):
	"""
	An exception used to note the failure of a critical resource.

	Instances of this class are usually interjected into the main thread causing
	the process to immediately terminate.

	.. note:: This ultimately inherits from BaseException.
	"""
	__kill__ = True

class Interruption(Control):
	"""
	Similar to KeyboardInterrupt, but causes :py:func:`control` to exit with the signal,
	and calls critical status hooks.
	"""
	__kill__ = True

	def __init__(self, type, signo = None):
		self.type = type
		self.signo = signo

	def __str__(self):
		if self.type == 'signal':
			signame = libhazmat.process_signal_names.get(self.signo, 'unknown')
			sigid = libhazmat.process_signal_identifiers.get(self.signo, 'UNKNOWN-SIG')
			return "[{0} signal] {1}: {2}".format(signame, sigid, str(self.signo))
		else:
			return str(self.type)

	def raised(self):
		"""
		Register a libc-level atexit handler that will cause the process to exit with
		the configured signal.
		"""
		# if the noted signo is normally fatal, make it exit by signal.
		if self.signo in libhazmat.process_fatal_signals:
			# SIG_DFL causes process termination
			system.exit_by_signal(self.signo)

		return super().raised() # Interruption

	@classmethod
	def interrupt(Class, signo, frame):
		"""
		Signal handler that interjects an Interruption instance into the main thread.

		Default nucleus signal handler for SIGINT.
		"""
		if signo in libhazmat.process_fatal_signals:
			interject(Class('signal', signo).raised) # .fork.lib.Interruption

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
		Assign the interrupt signal handler to the all of the given signal numbers.
		"""
		for x in sigs:
			signal(x, Class.interrupt)

	@classmethod
	@contextlib.contextmanager
	def trap(Class,
		catches = (signal.SIGINT, signal.SIGTERM),
		filters = (signal.SIGUSR1, signal.SIGUSR2),
		signal = signal.signal, ign = signal.SIG_IGN,
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
		transitioned_pivot = functools.partial(Class(controller, *args, **kw).pivot, T)
		interject(transitioned_pivot) # .fork.lib.Fork.dispatch

		# wait on commit until the fork() in the above pivot() method occurs in the main thread.
		return T.commit()

	@classmethod
	def trap(Class, controller, *args, **kw):
		"""
		trap(controller, *args, **kw)

		Establish a point for substituting the process. Trap provides an
		exception trap for replacing the controlling stack. This is used to
		perform safe fork operations that allow tear-down of process specific resources
		in a well defined manner.

		.. note::
			Due to the varying global process state that may exist in a given process, it
			is often better to start a new Python instance.
		"""
		while True:
			try:
				if not __interject_lock__.locked():
					raise Panic("interjection knot not configured")
				try:
					__interject_lock__.release()
					return controller(*args, **kw) # Process replacement point.
				finally:
					__interject_lock__.acquire(0) # block subsequent acquisitions
			except Class as exe:

				# Raised a Fork exception.
				# This is normally used by clone resources.

				# Replace existing locals and loop.
				controller = exe.controller
				args = exe.arguments
				kw = exe.keywords

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
		return callable(*args, **kw) # should never exit
	except BaseException as exc:
		ce = Panic("critical call raised exception")
		ce.__cause__ = exc

		if __control_lock__.locked():
			raise_panic = ce.raised
			system.interject(raise_panic) # .fork.lib.critical
		else:
			raise ce

def protect(*init, looptime = 8):
	"""
	Perpetually protect the main thread.
	Used by :py:func:`control` to hold the main thread in :py:meth:`Fork.trap`.
	"""
	import time
	global current_process_id, parent_process_id

	while 1:
		time.sleep(looptime) # main thread system call

		# Check for parent process changes.
		newppid = os.getppid()
		if newppid != parent_process_id:
			# Emit a context signal to the process.
			parent_process_id = newppid
			os.kill(os.getpid(), libhazmat.process_signals['context'])

	# Relies on Fork.trip() and system.interject to manage the main thread's stack.
	raise Panic("infinite loop exited")

def control(main, *args, **kw):
	"""
	control(execution = None)

	:param execution: Initial parameter data extracted from sys.argv.
	:type execution: :py:class:`.Execution`

	Give control of the process over to the manager.

	A program that calls this is making an explicit declaration that signals should be
	defaulted and the main thread should be protected from uninterruptable calls to allow
	prompt exits.
	"""
	# Registers the atfork functions.
	system.initialize(sys.modules[__name__])

	with Interruption.trap(), __control_lock__:
		try:
			Fork.trap(main, *args, **kw)
			# Fork.trap() should not return.
			raise RuntimeError("fork.lib.Fork.trap did not raise SystemExit or Interruption")
		except Interruption as e:
			import traceback
			sys.stderr.write("\nINTERRUPT: {0}\n".format(str(e)))
			sys.stderr.flush()
			raise Exit(250)

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
