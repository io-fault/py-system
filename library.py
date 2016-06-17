"""
Main thread protection, thread primitives, and system process invocation interfaces.

&.library provides a framework for managing a process and the resources that interact
with the operating system. Notably, it provides access to POSIX atfork callbacks used
to manage the re-initialization of child processes.

[ Functions ]

/create_thread
	Create a new thread and run the given callable inside of it.

/create_lock
	Create a lock for mutual exclusion across threads.

/identify_thread
	When executed inside a thread, return the identifier
	of the running thread.

[ Properties ]

/process_signals
	Mapping of generalized signal names to signal identifiers.

/process_signal_names
	Mapping of signal identifiers to names.

/process_fatal_signals
	Set of signals that would cause an immediate exit if `SIG_DFL` were set.

/process_signal_identifier
	Mapping of signal identifiers to names used in POSIX header files.
"""
import sys
import os
import signal
import functools
import contextlib
import typing
import types

from . import kernel
from ..computation import library as libc

__shortname__ = 'libsys'

import _thread
create_thread = _thread.start_new_thread
create_lock = _thread.allocate_lock
identify_thread = _thread.get_ident

_main_thread_id = identify_thread() # Presume import of system.library occurs on main thread.

def interrupt_thread(tid, exception=None,
		setexc=kernel.interrupt,
		pthread_kill=signal.pthread_kill
	):
	"""
	Raise the given exception in the thread with the given identifier, &tid.

	The thread being interrupted will be signalled after the exception has been set.
	This helps ensure that system calls will not stop the exception from being raised
	in order to kill the thread.

	! WARNING:
		Cases where usage is appropriate is rare. Managing the interruption
		of threads in this fashion is only appropriate in certain applications.

	[ Parameters ]

	/tid
		The thread's low-level identifier to interrupt.
	/exception
		The exception that is raised in the thread.
	"""
	global Sever

	r =  setexc(tid, exception or Sever)
	pthread_kill(tid, 0) # interrupt system call if any.

	return r

def select_thread_frame(tid:int) -> types.FrameType:
	"""
	Select the frame of the thread's identifier.

	[Parameters]
	/tid
		Identifier of the thread returned by &create_thread or &identify_thread.
		Returns &None when the thread is not running.
	"""
	global sys
	return sys._current_frames().get(x)

def select_fabric(tids:typing.Sequence[int]) -> typing.Sequence[typing.Tuple[int, types.FrameType]]:
	"""
	Select a set of threads from the same snapshot of frames.
	"""
	global sys
	snapshot = sys._current_frames()
	return [
		(x, snapshot[x]) for x in tids
	]

# Lock held when &control is managing the main thread.
__control_lock__ = create_lock()

# Protects superfluous interjections.
__interject_lock__ = create_lock()
__interject_lock__.acquire() # released in Fork.trap()

# Call to identify if &control is managing the main thread.
controlled = __control_lock__.locked

# Maintained process identifier object. Do not change or delete.
current_process_id = os.getpid()

# Currently identified parent process.
parent_process_id = os.getppid()

# Intercontext Fork Lock
__fork_lock__ = create_lock()

# Add callables to be dispatched in the parent *before* a fork occurs.
# Useful for creating context independent parent-child connections.
fork_prepare_callset = set()

# Add callables to be dispatched in the parent after a fork call is performed.
# Each object in the set will be called with one parameter, the process id of the child.
fork_parent_callset = set()

# Add callables to be dispatched in the child after a fork call is performed.
# If &.library did not perform the (system:manual)`fork(2)` operation,
# these callables will *not* be ran.
fork_child_callset = set()

# Initial set of callables to run. These are run whether or not the fork operation
# was managed by &.library.
fork_child_cleanup = set()

