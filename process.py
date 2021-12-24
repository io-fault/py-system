"""
# Process invocation and management interfaces.

# [ Elements ]

# /signal_codes/
	# Mapping of categorized signal names to signal (integer) codes.
# /signal_names/
	# Mapping of signal (integer) codes to categorized names.
# /signal_identifiers/
	# Mapping of signal codes to identifiers used in POSIX header files.
# /fatal_signals/
	# Set of signals that would cause an immediate exit if `SIG_DFL` were set.
"""
import sys
import os
import signal
import functools
import contextlib
import typing
import types
import builtins

from . import kernel
from . import runtime
from . import thread
from . import files

# Lock held when &control is managing the main thread.
__control_lock__ = thread.amutex()

# Protects superfluous interjections.
__interject_lock__ = thread.amutex()
__interject_lock__.acquire() # released in Fork.trap()

# Call to identify if &control is managing the main thread.
controlled = __control_lock__.locked

# Maintained process identifier object. Do not change or delete.
current_process_id = os.getpid()

# Currently identified parent process.
parent_process_id = os.getppid()

# Presume import occurs on main. Applications must update if incorrect.
main_thread_id = thread.identify()
index = {
	'root': ('thread', thread.identify(), None),
}

# Intercontext Fork Lock
__fork_lock__ = thread.amutex()

# Add callables to be dispatched in the parent *before* a fork occurs.
# Useful for creating context independent parent-child connections.
fork_prepare_callset = set()

# Add callables to be dispatched in the parent after a fork call is performed.
# Each object in the set will be called with one parameter, the process id of the child.
fork_parent_callset = set()

# Add callables to be dispatched in the child after a fork call is performed.
# If &.library did not perform the (system/manual)`fork(2)` operation,
# these callables will *not* be ran.
fork_child_callset = set()

# Initial set of callables to run. These are run whether or not the fork operation
# was managed by &.library.
fork_child_cleanup = set()

getattr=getattr
# Normalized identities for signals.
signal_codes = {
	'process/suspend': signal.SIGTSTP,
	'process/resume': signal.SIGCONT,
	'process/stop': signal.SIGSTOP,
	'process/continue': signal.SIGCONT,
	'process/terminate' : signal.SIGTERM,
	'process/quit' : signal.SIGQUIT,
	'process/interrupt' : signal.SIGINT,
	'process/kill' : signal.SIGKILL,

	'limit/cpu': signal.SIGXCPU,
	'limit/file': signal.SIGXFSZ,
	'limit/time': signal.SIGVTALRM,

	'terminal/stop': signal.SIGTSTP,
	'terminal/query': getattr(signal, 'SIGINFO', None) or getattr(signal, 'SIGUSR1', None),
	'terminal/delta': getattr(signal, 'SIGWINCH', None),
	'terminal/closed': signal.SIGHUP,
	'terminal/background-read': signal.SIGTTIN,
	'terminal/background-write': signal.SIGTTOU,

	'user/1': signal.SIGUSR1,
	'user/2': signal.SIGUSR2,

	'exception/floating-point': signal.SIGFPE,
	'exception/broken-pipe': signal.SIGPIPE,

	'error/invalid-memory-access': signal.SIGBUS,
	'error/restricted-memory-access': signal.SIGSEGV,
	'error/invalid-instruction': signal.SIGILL,
	'error/invalid-system-call': signal.SIGSYS,
}
signals = signal_codes

# Signal numeric identifier to Signal Names mapping.
signal_names = dict([(v, k) for k, v in signal_codes.items()])

# Signals that *would* terminate the process *if* SIG_DFL was set.
# Notably, this set is used to help preserve the appropriate exit code.
fatal_signals = {
	signal.SIGINT,
	signal.SIGTERM,
	getattr(signal, 'SIGXCPU', None),
	getattr(signal, 'SIGXFSZ', None),
	getattr(signal, 'SIGVTALRM', None),
	getattr(signal, 'SIGPROF', None),
	getattr(signal, 'SIGUSR1', None),
	getattr(signal, 'SIGUSR2', None),
}
fatal_signals.discard(None)

signal_identifiers = {
	getattr(signal, name): name
	for name in dir(signal)
	if name.startswith('SIG') and name[3] != '_' and isinstance(getattr(signal, name), int)
}
del getattr

