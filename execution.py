"""
# System process execution interfaces and
# process global registry for controlling the composition of &Specification' for system process execution.

# [ Purpose ]

# Primarily, this process global registry is intended for test cases that need to allow the harness to
# override the execution methods for selecting the correct executable. However, general purpose usage
# is reasonable for formalizing system process execution.

# [ Interface ]

# The &select callable returns the handler for a given execution method &Identifier. This allows the
# command constructor to be cached for repeat use. The &prepare callable selects and invokes
# the method returning the invocation specification.

# [ Default Methods ]

# /(id)`python-script`/
	# Execute a Python script selected using a file system path.
# /(id)`python-module`/
	# Execute a Python module available in the &sys.path of the Python subprocess.
# /(id)`system-file`/
	# Execute an executable file selected using a file system path.
# /(id)`factor`/
	# Execute a factor available on the factor path. Once the factor's type is identified,
	# either (string)`system-file` or (string)`python-module` will be used.

# [ Types ]

# Symbolic names for annotations used by &.execution.

# /&Specification/
	# A pair containing a selector for the executable and the argument vector to be used
	# upon invocation. &typing.AnyStr is used to denote the possibility of mixed encoded
	# and decoded strings. Usage may require resolutions depending on the target execution interface.
# /&Vector/
	# An argument vector used to designate the parameters to a process.
	# A sequence of any string types; this is used from the perspective of a
	# calling process whose arguments may require encoding or conversion.
# /&Method/
	# Signature of a &Specification constructor.
# /&Identifier/
	# The hashable used to access a &Method.
"""
import sys
import os
import typing

Identifier = typing.Hashable
Vector = typing.Sequence[typing.AnyStr]
Specification = typing.Tuple[typing.AnyStr, Vector]
Method = typing.Callable[[typing.AnyStr, Vector], Specification]

from .kernel import Invocation as KInvocation # Public export.

def default_factor(index, path, arguments, name):
	from ..routes import library as libroutes
	ir = libroutes.Import.from_fullname(path)
	if ir.is_package():
		# presume composite
		from . import libfactor
		file = libfactor.selected(ir)
		return index.prepare("system-file", str(file), arguments, name)
	else:
		# presume python module
		return index.prepare("python-module", path, arguments, name)

def default_python_script(index, script, arguments, name=None):
	sysexe = sys.executable
	argv = [name or sysexe, script]
	argv.extend(arguments)
	return (sysexe, argv)

def default_python_module(index, module, arguments, name=None):
	sysexe = sys.executable
	argv = [name or sysexe, '-m', module]
	argv.extend(arguments)
	return (sysexe, argv)

def default_executable(index, executable, arguments, name=None):
	argv = [name or executable]
	argv.extend(arguments)
	return (executable, argv)

_defaults = (
	('python-script', default_python_script),
	('python-module', default_python_module),
	('executable', default_executable),
	('factor', default_factor),
)

# Essentially a domain type providing the semantics and type signatures for holding &Method's.
# A distinct class is used to allow re-use by execution contexts that may need to provide
# access to local overrides.
class Index:
	"""
	# Execution Method index associating a set of &Identifier's with their
	# corresponding &Method's.
	"""
	__slots__ = ('_get', '_storage', '__weakref__')

	def __init__(self, defaults=_defaults, Storage=dict):
		d = self._storage = Storage(defaults)
		self._get = d.get

	def fork(self, methods:typing.Set[typing.Tuple[Identifier,Method]]) -> "Index":
		"""
		# Duplicate the &Index instance &self and update it with the given &methods.
		"""
		new = self.__class__(defaults=self._storage.items())
		new._storage.update(methods)

	def install(self, method:Identifier, new:Method) -> Method:
		"""
		# Register &new as the handler for &method and return the previously installed &Method.
		# Returns the old &Method installed for &method identifier.
		"""
		old = self._get(method)
		self._storage[method] = new
		return old

	def select(self, method:Identifier) -> Method:
		"""
		# Return the &Specification constructor identified by &method.
		"""
		return self._get(method)

	def prepare(self,
			method:Identifier,
			executable:object,
			arguments:Vector,
			name:typing.Optional[typing.AnyStr]=None
		) -> Specification:
		"""
		# Construct a pair suitable for creating a &.library.KInvocation instance.
		"""
		return self._get(method)(self, executable, arguments, name)

# The root process global data.
root = Index()
select = root.select
prepare = root.prepare

