"""
# Process global registry for controlling the composition of &Specification' for system process execution.

# This module is used by software that needs to allow the controlling process to determine how a system
# executable or Python module is invoked.

# [ Purpose ]

# Primarily, this module intended for test cases that need to allow the harness to override the execution
# methods for selecting the correct executable. However, general purpose usage is reasonable for
# formalizing system process execution.

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
import typing

Identifier = typing.Hashable
Vector = typing.Sequence[typing.AnyStr]
Specification = typing.Tuple[typing.AnyStr, Vector]
Method = typing.Callable[[typing.AnyStr, Vector], Specification]

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

if __name__ == '__main__':
	method, target, *args = sys.argv[1:]
	print(root.prepare(method, target, args))
