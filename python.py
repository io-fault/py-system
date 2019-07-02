"""
# Python Execution Context support functions and classes.
"""
import sys
import os
import collections
import pkgutil
import importlib

from ..time import types as timetypes
from ..routes import types as routes

from . import files

class Import(routes.Selector):
	"""
	# Route for Python imports.
	"""

	__slots__ = ('context', 'points',)

	@classmethod
	def from_context(Class):
		"""
		# Return a new &Import to the package containing the module that is executing
		# &from_context.
		"""
		f = sys._getframe()
		while f is not None and f.f_globals['__name__'] == __name__:
			f = f.f_back
		if f is not None:
			return Class.from_fullname(f.f_globals['__package__'])
		return None

	@classmethod
	def from_fullname(Class, module_path_string):
		"""
		# Given a valid `__name__`, return an &Import instance to that path.
		"""
		return Class.from_points(None, *module_path_string.split('.'))

	@classmethod
	def from_module(Class, module):
		"""
		# Given module containing a valid `__name__`, return an &Import
		# instance to that path.
		"""
		return Class(None, tuple(module.__name__.split('.')))

	@classmethod
	def from_points(Class, context, *points):
		rob = object.__new__(Class)
		rob.__init__(context, points)
		return rob

	@classmethod
	def from_attributes(Class, path, tuple=tuple):
		"""
		# Create a &Route and an attributes sequence based on the given &path such that
		# the &Route is the &real portion of the &path and text following real module
		# path is the sequence of attributes.
		"""
		R = Class.from_fullname(path)
		module = R.real()
		attributes = path[len(str(module))+1:]
		return module, tuple(attributes.split('.') if attributes else ())

	@classmethod
	def dereference(Class, path):
		o, a = Class.from_attributes(path)
		o = o.module(trap=False)
		for x in a:
			o = getattr(o, x)
		return o

	def __bool__(self):
		return any((self.context, self.points))

	def __str__(self):
		return self.fullname

	def __repr__(self):
		return '{0}.{1}.from_fullname({2!r})'.format(__name__, self.__class__.__name__, self.fullname)

	def __contains__(self, abs):
		"""
		# Whether or not the Path contains the given Pointer.
		"""
		return abs.points[:len(self.points)] == self.points

	def __getitem__(self, req):
		return self.__class__(self.context, self.points[req])

	@property
	def fullname(self):
		"""
		# Return the absolute path of the module Route; dot separated module names.
		"""
		# accommodate for Nones
		return '.'.join(self.absolute)

	@property
	def basename(self):
		"""
		# The module's name relative to its package; node identifier used to refer to the module.
		# Alias to &identifier.
		"""

		return self.identifier

	@property
	def package(self):
		"""
		# Return a &Route to the module's package.
		# If the &Route is referencing a package, return &self.

		# ! WARNING:
			# This will be a method.
		"""

		if self.is_container():
			return self

		return self.__class__(self.context, self.points[:-1])

	@property
	def root(self):
		return self.__class__(self.context, self.points[0:1])

	@property
	def loader(self):
		"""
		# The loader of the module.
		"""

		return self.spec().loader

	def spec(self, find_spec=importlib.util.find_spec):
		"""
		# The spec for loading the module.
		"""
		try:
			return find_spec(self.fullname)
		except Exception:
			return None

	def exists(self):
		"""
		# Whether or not the module exists inside the Python paths.
		# However, the module may not be importable.
		"""

		return (self.spec() is not None)

	def is_container(self, find_loader=pkgutil.find_loader):
		"""
		# Interrogate the module's loader as to whether or not it's a "package module".
		"""

		if self.spec() is None:
			return False

		fn = self.fullname
		return find_loader(fn).is_package(fn)
	is_package = is_container

	def real(self):
		"""
		# The "real" portion of the Import.
		# Greatest Absolute Route that *actually exists* on the file system.

		# &None if no parts are real, and &self if the entire route exists.
		"""

		# XXX: Performs import, ideally it could check for existence.
		x = self
		while x.absolute:
			if x.module() is not None:
				return x
			x = x.container

	def get_last_modified(self) -> timetypes.Timestamp:
		"""
		# Return the modification time of the module's file as a chronometry Timestamp.
		"""
		return self.file().get_last_modified()

	def stack(self):
		"""
		# Return a list of module objects. The first being the outermost package module, the
		# last being the module being pointed to, subject module, and the between being the
		# packages leading to the &self.
		"""

		x = self
		r = []

		while (x.context, x.points) != (None, ()):
			mod = x.module()
			if mod is not None:
				r.append(mod)
			x = x.container

		return r

	def scan(self, attribute):
		"""
		# Scan the &stack of modules for the given attribute returning a pair
		# containing the module and the object accessed with the &attribute.
		"""

		modules = self.stack()
		for x in modules:
			if attribute in x.__dict__:
				yield (x, x.__dict__[attribute])

	def floor(self, valids={'project', 'context'}, name='__factor_type__'):
		"""
		# Find the context or project factor for the given module.
		"""

		for (mod, value) in self.scan(name):
			if value in valids:
				return self.__class__.from_fullname(mod.__name__)

		return None # No outer modules with __factor_type__.

	def project(self):
		"""
		# Return the 'project' module of the &floor package.
		"""

		f = self.floor()
		if f is not None:
			if f.module().__factor_type__ == 'context':
				return (f/'context'/'project').module()
			else:
				return (f/'project').module()

	def anchor(self):
		"""
		# Anchor the &Import route according to the project's context.
		# This returns a new &Import instance whose &context is the &floor of the module.
		"""
		points = self.absolute
		project = self.floor()
		rel = points[len(project.points):]
		return self.__class__(project, tuple(rel))

	def module(self, trap=True, import_module=importlib.import_module):
		"""
		# Return the module that is being referred to by the path.
		"""

		try:
			return import_module(self.fullname)
		except Exception:
			if trap is True:
				return None
			else:
				raise

	def subnodes(self, iter_modules=pkgutil.iter_modules):
		"""
		# Return a pairs of sequences containing routes to the subnodes of the route.
		"""

		packages = []
		modules = []

		spec = self.spec()
		if spec.submodule_search_locations is not None:
			prefix = self.fullname

			module = self.module()
			if module is not None:
				path = getattr(module, '__path__', None) or spec.submodule_search_locations

				for (importer, name, ispkg) in iter_modules(path):
					path = '.'.join((prefix, name))
					ir = self.__class__.from_fullname(path)

					# Filter entries identified as being a Python module,
					# but are not regular files or do not exist.
					if ir.spec() is None or ir.file().type() != 'file':
						# This applies to package modules as well as
						# the __init__.py file should be available saving
						# a namespace loader.
						continue

					if ispkg:
						packages.append(ir)
					else:
						modules.append(ir)

		return packages, modules

	def tree(self, deque = collections.deque):
		"""
		# Return a package's full tree as a pair of lists.
		"""

		pkgs, mods = self.subnodes()
		tree = {}
		pkgsq = deque(pkgs)

		while pkgsq:
			pkg = pkgsq.popleft()
			sp, pm = pkg.subnodes()

			# extend output
			pkgs.extend(sp)
			mods.extend(pm)

			# process subpackages
			pkgsq.extend(sp)
		return pkgs, mods

	def file(self, from_path=files.Path.from_path, getattr=getattr):
		"""
		# Get the &files.Path instance pointing to the module's file.
		"""

		path = getattr(self.loader, 'path', None)
		if path is None:
			# NamespaceLoader seems inconsistent here.
			return None

		return from_path(path)

	def directory(self):
		"""
		# The package directory of the module.
		"""

		pkg = self.package
		return pkg.file().container

	def cache(self):
		"""
		# The (filename)`__pycache__` directory associated with the module's file.
		"""

		return self.directory() / '__pycache__'