from dataclasses import dataclass
@dataclass
class Delta(object):
	"""
	# Descriptor of change in a subprocess' state.
	"""
	event:(str) = None
	status:(int) = None
	core:(bool) = None

	@property
	def running(self):
		return self.event in ('none', 'continue')

	@property
	def exited(self):
		return self.event == 'exit'
	@property
	def cored(self):
		return self.event == 'exit' and self.core is True
	@property
	def continueed(self):
		return self.event == 'continue'
	@property
	def stopped(self):
		return self.event == 'stop'

def decode_process_status(
		status:int,
		wasexit = os.WIFEXITED,
		getstatus = os.WEXITSTATUS,

		wassignal = os.WIFSIGNALED,
		getsig = os.WTERMSIG,

		wasstopped = os.WIFSTOPPED,
		getstop = os.WSTOPSIG,

		wascontinued = os.WIFCONTINUED,

		wascore = os.WCOREDUMP,
	) -> Delta:
	"""
	# the process or SIGCHLD signals. This is an abstraction to &os.waitpid
	# and can only be used with child processes.

	# [ Parameters ]
	# /status/
		# The status code produced by &os.waitpid. (stat_loc)

	# [ Exceptions ]
	# /&ValueError/
		# Raised when the given status code could not be recognized.
	"""

	if wasexit(status):
		event = 'exit'
		code = getstatus(status)
		cored = wascore(status) or False
	elif wassignal(status):
		event = 'exit'
		code = - getsig(status)
		cored = wascore(status) or False
	elif wasstopped(status):
		event = 'stop'
		code = getstop(status) or 0
		cored = None
	elif wascontinued(status):
		event = 'continue'
		code = None
		cored = None
	else:
		raise ValueError("unrecognized process status") # Could not create &Delta

	return Delta(event, code, cored)

def reap(
		pid:int,
		waitpid = os.waitpid,
		options = os.WNOHANG | os.WUNTRACED,
	) -> typing.Optional[Delta]:
	"""
	# Transform pending process events such as exits into a &Delta describing
	# the event. Normally used to respond to process exit events in order to reap
	# the process or SIGCHLD signals. This is an abstraction to &os.waitpid
	# and can only be used with child processes.

	# [ Parameters ]
	# /pid/
		# The process identifier to reap.
	# /options/
		# Keyword parameter defaulting to `os.WNOHANG | os.WUNTRACED`.
		# This can be altered in cases where the flags are not desired.

	# [ Returns ]
	# /&Delta/
		# Core is &True or &False for exits, and &None in all other cases.
	# /&None/
		# No status was available due to error.
	"""

	try:
		rpid, code = waitpid(pid, options)
	except OSError:
		return None

	if (rpid, code) == (0, 0):
		return Delta('none', None, None)

	return decode_process_status(code)

del dataclass

def dereference(invocation:KInvocation, stderr=2, stdout=1):
	"""
	# Execute the given invocation collecting (system/file)`/dev/stdout` into a &bytes instance.
	# &dereference blocks until EOF is read from the created pipe and should only be used to
	# execute reliable processes.

	# If (system/signal)`SIGALRM` is not appropriate for timeouts, invocation should be managed
	# directly.

	# ! RELATED: &execute
	"""

	pid = None
	status = None
	eof = False
	data = b''
	read = os.read
	r, w = os.pipe()

	try:
		with open(os.devnull, 'rb+') as null: # stderr
			nfd = null.fileno()
			pid = invocation([(nfd, 0), (w, stdout), (nfd, stderr)])

		os.close(w)
		while True:
			# Blocking reads until EOF (empty read).
			new = read(r, 2048)
			if not new:
				eof = True
				break
			data += new
	finally:
		os.close(r)

		if pid:
			if not eof:
				os.kill(pid, 9)
			delta = reap(pid, options=0)

	return pid, delta.status, data

def effect(invocation:KInvocation):
	"""
	# Execute the given invocation collecting (system/file)`/dev/stderr` into a &bytes instance.
	# &effect blocks until EOF is read from the created pipe and should only be used to
	# execute reliable processes.

	# ! RELATED: &dereference
	"""

	return dereference(invocation, stderr=1, stdout=2) # Remap to collect stderr instead of out.

def perform(invocation:KInvocation) -> int:
	"""
	# Execute an &invocation waiting for the subprocess to exit before allowing
	# the thread to continue. The invocation will inherit the process' standard
	# input, output, and error.

	# Returns the status of the subprocess; negative if the process exited due to a signal.

	# ! RELATED: &effect
	"""

	pid = invocation(((0, 0), (1, 1), (2, 2)))
	return reap(pid, options=0).status

