"""
fork.library is used to manage main thread control and controlled process fork() sequences.

In some versions of C-Python, forking a process in a thread can leave the process in an
inconsistent state unless some maintenance is performed during the fork operation.
fork.library provides interfaces for managing callback sets for resolving inconsistent state
after a fork operation is performed and control functions to allow for safe main thread
forking.

In addition to safe forking, the main thread protection allows for immmediate exits to be
performed when dealing with interrupt signals from the operating system. Considering that
no significant process machinery should exist there, the interrupt exception can be raised
so that fork can properly propagate error. Notably, fork uses some of the C libraries to
return proper status codes for SIGINT killed processes.
"""
import sys
import os
import signal
import functools
import contextlib

from . import system
from . import kernel
from . import libhazmat

# Lock held when @control is managing the main thread.
__control_lock__ = libhazmat.create_knot()

# Protects superfluous interjections.
__interject_lock__ = libhazmat.create_knot()
__interject_lock__.acquire() # released in Fork.trap()

# Call to identify if @control is managing the main thread.
controlled = __control_lock__.locked

# Maintained process identifier object. Do not change or delete.
current_process_id = os.getpid()

# Currently identified parent process.
parent_process_id = os.getppid()

# Intercontext Fork Lock
__fork_knot__ = libhazmat.create_knot()

# Add callables to be dispatched in the parent *before* a fork occurs.
# Useful for creating context independent parent-child connections.
fork_prepare_callset = set()

# Add callables to be dispatched in the parent after a fork call is performed.
# Each object in the set will be called with one parameter, the process id of the child.
fork_parent_callset = set()

# Add callables to be dispatched in the child after a fork call is performed.
# If @fork.library did not perform the :manpage:`fork(2)` operation,
# these callables will *not* be ran.
fork_child_callset = set()

# Initial set of callables to run. These are run whether or not the fork operation
# was managed by @fork.library.
fork_child_cleanup = set()

def interject(main_thread_exec, signo = signal.SIGUSR2):
	"""
	Trip the main thread by sending the process a SIGUSR2 signal in order to cause any
	running system call to exit early. Used in conjunction with
	@system.interject
	"""
	__interject_lock__.acquire() # One interjection at a time.
	system.interject(main_thread_exec) # executed in main thread
	os.kill(current_process_id, signo)

