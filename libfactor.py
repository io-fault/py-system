"""
Factor support for Python extensions, archive files, shared objects, and executables.

[ Properties ]

/selections
	A mapping providing the selected role to use for the factor module.

/import_role
	The default role to import modules with.

/python_triplet
	The `-` separated strings representing the currently executing Python context.
	Used to construct directories for Python extension builds.
"""
import sys
import imp
import types
import importlib

from ..computation import libmatch
from ..routes import library as libroutes

def python_context(implementation, version_info, abiflags, platform):
	"""
	Construct the triplet representing the Python context for the platform.
	Used to define the construction context for Python extension modules.
	"""
	pyversion = ''.join(map(str, version_info[:2]))
	return '-'.join((implementation, pyversion + abiflags, platform))

# Used as the context name for extension modules.
python_triplet = python_context(
	sys.implementation.name, sys.version_info, sys.abiflags, sys.platform
)

selections = None

_factor_role_patterns = None
_factor_roles = None # exact matches

def select(module, role, context=None):
	"""
	Designate that the given role should be used for the identified &package and its content.

	&select should only be used during development or development related operations. Notably,
	selecting the role for a given package during the testing of a project.

	It can also be used for one-off debugging purposes where a particular target is of interest.
	"""
	global _factor_roles, _factor_role_patterns
	if _factor_roles is None:
		_factor_roles = {}

	if module.endswith('.'):
		path = tuple(module.split('.')[:-1])

		if _factor_role_patterns is None:
			_factor_role_patterns = libmatch.SubsequenceScan([path])
		else:
			x = list(_factor_role_patterns.sequences)
			x.append(path)
			_factor_role_patterns = libmatch.SubsequenceScan(x)

		_factor_roles[module[:-1]] = role
	else:
		# exact
		_factor_roles[module] = role

def role(module, role='optimal', context=None):
	"""
	Get the configured role for the given module path.
	"""
	global _factor_roles, _factor_role_patterns

	path = str(module)

	if _factor_roles is None:
		return role

	if path in _factor_roles:
		return _factor_roles[path]

	if _factor_role_patterns is not None:
		# check for pattern
		path = _factor_role_patterns.get(tuple(path.split('.')))
		return _factor_roles['.'.join(path)]

	return default_role

def canonical_name(route:libroutes.Import):
	"""
	Identify the canonical name of the factor.
	"""
	r = []
	add = r.append

	while route:
		mod = importlib.import_module(str(route))
		nid = mod.__dict__.get('__canonical__', route.identifier)
		add(nid)
		route = route.container

	r.reverse()
	return '.'.join(r)

def cache_directory(module, context, role, subject):
	"""
	Get the relevant context-role directory inside the associated
	(fs-directory)`__pycache__` directory.
	"""
	# Get a route to a directory in __pycache__.
	return libroutes.File.from_absolute(module.__file__).container / '__pycache__' / context / role / subject

def extension_access_name(name:str) -> str:
	"""
	The name, Python module path, that the extension module will be available at.

	Python extension module targets that are to be mounted to canonical full names
	are placed in packages named (python:identifier)`extensions`. In order to resolve
	the full name, the first `'.extensions.'` substring is replaced with a single `'.'`.

	For instance, `'project.extensions.capi_module'` will become `'project.capi_module'`.
	"""
	return '.'.join(name.split('.extensions.', 1))

def extension_composite_name(name:str) -> str:
	"""
	Given the name of a Python extension module, inject the identifier `'extension'`
	between the package and module's identifier.

	[ Effects ]
	/Product
		A string referring to a (module) composite factor.
	"""
	root = str(libroutes.Import.from_fullname(name).floor())
	return '.'.join((root, 'extensions', name[len(root)+1:]))

def package_directory(import_route:libroutes.Import):
	return import_route.file().container

def sources(factor:libroutes.Import, dirname='src', module=None):
	"""
	Return the &libroutes.File instance to the set of sources.
	"""
	global libroutes

	if module is not None:
		pkgdir = libroutes.File.from_absolute(module.__file__).container
	else:
		pkgdir = package_directory(factor)

	return pkgdir / dirname

def work(module:libroutes.Import, context:str, role:str):
	"""
	Return the work directory of the factor &module for the given &context and &role.
	"""
	path = package_directory(module) / context / role
	return libroutes.File(None, path.absolute)

def composite(factor:libroutes.Import):
	"""
	Whether the given &factor reference is a composite. &factor must be a real route.
	"""
	global sources

	if not factor.is_container():
		return False
	if factor.module().__dict__.get('__factor_type__') is None:
		return False
	if not sources(factor).exists():
		return False

	return True

def probe(module:types.ModuleType):
	return module.__factor_type__ == 'system.probe'

def reduction(composite:libroutes.Import, context=None, role=None, module=None):
	"""
	The reduction of the &composite.

	The file is relative to the Python cache directory of the package module
	identifying itself as a system module: (fs-path)`{context}/{role}/factor`.
	"""
	if module is not None:
		pkgdir = libroutes.File.from_absolute(module.__file__).container
	else:
		pkgdir = composite.file().container

	return pkgdir / '__pycache__' / context / role / 'factor'

def dependencies(factor:types.ModuleType):
	"""
	Collect and yield a sequence of dependencies identified by
	the dependent's presence in the module's globals.

	This works on the factor's module object as the imports performed
	by the module body make up the analyzed data.
	"""
	ModuleType = types.ModuleType

	for k, v in factor.__dict__.items():
		if isinstance(v, ModuleType) and getattr(v, '__factor_type__', None) is not None:
			yield v

def python_extension(module, probe_id='.'.join((__package__, 'probes', 'libpython'))):
	for x in module.__dict__.values():
		if isinstance(x, types.ModuleType) and x.__name__ == probe_id:
			return True
	return False