class Pipeline(tuple):
	"""
	# Structure holding the file descriptors associated with a *running* pipeline
	# of operating system processes. Returned by called &PInvocation instances to
	# provide access to the input and output pipes and the standard errors of each process.
	"""
	__slots__ = ()

	@property
	def input(self) -> int:
		"""
		# Pipe file descriptor for the pipeline's input.
		"""
		return self[0]

	@property
	def output(self) -> int:
		"""
		# Pipe file descriptor for the pipeline's output.
		"""
		return self[1]

	@property
	def process_identifiers(self):
		"""
		# The sequence of process identifiers of the commands that make up the pipeline.
		"""
		return self[2]

	@property
	def standard_errors(self) -> typing.Sequence[int]:
		"""
		# Mapping of process identifiers to the standard error file descriptor.
		"""
		return self[3]

	def __new__(Class,
			input:int, output:int,
			pids, errfds:typing.Sequence[int],
			tuple=tuple
		):
		tpids = tuple(pids)
		errors = dict(zip(tpids, errfds))
		return super().__new__(Class, (
			input, output,
			tpids, errors,
		))

	def void(self, close=os.close) -> None:
		"""
		# Close all file descriptors and kill -9 all processes involved in the pipeline.

		# Normally used in exception cases where the caller failed to construct an interface
		# to the running pipeline.
		"""
		for x in self.standard_errors:
			close(x)
		close(self[0])
		close(self[1])

		for pid in self.process_identifiers:
			kill(pid, 9)

class PInvocation(tuple):
	"""
	# A sequence of &KInvocation instances used to form a pipeline for
	# system processes; a process image where the file descriptors 0, 1, and 2
	# refer to standard input, standard output, and standard error.

	# Pipelines of zero commands can be created; it will merely represent a pipe
	# with no standard errors and no process identifiers.
	"""
	__slots__ = ()
	Invocation = KInvocation

	@classmethod
	def from_commands(Class, *commands):
		"""
		# Create a &PInvocation instance from a sequences of commands.

		#!/pl/python
			pr = libexec.PInvocation.from_commands(
				['/bin/cat', '/bin/cat', 'somefile'],
				['/usr/bin/tee', 'tee', 'duplicate-1', 'duplicate-2'],
			)

		# The command tuples must specify the absolute path to the executable
		# as the first item, the second item is the program's runtime name
		# that is accessible as the first argument. Often, the basename of the
		# command if it were invoked using (system:environment)`PATH` resolution.
		"""
		return Class([
			Class.Invocation(path, args)
			for path, *args in commands
		])

	@classmethod
	def from_pairs(Class, commands):
		"""
		# Create a Pipeline Invocation from a sequence of process-path and process-arguments
		# pairs.

		#!/pl/python
			pr = libexec.PInvocation.from_pairs([("/bin/cat", ("cat", "file", "-")), ...])
		"""
		return Class([Class.Invocation(*x) for x in commands])

	def __call__(self, signal=9, pipe=os.pipe, close=os.close, range=range) -> Pipeline:
		"""
		# Execute the series of invocations returning a &Pipeline instance containing
		# the file descriptors used for input, output and the standard error of all the commands.

		# [ Parameters ]
		# /signal/
			# The signal used to kill the process in case of an exception during spawn.
			# Defaults to (system/signal)`SIGKILL`.
		"""

		# one for each command, split read and write ends into separate lists
		stderr = []
		pipes = []
		pids = []
		n = self.__len__()

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

			for i, inv, err in zip(range(n), self, child_errors):
				pid = inv(((pipes[i][0], 0), (pipes[i+1][1], 1), (err, 2)))
				pids.append(pid)

			return Pipeline(input, output, pids, errors)
		except:
			# Close file descriptors that were going to be kept given the
			# success; the finally clause will make sure everything else is closed.
			if pipes:
				close(pipes[0][1])
				close(pipes[-1][0])

			# kill -9 any invoked processes.
			# There is no guarantee that the process will properly
			# terminate from closing the pipes, so try to ensure
			# that the process was cleaned up as well.
			for pid in pids:
				os.kill(pid, signal)
				os.waitpid(pid, 0) # After kill -9; during exception

			raise
		finally:
			# the middle range is wholly owned by the child processes.
			for r, w in pipes[1:-2]:
				close(r)
				close(w)

			# fd's inherited in the child processes will
			# be unconditionally closed.
			for r, w in stderr:
				close(w)

			# special for the edges as the process is holding
			# the reference to the final read end and the initial write end.
			if self:
				close(pipes[0][0])
				close(pipes[-1][1])

if __name__ == '__main__':
	method, target, *args = sys.argv[1:]
	print(root.prepare(method, target, args))