def clear_atexit_callbacks(pid = None):
	"""
	In cases where there may be process dependent callbacks, add this to the
	&fork_child_callset to clear the callbacks.
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

class SystemExit(SystemExit):
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

	# Exit code indicating the type of information presented on standard error.
	# Indicates that the standard error contains help output.
	exiting_with_information = 100

	def raised(self):
		raise self

class Invocation(object):
	"""
	A means of representing the invocation of an abstract executable and the specification
	of the means of exiting. Normally, used to describe how the process was invoked and the
	corresponding parameters, argv and environ, in which the invocation should be reacting to.

	For system invocation, the &parameters dictionary will have two entries by default
	"""
	def __init__(self, exit_method, context = None):
		self.exit_method = exit_method
		self.parameters = {}
		self.context = context

	def exit(self, result):
		"""
		Perform the exit method designated during the initialization of the invocation.
		"""
		self.exit_method(result)

	@classmethod
	def system(Class, context = None, environ = ()):
		"""
		Create an instance representing that of the invocation from the operating
		system. Primarily, information is retrieved from the @sys and @os module.
		"""
		r = Class(Class.system_exit_method, context = context)
		r.parameters['type'] = 'system'

		system = r.parameters['system'] = {}
		system['name'] = sys.argv[0]
		system['arguments'] = sys.argv[1:]
		system['directory'] = os.getcwd()

		if environ:
			# copy interesting environment variables
			local = system['environment'] = {}
			for x in environ:
				if isinstance(x, str):
					local[x] = os.environ[x]
				else:
					local[x] = os.environ[x]

		r.exit_method = Class.system_exit_method
		return r

	@classmethod
	def system_exit_method(Class, exit_status):
		"""
		A means of exit used with a @Fork.trap managed process.
		"""
		interject(SystemExit(exit_status).raised)

class Control(BaseException):
	"""
	Process control exceptions for significant events.

	This is a control exception inheriting from @BaseException. It should not be trapped.
	"""
	__kill__ = None

	def raised(self):
		raise self

class Panic(Control):
	"""
	An exception used to note the failure of a critical resource.

	Instances of this class are usually interjected into the main thread causing
	the process to immediately terminate.

	This is a control exception inheriting from @BaseException. It should not be trapped.
	"""
	__kill__ = True

class Interruption(Control):
	"""
	Similar to @KeyboardInterrupt, but causes @control to exit with the signal,
	and calls critical status hooks.

	Primarily used to cause signal exit codes that are usually masked with
	@KeyboardInterrupt.
	"""
	__kill__ = True

	def __init__(self, type, signo = None):
		self.type = type
		self.signo = signo

	def __str__(self):
		if self.type == 'signal':
			signame = libhazmat.process_signal_names.get(self.signo, 'unknown')
			sigid = libhazmat.process_signal_identifiers.get(self.signo, 'UNKNOWN-SIG')
			return "{1}[{2}]".format(signame, sigid, str(self.signo))
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

		Default signal handler for SIGINT.
		"""
		if signo in libhazmat.process_fatal_signals:
			interject(Class('signal', signo).raised) # .fork.library.Interruption

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
	@Control exception used to signal @Fork.trap to replace the existing managed call.

	Usual case is that a @Fork.trap call is made on the main thread where other threads are
	created to run the actual program. Given that the program is finished and another should be ran
	*as if the current program were never ran*, the @Control exception can be raised in the
	main thread replacing the initial callable given to @Fork.trap.

	The exception should only be used through the provided classmethods.
	"""
	__controlled_thread_id__ = None

	def __init__(self, controller, *args, **kw):
		self.controller = controller
		self.arguments = args
		self.keywords = kw

	def __str__(self):
		return "no Fork.trap was present to catch the exception"

	def pivot(self, T, fork = os.fork):
		pid = fork()
		# Unconditionally perform the transition, it doesn't matter.
		T.endpoint(libhazmat.ContainedReturn((pid,)))
		if pid == 0:
			# In the child, raise the Fork() exception
			# to trigger pivot's replacement functionality.
			raise self

	@classmethod
	def substitute(Class, callable, *args, **kw):
		"""
		Substitute the existing control call with the given one.

		Immediately raises a @Fork instance in the calling thread to be caught
		by a corresponding @trap call.

		Only to be used in cases where it is known that the current frame stack is
		being managed by @trap.
		"""
		raise Class(callable, *args, **kw)

	@classmethod
	def dispatch(Class, controller, *args, **kw):
		"""
		dispatch(controller, *args, **kw)

		:param controller: The object that will be called in the clone.
		:param args: Arguments given to the callable.
		:param kw: Keywords given to the callable.
		:returns: The child process' PID.
		:rtype: :py:class:`int`

		Execute the given callable with the given arguments in a child process.
		This performs an @interject call. Given that @pivot was called to execute the
		program, the pivot function will catch the exception, in the child, and
		execute the replacement.
		"""
		fcontroller = Class(controller, *args, **kw)

		# Don't bother with the interjection if we're dispatching from the main thread.
		# Usually, this doesn't happen, but it can be desirable to have fork's control
		# provisions in even simple programs.
		if Class.__controlled_thread_id__ == libhazmat.identify_thread():
			pid = os.fork()
			if pid == 0:
				raise fcontroller
			return pid
		else:
			# Not in the thread controlled by Fork.trap().

			# Transition is used because we're probably in a thread and we want to hold
			# until the fork occurs.
			T = libhazmat.Transition()
			transitioned_pivot = functools.partial(fcontroller.pivot, T)

			__fork_knot__.acquire() # Released by atfork handler.
			interject(transitioned_pivot) # .fork.library.Fork.dispatch

			# wait on commit until the fork() in the above pivot() method occurs in the main thread.
			return T.commit()
		raise RuntimeError("method branches did not return process identifier")

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
			Class.__controlled_thread_id__ = libhazmat.identify_thread()
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

def critical(context, callable, *args, **kw):
	"""
	A Callable used to trap exceptions and interject a @Panic instance caused by the
	original.

	For example::

		from fault.fork.library import critical

		def fun():
			while True:
				# critical loop
				# any exception causes the process to terminate
				...

		critical(None, fun)
	"""
	try:
		r = callable(*args, **kw) # should never exit
		return r
	except BaseException as exc:
		ce = Panic("critical call raised exception")
		ce.__cause__ = exc

		if __control_lock__.locked():
			raise_panic = ce.raised
			system.interject(raise_panic) # .fork.library.critical
		else:
			raise ce

def protect(*init, looptime = 8):
	"""
	Perpetually protect the main thread.

	Used by @control to hold the main thread in @Fork.trap.
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
	control(main, *args, **kw)

	A program that calls this is making an explicit declaration that signals should be
	defaulted and the main thread should be protected from uninterruptable calls to allow
	prompt process exits.

	The given @main is executed with the given positionals @args and keywords @kw inside
	of a @Fork.trap call. @Fork handles formal exits and main-call substitution.
	"""
	# Registers the atfork functions.
	system.initialize(sys.modules[__name__])

	with Interruption.trap(), __control_lock__:
		try:
			Fork.trap(main, *args, **kw)
			# Fork.trap() should not return.
			raise RuntimeError("fork.library.Fork.trap did not raise SystemExit or Interruption")
		except Interruption as e:
			highlight = lambda x: '\x1b[38;5;' '196' 'm' + x + '\x1b[0m'
			sys.stderr.write("\r{0}: {1}".format(highlight("INTERRUPT"), str(e)))
			sys.stderr.flush()
			raise SystemExit(250)

def concurrently(controller, exe = Fork.dispatch):
	"""
	#!/usr/bin/env eclectic

	/controller
		The object to call to use the child's controller. &collections.Callable

	Dispatch the given controller in a child process of a fork.library controlled process.
	The returned object is a reference to the result that will block until the child
	process has written the pickled response to a pipe.
	#!/bin/exit
	"""
	if not __control_lock__.locked():
		raise RuntimeError("main thread is not managed with libfork.control")

	rw = os.pipe()

	import io
	import pickle

	dump = pickle.dump
	load = pickle.load

	def execute_controller(call = controller, rw = rw):
		os.close(rw[0])
		try:
			result = call()
		except SystemExit:
			result = None

		write = io.open(rw[1], 'wb')
		dump(result, write)
		write.close()
		raise SystemExit(0)

	# child never returns
	pid = exe(execute_controller)
	del execute_controller

	os.close(rw[1])
	def read_child_result(read = io.open(rw[0], 'rb'), pid = pid, status_ref = None):
		try:
			with read:
				result = load(read)
		except EOFError:
			result = None

		status = os.waitpid(pid, 0)
		if status_ref is not None:
			status_ref(status)

		return result

	return read_child_result

# Public export of kernel.Invocation
KInvocation = kernel.Invocation

class Pipeline(tuple):
	"""
	Object holding the file descriptors of a running pipeline.
	"""
	__slots__ = ()

	@property
	def input(self):
		"Pipe file descriptor for the pipeline's input"
		return self[0]

	@property
	def output(self):
		"Pipe file descriptor for the pipeline's output"
		return self[1]

	@property
	def process_identifiers(self):
		"The sequence of process identifiers of the commands that make up the pipeline"
		return self[2]

	@property
	def standard_errors(self):
		"Mapping of process identifiers to the standard error file descriptor."
		return self[3]

	def __new__(Class, input, output, pids, errfds, tuple=tuple):
		tpids = tuple(pids)
		errors = dict(zip(tpids, errfds))
		return super().__new__(Class, (
			input, output,
			tpids, errors,
		))

	def void(self, close=os.close):
		"Close all file descriptors and kill -9 all processes involved in the pipeline."
		for x in self.standard_errors:
			close(x)
		close(self[0])
		close(self[1])

		for pid in self.process_identifiers:
			kill(pid, 9)

class PInvocation(tuple):
	"""
	A sequence of &KInvocation instances used to form a pipeline for
	unix processes; a process image where the file descriptors 0, 1, and 2
	refer to standard input, standard output, and standard error.

	Pipelines of zero commands can be created; it will merely represent a pipe
	with no standard errors and no process identifiers.
	"""
	__slots__ = ()

	from . import kernel
	Invocation = kernel.Invocation
	del kernel

	@classmethod
	def from_commands(Class, *commands):
		"""
		Create a &PInvcoation instance from a sequences of commands.

			pr = forklib.PInvocation.from_commands(('cat', 'somefile'), ('process', '--flags'))
		"""
		return Class([
			Class.Invocation(path, args)
			for path, *args in commands
		])

	@classmethod
	def from_pairs(Class, commands):
		"""
		Create a Pipeline Invocation from a sequence of process-path and process-arguments
		pairs.

			pr = forklib.PInvocation.from_pairs([("/bin/cat", ("file", "-")), ...])
		"""
		return Class([Class.Invocation(*x) for x in commands])

	def __call__(self, pipe=os.pipe, close=os.close):
		"""
		Execute the series of invocations returning a &Pipeline instance containing
		the file descriptors used for input, output and the standard error of all the commands.
		"""
		n = len(self)

		# one for each command, split read and write ends into separate lists
		stderr = []
		pipes = []

		try:
			for i in range(n):
				stderr.append(pipe())

			errors = [x[0] for x in stderr]
			child_errors = [x[1] for x in stderr]

			# using list.append instead of a comprehension
			# so cleanup can be properly performed in the except clause
			for i in range(n+1):
				pipes.append(pipe())

			# first stdin (write) and final stdout (read)
			input = pipes[0][1]
			output = pipes[-1][0]

			pids = []
			for i, inv, err in zip(range(n), self, child_errors):
				pid = inv(((pipes[i][0], 0), (pipes[i+1][1], 1), (err, 2)))
				pids.append(pid)

			return Pipeline(input, output, pids, errors)
		except:
			# Close file descriptors that were going to be kept given the
			# success; the finally clause will make sure everything else is closed.
			close(pipes[0][1])
			close(pipes[-1][0])

			# kill any invoked processes
			for pid in pids:
				os.kill(pid, 9)
				os.waitpid(pid)
			raise
		finally:
			# fd's inherited in the child processes will
			# be unconditionally closed.
			for r, w in stderr:
				close(w)

			# special for the edges as the process is holding
			# the reference to the final read end and
			# the initial write end.
			if self:
				close(pipes[0][0])
				close(pipes[-1][1])

				# the middle range is wholly owned by the child processes.
				for r, w in pipes[1:-2]:
					close(r)
					close(w)