getattr=getattr
# Normalized identities for signals.
process_signals = {
	'stop': signal.SIGSTOP,
	'istop': signal.SIGTSTP,
	'continue': signal.SIGCONT,
	'terminate' : signal.SIGTERM,
	'quit' : signal.SIGQUIT,
	'interrupt' : signal.SIGINT,
	'kill' : signal.SIGKILL,

	'terminal.query': getattr(signal, 'SIGINFO', None),
	'terminal.view': getattr(signal, 'SIGWINCH', None),

	'delta': signal.SIGHUP,
	'context': signal.SIGUSR1,
	'trip' : signal.SIGUSR2,

	'limit-cpu': signal.SIGXCPU,
	'limit-file.size': signal.SIGXFSZ,
	'profiler': signal.SIGPROF,
}

# Signal numeric identifier to Signal Names mapping.
process_signal_names = dict([(v, k) for k, v in process_signals.items()])

# Signals that *would* terminate the process *iff* SIG_DFL was set.
# Notably, this set is used to help preserve the appropriate exit code.
process_fatal_signals = {
	signal.SIGINT,
	signal.SIGTERM,
	getattr(signal, 'SIGXCPU', None),
	getattr(signal, 'SIGXFSZ', None),
	getattr(signal, 'SIGVTALRM', None),
	getattr(signal, 'SIGPROF', None),
	getattr(signal, 'SIGUSR1', None),
	getattr(signal, 'SIGUSR2', None),
}
process_fatal_signals.discard(None)

process_signal_identifiers = {
	getattr(signal, name): name
	for name in dir(signal)
	if name.startswith('SIG') and name[3] != '_' and isinstance(getattr(signal, name), int)
}
del getattr

def interject(main_thread_exec, replacement=True, signo=signal.SIGUSR2):
	"""
	Trip the main thread by sending the process a SIGUSR2 signal in order to cause any
	running system call to exit early. Used in conjunction with
	&kernel.interject
	"""
	global signal

	if replacement:
		# One interjection at a time if replacing.
		__interject_lock__.acquire()

	kernel.interject(main_thread_exec) # executed in main thread
	signal.pthread_kill(_main_thread_id, signo)

def clear_atexit_callbacks(pid = None):
	"""
	In cases where there may be process dependent callbacks, add this to the
	&fork_child_callset to clear the callbacks.
	"""
	global sys

	if 'atexit' in sys.modules:
		# It's somewhat uncommon to retain the forked process image,
		# so Python just leaves atexit alone. In the context of a fault.system
		# managed process, it is anticipated that it will exit normally and
		# fire the atexit callbacks which will be redundant with the parent.
		try:
			sys.modules['atexit']._clear()
		except:
			# XXX: Warning
			pass

##
# These are invoked by AddPendingCall.
def _after_fork_parent(child_pid):
	global __fork_lock__

	if not __fork_lock__.locked():
		# Don't perform related duties unless the fork() was managed by libsys.
		return

	try:
		for after_fork_in_parent_task in fork_parent_callset:
			after_fork_in_parent_task(child_pid)
	finally:
		__fork_lock__.release()

def _after_fork_child():
	global parent_process_id, current_process_id, __fork_lock__

	# Unconditionally update this data.
	parent_process_id = current_process_id
	current_process_id = os.getpid()

	if not __fork_lock__.locked():
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
			__fork_lock__.release()

class Sever(BaseException):
	"""
	Exception used to signal thread kills.
	"""
	__kill__ = True

class SystemExit(SystemExit):
	"""
	Extension of SystemExit for use with interjections.

	[ Properties ]

	/exiting_with_information
		Exit code indicating the type of information presented on standard error.
		Indicates that the standard error contains help output.

	/exiting_for_termination
		Exit code indicating that the daemon was signalled to shutdown
		by an administrative function.

	/exiting_for_restart
		Code used to communicate to the parent that it should be restarted.
		Primarily used by forking daemons to signal the effect of its exit
		without maintaining specific context.

		Parent processes should, naturally, restart the process when this
		code is used; usually it is used for automatic process cycling.

	/exiting_for_reduction
		Essentially exiting for termination, but gives a clear indicator
		about the purpose of the exit. Used by forking processes to
		indicate that the exit is purposeful and should essentially be ignored.

	/exiting_by_exception
		Code used to communicate that the process exited due to an exception.
		Details *may* be written standard error.
		Essentially, this is a runtime coredump.

	/exiting_by_signal_status
		&Invocation exit status code used to indicate that the
		process will exit using a signal during &atexit(2).
		The calling process will *not* see this code. Internal indicator.

	/exiting_by_default_status
		&Invocation exit status code used to indicate that the
		the process failed to explicitly note status.
	"""

	exiting_with_information = 200

	exiting_for_termination = 240
	exiting_for_restart = 241
	exiting_for_reduction = 242

	exiting_by_exception = 253
	exiting_by_signal_status = 254
	exiting_by_default_status = 255

	def raised(self):
		raise self

