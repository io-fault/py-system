"""
Factor support for Python extensions, archive files, shared objects, and executables.

For composite factors that are not Python extensions, a context and role need to be
selected in order to get the appropriate output to use with the program.
&.system.libfactor provides the necessary configuration and functions for identifying
the appropriate files to use at runtime.

&.system.libfactor is canonical; &.development.libfpi uses it to identify where
targets should be processed and how to access them for implementation or installation.
"""
import sys
import types
import typing
import importlib

from ..routes import library as libroutes

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
	between the package and module's identifier giving the source factor of the
	extension module.

	[ Return ]

	/&*Annotation
		A string referring to a (module) composite factor.
	"""
	root = str(libroutes.Import.from_fullname(name).floor())
	return '.'.join((root, 'extensions', name[len(root)+1:]))

def package_directory(import_route:libroutes.Import) -> libroutes.File:
	return import_route.file().container

def inducted(factor:libroutes.Import, slot:str='factor') -> libroutes.File:
	"""
	Return the &libroutes.File instance to the inducted target.

	The selected construction context can designate that a different
	slot be used for inductance. This is to allow inspect output to reside
	alongside functioning output.

	[ Parameters ]
	/slot
		The inducted entry to use; defaults to `'factor'`, but
		specified for cases where the providing context inducts
		the data into another directory.
	"""
	return (factor.file().container / '__pycache__' / slot)

def package_inducted(modudle, slot:str='factor') -> libroutes.File:
	"""
	Return the &libroutes.File instance to the inducted factor.

	The selected construction context can designate that a different
	slot be used for inductance. This is to allow inspect output to reside
	alongside functioning output.

	[ Parameters ]
	/slot
		The inducted entry to use; defaults to `'factor'`, but
		specified for cases where the providing context inducts
		the data into another directory.
	"""
	c = libroutes.File.from_absolute(module.__file__).container
	return c / '__pycache__' / slot

def sources(factor:libroutes.Import, dirname='src', module=None):
	"""
	Return the &libroutes.File instance to the set of sources.
	"""
	global libroutes

	if module is not None:
		pkgdir = libroutes.File.from_absolute(module.__file__).container
		if module.__factor_type__ == 'python.extension':
			# Likely simulated composite. This is *not* a 'system.extension'.
			return pkgdir
	else:
		pkgdir = package_directory(factor)

	return pkgdir / dirname

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
	"""
	Whether the module is declared to be a system probe.
	"""
	return (
		module.__factor_type__ == 'system' and \
		module.__factor_dynamics__ == 'probe'
	)

def dependencies(factor:types.ModuleType) -> typing.Iterable[types.ModuleType]:
	"""
	Collect and yield a sequence of dependencies identified by
	the dependent's presence in the module's globals.

	This works on the factor's module object as the imports performed
	by the module body make up the analyzed data.

	[ Return ]

	/&*Annotation
		An iterable producing modules referenced by &factor that have
		explicitly defined the (python:attribute)`__factor_type__` name.
	"""
	ModuleType = types.ModuleType

	for k, v in factor.__dict__.items():
		if isinstance(v, ModuleType) and getattr(v, '__factor_type__', None) is not None:
			yield v

def python_extension(module) -> bool:
	"""
	Determine if the given package module represents a Python extension by
	analyzing its dependencies.
	"""
	for x in module.__dict__.values():
		if isinstance(x, types.ModuleType):
			if getattr(x, 'context_extension_probe', False):
				return True
	return False