def interject(main_thread_exec, replacement=True, signo=signal.SIGUSR2):
	"""
	# Trip the main thread by sending the process a SIGUSR2 signal in order to cause any
	# running system call to exit early. Used in conjunction with
	# &runtime.interject
	"""
	global signal

	if replacement:
		# One interjection at a time if replacing.
		__interject_lock__.acquire()

	runtime.interject(main_thread_exec) # executed in main thread
	signal.pthread_kill(main_thread_id, signo)

def clear_atexit_callbacks(pid = None):
	"""
	# In cases where there may be process dependent callbacks, add this to the
	# &fork_child_callset to clear the callbacks.
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

	if 'scheduler' in index:
		index.pop('scheduler', None)
		s = globals().pop('scheduler', None)
		if s is not None:
			try:
				s.void()
			except:
				sys.unraisablehook(*sys.exc_info())

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

class Exit(SystemExit):
	"""
	# Extension of SystemExit for use with interjections.
	"""

	# Default status for applications that did not specify an exit code.
	unspecified_status_code = 255

	# Proper exit code for --help invocations.
	# Used to explicitly declare that only usage information was emitted.
	usage_query_code = 200

	def raised(self):
		raise self

class Invocation(object):
	"""
	# A structure representing the invocation of a system process and the specification
	# of the means of exiting. Normally, used to describe how the process was invoked and the
	# corresponding parameters, argv and environ, in which the invocation should be reacting to.

	# For system invocation, the &parameters dictionary will have two entries by default:
	# `'system'` and `'type'`.

	# [ Elements ]
	# /context/
		# The Invocation that caused the creation of this &Invocation.
		# By default and when created from &system, this property is &None.
	# /parameters/
		# Arbitrary storage for the parameteres of the invocation. Usually, `'type'`, `'system'`,
		# and `'structured'` keys are present.
	"""
	context = None
	parameters = None

	def __init__(self, exit_method, context=None):
		self.exit_method = exit_method
		self.parameters = {}
		self.context = context

	def exit(self, result):
		"""
		# Perform the exit method designated during the initialization of the invocation.
		"""
		self.exit_method(result)

	@property
	def fs_pwd(self):
		"""
		# The working directory of the process when the &Invocation was created.
		"""
		return self.parameters['system']['directory']

	@property
	def argv(self):
		"""
		# Arguments provided by the system without the leading command name.
		"""
		return self.parameters['system']['arguments']
	args = argv

	@property
	def environ(self) -> dict:
		"""
		# The environment variables collected from the system during the creation of the instance.
		"""
		return self.parameters['system']['environment']

	def imports(self, envvars:typing.Iterable[str]):
		"""
		# Copy the given &envvars from &os.environ into the invocation's &parameters.
		# The snapshot of imported environment variables may be accessed using &environ.

		# [ Parameters ]
		# /envvars/
			# The collection of variables to copy from the environment.
		"""

		try:
			env = self.parameters['system']['environment']
		except KeyError:
			env = self.parameters['system']['environment'] = {}

		for x in envvars:
			if x not in env:
				env[x] = os.environ.get(x)

	@classmethod
	def system(Class, context=None, environ=(), isinstance=isinstance, str=str):
		"""
		# Create an instance representing that of the invocation from the operating
		# system. Primarily, information is retrieved from the &sys and &os module.

		# [ Parameters ]
		# /context/
			# A reference context identifying the &Invocation caused this invocation to be created.
		# /environ/
			# Sequence declaring the environment variables to acquire a snapshot of.
		"""
		r = Class(Class.system_exit_method, context = context)
		r.parameters['type'] = 'system'
		r.parameters['structured'] = None

		system = r.parameters['system'] = {}
		system['name'] = sys.argv[0]
		system['arguments'] = sys.argv[1:]
		system['directory'] = fs_pwd()
		system['environment'] = {}

		if environ:
			# Copy environment variables of interest.
			local = system['environment']
			for x in environ:
				if not isinstance(x, str):
					x = str(x)
				if x in os.environ:
					local[x] = os.environ[x]

		r.exit_method = Class.system_exit_method

		return r

	@classmethod
	def system_exit_method(Class, exit_status):
		"""
		# A means of exit used with a &Fork.trap managed process.

		# Injecting the exception on the main thread, this can also
		# be used within regular Python processes.
		"""
		interject(Exit(exit_status).raised)

class ControlException(BaseException):
	"""
	# Process control exceptions for significant events.

	# This is a control exception inheriting from &BaseException.
	# It should not be directly trapped, but subclasses may use it to classify
	# exceptions that are not necessarily indicative of a failure.
	"""

	__kill__ = None

	def raised(self):
		raise self

class Critical(ControlException):
	"""
	# An exception used to communicate that a fatal condition was identified.
	"""

	__kill__ = True

def panic(message, context=None, /, cause=None):
	"""
	# Raise a &Critical exception in the main thread forcing the process to exit.
	"""
	from .runtime import interject
	from signal import pthread_kill

	crit = Critical(message)
	crit.__cause__ = cause

	# Raise &crit in main thread.
	interject(crit.raised)
	# Only executed if not in the main thread.
	pthread_kill(main_thread_id, 0)

class Interruption(ControlException):
	"""
	# Similar to &KeyboardInterrupt, but causes &control to exit with the signal,
	# and calls critical status hooks.

	# Primarily used to cause signal exit codes that are usually masked with
	# &KeyboardInterrupt.
	"""

	__kill__ = True

	def __init__(self, type, signo = None):
		self.type = type
		self.signo = signo

	def __str__(self):
		global signal_names
		global signal_identifier

		if self.type == 'signal':
			signame = signal_names.get(self.signo, 'unknown')
			sigid = signal_identifiers.get(self.signo, 'UNKNOWN-SIG')
			return "{1}[{2}]".format(signame, sigid, str(self.signo))
		else:
			return str(self.type)

	def raised(self):
		"""
		# Register a system-level atexit handler that will cause the process to exit with
		# the configured signal.
		"""

		# if the noted signo is normally fatal, make it exit by signal.
		if self.signo in fatal_signals:
			# SIG_DFL causes process termination
			kernel.signalexit(self.signo)

		return super().raised() # Interruption

	@classmethod
	def interrupt(Class, signo, frame):
		"""
		# Signal handler that interjects an Interruption instance into the main thread.

		# Default signal handler for SIGINT.
		"""

		if signo in fatal_signals:
			interject(Class('signal', signo).raised) # fault.system.process.Interruption

	@staticmethod
	def void(signo, frame):
		"""
		# Python-level signal handler that does nothing.
		"""
		pass

	@classmethod
	def filter(Class, *sigs, signal = signal.signal):
		"""
		# Assign the void signal handler to the all of the given signal numbers.
		"""
		for x in sigs:
			signal(x, Class.void)

	@classmethod
	def catch(Class, *sigs, signal = signal.signal):
		"""
		# Assign the interrupt signal handler to the all of the given signal numbers.
		"""
		for x in sigs:
			signal(x, Class.interrupt)

	@classmethod
	@contextlib.contextmanager
	def trap(Class,
			catches=(signal.SIGINT, signal.SIGTERM),
			filters=(signal.SIGUSR1, signal.SIGUSR2),
			signal=signal.signal,
			ign=signal.SIG_IGN,
		):
		"""
		# Signal handler for a root process.
		"""
		stored_signals = {}

		try:
			for sig in fatal_signals:
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

class Fork(ControlException):
	"""
	# Exception used to signal &Fork.trap to replace the existing managed call.

	# Usual case is that a &Fork.trap call is made on the main thread where other threads are
	# created to run the actual program. Given that the program is finished and another should be ran
	# *as if the current program were never ran*, the &ControlException can be raised in the
	# main thread replacing the initial callable given to &Fork.trap.

	# The exception should only be used through the provided classmethods.

	# This exception should never be displayed in a traceback as it is intended to be caught
	# by &Fork.trap.
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
		T.transfer(pid)
		if pid == 0:
			# In the child, raise the Fork() exception
			# to trigger pivot's replacement functionality.
			raise self

	@classmethod
	def substitute(Class, callable, *args, **kw):
		"""
		# Substitute the existing control call with the given one.

		# Immediately raises a &Fork instance in the calling thread to be caught
		# by a corresponding &trap call.

		# Only to be used in cases where it is known that the current frame stack is
		# being managed by &trap.
		"""
		raise Class(callable, *args, **kw)

	@classmethod
	def dispatch(Class, controller, *args, **kw) -> int:
		"""
		# Execute the given callable with the given arguments in a child process.
		# This performs an &interject call. Given that &pivot was called to execute the
		# program, the pivot function will catch the exception, in the child, and
		# execute the replacement.

		# [Parameters]

		# /controller/
			# The object that will be called in the clone.
		# /args/
			# Arguments given to the callable.
		# /kw/
			# Keywords given to the callable.

		# [Returns]
		# /&int/
			# The child process' PID.

		# [Exceptions]
		# /&Critical/
			# Raised when the returns in the branches did not return.
		"""
		fcontroller = Class(controller, *args, **kw)

		# Don't bother with the interjection if we're dispatching from the main thread.
		# Usually, this doesn't happen, but it can be desirable to have fork's control
		# provisions in even simple programs.
		if Class.__controlled_thread_id__ == thread.identify():
			pid = os.fork()
			if pid == 0:
				raise fcontroller
			return pid
		else:
			# Not in the thread controlled by Fork.trap().

			# Transition is used because we're probably in a thread and we want to hold
			# until the fork occurs.
			T = thread.Transition()
			transitioned_pivot = functools.partial(fcontroller.pivot, T)

			__fork_lock__.acquire() # Released by atfork handler.
			interject(transitioned_pivot, replacement=False) # fault.system.process.Fork.pivot

			# wait on commit until the fork() in the above pivot() method occurs in the main thread.
			return T.commit()
		raise Critical("method branches did not return process identifier")

	@classmethod
	def trap(Class, controller, *args, **kw):
		"""
		# Establish a point for substituting the process. Trap provides an
		# exception trap for replacing the controlling stack. This is used to
		# perform safe fork operations that allow tear-down of process specific resources
		# in a well defined manner.

		# ! NOTE:
			# Due to the varying global process state that may exist in a given process, it
			# is often better to start a new Python instance.
		"""

		while True:
			Class.__controlled_thread_id__ = thread.identify()
			try:
				if not __interject_lock__.locked():
					raise Critical("interject lock not held")

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
	# A callable used to trap exceptions and interject a &Critical instance caused by the
	# original. This function is intended for critical sections where the failure is likely
	# to cause the application to become unresponsive via usual routes.

	# &critical may return, but must never raise.

	# For example:

	#!syntax/python
		from fault.system.process import critical

		def fun():
			while True:
				# # critical loop
				# # any exception causes the process to terminate
				...

		critical(None, fun)
	"""
	try:
		r = callable(*args, **kw)
		return r
	except BaseException as exc:
		ce = Critical("critical call raised exception")
		ce.__cause__ = exc

		if __control_lock__.locked():
			# Only use interject when the control() lock has been used.
			raise_panic = ce.raised
			interject(raise_panic) # fault.system.process.critical
		else:
			raise ce

def protect(*init, looptime=8):
	"""
	# Perpetually protect the main thread using a sleep loop that can only exit
	# using an interjection.

	# Used by &control to hold the main thread in &Fork.trap for applications
	# that rely on a set of threads to perform the actual work.

	# [ Exceptions ]
	# /&Critical/
		# Raised in cases where the infinite loop exits.

	# /&Exit/
		# Raised by an application thread using &interject.
	"""
	global current_process_id, parent_process_id

	from ..time import kernel # kernel.sleep_us
	ltus = looptime * 1000000

	while 1:
		kernel.sleep_us(ltus) # main thread system call; releases gil.

		# Check for parent process changes.
		newppid = os.getppid()
		if newppid != parent_process_id:
			# Emit a context signal to the process.
			parent_process_id = newppid
			os.kill(os.getpid(), signal_codes['user/1'])

	# Relies on Fork.trip() and runtime.interject to manage the main thread's stack.
	raise Critical("infinite loop exited") # interject should be used to raise process.Exit()

def control(main, *args, **kw):
	"""
	# A program that calls this is making an explicit declaration that signals should be
	# defaulted and the main thread should be protected from uninterruptable calls to allow
	# prompt process exits while still performing critical state restoration.

	# The given &main is executed with the given positionals &args and keywords &kw inside
	# of a &Fork.trap call. &Fork handles formal exits and main-call substitution.
	"""

	# Registers the atfork functions.
	kernel.initialize(sys.modules[__name__])

	with Interruption.trap(), __control_lock__:
		try:
			r = Fork.trap(main, *args, **kw)
			raise Exit(255) # Unspecified exit code.
		except Interruption as e:
			raise Exit(250)
		except SystemExit as exit:
			# Explicit exit request.
			# If associated with an exception, display using the installed hook.

			if exit.__context__:
				sys.stderr.write("Exit status was associated with exception context.\n")
				exc = exit.__context__
				sys.excepthook(exc.__class__, exc, exc.__traceback__)

			if exit.__cause__:
				sys.stderr.write("Exit status was associated with exception.\n")
				exc = exit.__cause__
				sys.excepthook(exc.__class__, exc, exc.__traceback__)

			raise
		except:
			# Exception caused exit.

			kernel.signalexit(signal.SIGUSR1) # Communicate exception.
			raise
		else:
			# Fork.trap() should not return.
			kernel.signalexit(signal.SIGUSR2)
			raise Critical("system.process.Fork.trap did not raise Exit or Interruption")

@contextlib.contextmanager
def timeout(duration=4, update=signal.alarm, signo=signal.SIGALRM):
	"""
	# (system/signal)`SIGALRM` based context manager for maximum time interruptions.
	"""

	try:
		prior = signal.signal(signo, Interruption.raised)
		update(duration)
		yield duration
	finally:
		update(0)
		signal.signal(signo, prior)

def concurrently(controller:typing.Callable, exe=Fork.dispatch, waitpid=os.waitpid):
	"""
	# Dispatch the given controller in a child process of a &control controlled process.
	# The returned object is a reference to the result that will block until the child
	# process has written the serialized response to a pipe.

	# Used to create *very simple* fork trees or workers that need to send completion reports back to
	# the parent. This expects the calling process to have been launched with &control.

	# [ Parameters ]

	# /controller/
		# The object to call to use the child's controller.
	"""
	if not __control_lock__.locked():
		raise RuntimeError("main thread is not managed with fault.system.process.control")

	rw = os.pipe()

	# XXX: Imports performed here as this is the only dependant in the module.
	# This should likely be relocated to another module.
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

		status = waitpid(pid, 0)
		if status_ref is not None:
			status_ref(status)

		return result

	return pid, read_child_result

def fs_pwd() -> files.Path:
	"""
	# Construct a &files.Path instance referring to (system/environ)`PWD` or
	# the current working directory if the environment variable is not defined
	# or it is an empty string.

	# The returned path is not maintained within any cache so repeat calls
	# will create a new instance.
	"""
	return files.Path.from_absolute(os.environ.get('PWD') or os.getcwd())

def fs_chdir(directory) -> files.Path:
	"""
	# Update (system/environ)`PWD` and the current working directory.

	# The current working directory is set prior to the environment being updated.
	# Exceptions should not require (system/environ)`PWD` to be reset by the caller.

	# [ Returns ]
	# The given &directory for chaining if it is a &files.Path instance.
	# Otherwise, the result of &fs_pwd is returned.
	"""
	path = str(directory)
	os.chdir(path)
	os.environ['PWD'] = path
	return directory if isinstance(directory, files.Path) else fs_pwd()

def _scheduler_loop(ks:kernel.Scheduler, proxy, limit, final):
	index['scheduler'] = ('thread', thread.identify(), ks)
	try:
		while not ks.closed:
			ks.wait(limit)
			ks.execute()

		# Execute remaining task.
		for i in range(final):
			if ks.execute() == 0:
				break
	finally:
		# Don't presume the key is still present,
		# application may have chosen to remove it
		# early for signalling purposes.
		if globals().get('scheduler', None) is proxy:
			index.pop('scheduler', None)
			globals().pop('scheduler', None)

def Scheduling(limit=16, final=32, loop=_scheduler_loop) -> kernel.Scheduler:
	"""
	# Initialize or return &.process.scheduler.

	# Accessing &.process.scheduler will perform this automatically.
	"""
	if 'scheduler' in globals():
		return scheduler

	from .kernel import Scheduler
	from weakref import proxy

	ks = Scheduler()

	p = globals()['scheduler'] = proxy(ks)
	tid = thread.create(loop, (ks, p, limit, final))
	index['scheduler'] = ('thread', tid, p)

	return p

del _scheduler_loop
def __getattr__(name):
	if name == 'scheduler':
		return Scheduling()
	raise AttributeError(name)