class Transition(object):
	"""
	A synchronization mechanism used to manage the transition
	of an arbitrary Container from one thread to another.

	Transitions are used by two threads in order to synchronize
	a single transfer.

	In terms of Python's threading library, Transitions would
	be the kind of synchronization mechanism used to implement
	&threading.Thread.join
	"""
	__slots__ = ('mutex', 'container')

	def __init__(self, mutex = create_lock):
		self.container = None
		# acquire prior to setting
		mtx = mutex()
		mtx.acquire()
		self.mutex = mtx

	def commit(self):
		"""
		Commit to the transition. If the object
		hasn't been placed, block until it is.

		A RuntimeError will be raised upon multiple invocations of commit.
		"""
		mutex = self.mutex
		if mutex is None:
			raise RuntimeError("transitioned")
		with mutex:
			self.mutex = None
			return self.container.open() # Thread Result

	def endpoint(self, container):
		"""
		Initiate the transition using the given container.
		"""
		mutex = self.mutex
		if mutex is None:
			raise RuntimeError("transitioned")
		self.container = container
		mutex.release()

	def relay(self, callable, *args, contain = libc.contain):
		return self.endpoint(contain(callable, *args))

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
	def system(Class, context=None, environ=(), isinstance=isinstance, str=str):
		"""
		Create an instance representing that of the invocation from the operating
		system. Primarily, information is retrieved from the &sys and &os module.
		"""
		global os, sys

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
		A means of exit used with a &Fork.trap managed process.
		"""
		global interject

		interject(SystemExit(exit_status).raised)

class Control(BaseException):
	"""
	Process control exceptions for significant events.

	This is a control exception inheriting from &BaseException. It should not be trapped.
	"""

	__kill__ = None

	def raised(self):
		raise self

class Panic(Control):
	"""
	An exception used to note the failure of a critical resource.

	Instances of this class are usually interjected into the main thread causing
	the process to immediately terminate.

	This is a control exception inheriting from &BaseException. It should not be trapped.
	"""

	__kill__ = True

class Interruption(Control):
	"""
	Similar to &KeyboardInterrupt, but causes &control to exit with the signal,
	and calls critical status hooks.

	Primarily used to cause signal exit codes that are usually masked with
	&KeyboardInterrupt.
	"""

	__kill__ = True

	def __init__(self, type, signo = None):
		self.type = type
		self.signo = signo

	def __str__(self):
		global process_signal_names
		global process_signal_identifier

		if self.type == 'signal':
			signame = process_signal_names.get(self.signo, 'unknown')
			sigid = process_signal_identifiers.get(self.signo, 'UNKNOWN-SIG')
			return "{1}[{2}]".format(signame, sigid, str(self.signo))
		else:
			return str(self.type)

	def raised(self):
		"""
		Register a system-level atexit handler that will cause the process to exit with
		the configured signal.
		"""
		global process_fatal_signals

		# if the noted signo is normally fatal, make it exit by signal.
		if self.signo in process_fatal_signals:
			# SIG_DFL causes process termination
			kernel.exit_by_signal(self.signo)

		return super().raised() # Interruption

	@classmethod
	def interrupt(Class, signo, frame):
		"""
		Signal handler that interjects an Interruption instance into the main thread.

		Default signal handler for SIGINT.
		"""
		global process_fatal_signals

		if signo in process_fatal_signals:
			interject(Class('signal', signo).raised) # fault.system.library.Interruption

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
		global process_fatal_signals
		stored_signals = {}

		try:
			for sig in process_fatal_signals:
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
	&Control exception used to signal &Fork.trap to replace the existing managed call.

	Usual case is that a &Fork.trap call is made on the main thread where other threads are
	created to run the actual program. Given that the program is finished and another should be ran
	*as if the current program were never ran*, the &Control exception can be raised in the
	main thread replacing the initial callable given to &Fork.trap.

	The exception should only be used through the provided classmethods.

	This exception should never be displayed in a traceback as it is intended to be caught
	by &Fork.trap.
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
		T.endpoint(libc.ContainedReturn((pid,)))
		if pid == 0:
			# In the child, raise the Fork() exception
			# to trigger pivot's replacement functionality.
			raise self

	@classmethod
	def substitute(Class, callable, *args, **kw):
		"""
		Substitute the existing control call with the given one.

		Immediately raises a &Fork instance in the calling thread to be caught
		by a corresponding &trap call.

		Only to be used in cases where it is known that the current frame stack is
		being managed by &trap.
		"""
		raise Class(callable, *args, **kw)

	@classmethod
	def dispatch(Class, controller, *args, **kw):
		"""
		Execute the given callable with the given arguments in a child process.
		This performs an &interject call. Given that &pivot was called to execute the
		program, the pivot function will catch the exception, in the child, and
		execute the replacement.

		[Parameters]

		/controller
			The object that will be called in the clone.
		/args
			Arguments given to the callable.
		/kw
			Keywords given to the callable.

		[Return]

		The child process' PID.
		"""
		global interject, identify_thread

		fcontroller = Class(controller, *args, **kw)

		# Don't bother with the interjection if we're dispatching from the main thread.
		# Usually, this doesn't happen, but it can be desirable to have fork's control
		# provisions in even simple programs.
		if Class.__controlled_thread_id__ == identify_thread():
			pid = os.fork()
			if pid == 0:
				raise fcontroller
			return pid
		else:
			# Not in the thread controlled by Fork.trap().

			# Transition is used because we're probably in a thread and we want to hold
			# until the fork occurs.
			T = Transition()
			transitioned_pivot = functools.partial(fcontroller.pivot, T)

			__fork_lock__.acquire() # Released by atfork handler.
			interject(transitioned_pivot, replacement=False) # fault.system.library.Fork.pivot

			# wait on commit until the fork() in the above pivot() method occurs in the main thread.
			return T.commit()
		raise RuntimeError("method branches did not return process identifier")

	@classmethod
	def trap(Class, controller, *args, **kw):
		"""
		Establish a point for substituting the process. Trap provides an
		exception trap for replacing the controlling stack. This is used to
		perform safe fork operations that allow tear-down of process specific resources
		in a well defined manner.

		! NOTE:
			Due to the varying global process state that may exist in a given process, it
			is often better to start a new Python instance.
		"""

		while True:
			Class.__controlled_thread_id__ = identify_thread()
			try:
				if not __interject_lock__.locked():
					raise Panic("interjection lock not configured")
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
	A Callable used to trap exceptions and interject a &Panic instance caused by the
	original.

	For example:

	#!/pl/python
		from fault.system.library import critical

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
			interject(raise_panic) # fault.system.library.critical
		else:
			raise ce

def protect(*init, looptime = 8):
	"""
	Perpetually protect the main thread.

	Used by &control to hold the main thread in &Fork.trap.
	"""
	global current_process_id, parent_process_id, process_signals

	from ..chronometry import kernel # kernel.sleep_us
	ltus = looptime * 1000000

	while 1:
		kernel.sleep_us(ltus) # main thread system call

		# Check for parent process changes.
		newppid = os.getppid()
		if newppid != parent_process_id:
			# Emit a context signal to the process.
			parent_process_id = newppid
			os.kill(os.getpid(), process_signals['context'])

	# Relies on Fork.trip() and kernel.interject to manage the main thread's stack.
	raise Panic("infinite loop exited")

def control(main, *args, **kw):
	"""
	A program that calls this is making an explicit declaration that signals should be
	defaulted and the main thread should be protected from uninterruptable calls to allow
	prompt process exits.

	The given &main is executed with the given positionals &args and keywords &kw inside
	of a &Fork.trap call. &Fork handles formal exits and main-call substitution.
	"""
	global kernel
	global Interruption, Fork
	global __control_lock__

	# Registers the atfork functions.
	kernel.initialize(sys.modules[__name__])

	with Interruption.trap(), __control_lock__:
		try:
			Fork.trap(main, *args, **kw)
			# Fork.trap() should not return.
			raise RuntimeError("libsys.Fork.trap did not raise SystemExit or Interruption")
		except Interruption as e:
			highlight = lambda x: '\x1b[38;5;' '196' 'm' + x + '\x1b[0m'
			sys.stderr.write("\r{0}: {1}".format(highlight("INTERRUPT"), str(e)))
			sys.stderr.flush()
			raise SystemExit(250)

def process_delta(
		pid:int,

		wasexit = os.WIFEXITED,
		getstatus = os.WEXITSTATUS,

		wassignal = os.WIFSIGNALED,
		getsig = os.WTERMSIG,

		wasstopped = os.WIFSTOPPED,
		getstop = os.WSTOPSIG,

		wascontinued = os.WIFCONTINUED,

		wascore = os.WCOREDUMP,

		waitpid = os.waitpid,
		options = os.WNOHANG | os.WUNTRACED,
	) -> typing.Tuple[str, int, typing.Union[bool, None.__class__]]:
	"""
	Transform pending process events such as exits into a triple describing
	the event. Normally used to respond to process exit events in order to reap
	the process or SIGCHLD signals.

	[ Parameters ]

	/pid
		The process identifier to reap.
		In cases of (system:signal)`SIGCHLD` events, the process-id associated
		with the received signal.

	[ Return ]

	A triple describing the event: `(event, status, core)`.

	The event is one of:

		- `'exit'`
		- `'signal'`
		- `'stop'`
		- `'continue'`

	The first two events mean that the process has been reaped and their `core` field will be
	&True or &False indicating whether or not the process left a process image
	behind. If the `core` field is &None, it's an authoritative statement that
	the process did *not* exit regardless of the platform.

	The status (code) is the exit status if an exit event, the signal number that killed or
	stopped the process, or &None in the case of `'continue'` event.
	"""

	try:
		_, code = waitpid(pid, options)
	except OSError:
		return None

	if wasexit(code):
		event = 'exit'
		status = getstatus(code)
		cored = wascore(code) or False
	elif wassignal(code):
		event = 'exit'
		status = - getsig(code)
		cored = wascore(code) or False
	elif wasstopped(code):
		event = 'stop'
		status = getstop(code) or 0
		cored = None
	elif wascontinued(code):
		event = 'continue'
		status = None
		cored = None

	return (event, status, cored)

def concurrently(controller, exe = Fork.dispatch):
	"""
	Dispatch the given controller in a child process of a system.library controlled process.
	The returned object is a reference to the result that will block until the child
	process has written the serialized response to a pipe.

	[ Parameters ]
	/controller
		The object to call to use the child's controller. &collections.Callable
	"""
	if not __control_lock__.locked():
		raise RuntimeError("main thread is not managed with libsys.control")

	rw = os.pipe()

	import io
	import pickle
	import atexit

	dump = pickle.dump
	load = pickle.load

	def execute_controller(call = controller, rw = rw):
		os.close(rw[0])
		try:
			atexit._clear()
			result = call()
		except SystemExit:
			result = None

		write = io.open(rw[1], 'wb')
		dump(result, write)
		write.close()
		raise SystemExit(0)

	# child never returns
	pid = exe(execute_controller)

	# Parent Only:
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
	Object holding the file descriptors associated with a running pipeline of operating system
	processes.
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
		Create a &PInvocation instance from a sequences of commands.

		#!/pl/python
			pr = libsys.PInvocation.from_commands(('cat', 'somefile'), ('process', '--flags'))
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

		#!/pl/python
			pr = libsys.PInvocation.from_pairs([("/bin/cat", ("file", "-")), ...])
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