class Reference(object):
	"""
	# Field reference for passing by reference.

	# Primarily used in cases where the origin of a value should be retained
	# for structural purposes or origin tracking.

	# ! DEVELOPMENT: Location
		# May not be the appropriate location for this class.
		# While an environment variable references are the primary use-case,
		# there are certainly others.

		# Also, &routes.Selector might be a more appropriate baseclass;
		# load instead of value, store for update/overwrite.
	"""
	__slots__ = ('type', 'container_get', 'identifier', 'default')

	@classmethod
	def environment(Class, identifier, default, get=os.environ.get):
		return Class('environment', get, identifier, default)

	@classmethod
	def strings(Class, iterator):
		"""
		# Process the iterator producing References or values such that
		# values are emitted directly and references are only emitted
		# if their determined value is not None.
		"""
		for x in iterator:
			if isinstance(x, Class):
				v = x.value()
				if v is not None:
					yield v.__str__()
			else:
				yield x.__str__()

	def __init__(self, type, get, identifier, default=None):
		self.type = type
		self.container_get = get
		self.identifier = identifier
		self.default = default

	def __str__(self):
		"""
		# Return the string form of the container's value for the configured
		# key when present, otherwise the configured default.

		# If the resolved value is &None, an empty string will be returned.
		"""
		v = self.container_get(self.identifier, self.default)
		if v is None:
			return ''
		else:
			return v.__str__()

	def item(self):
		"""
		# Return the &identifier - &value pair.
		"""
		i = self.identifier
		return (i, self.container_get(i, self.default))

	def items(self):
		"""
		# Return &item in plural form; iterator that always returns a single pair.
		"""
		yield self.item()

	def value(self):
		"""
		# Return the value of the container's entry using &identifier as the key.
		# If no key exists, &default will be returned.
		"""
		self.container_get(self.identifier, self.default)
