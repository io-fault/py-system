"""
# Module finder for projects containing integrals.

# [ Properties ]
# /host/
	# Finder instance intended for resolving extensions and bytecode files for
	# loading by the host Python.
"""

import os
import sys
import importlib.machinery
import contextlib

from fault.routes import library as libroutes
from . import files
from . import identity

class IntegralFinder(object):
	"""
	# Select an integral based on the configured variants querying the connected factor paths.
	"""

	ModuleSpec = importlib.machinery.ModuleSpec
	ExtensionFileLoader = importlib.machinery.ExtensionFileLoader

	class Loader(importlib.machinery.SourceFileLoader):
		"""
		# Loader for compiled Python integrals.
		"""
		_compile = staticmethod(importlib._bootstrap_external._compile_bytecode)

		def __init__(self, bytecode, fullname, path):
			self._bytecode = bytecode
			self._source = path
			super().__init__(fullname, path)

		def exec_module(self, module):
			super().exec_module(module)
			module.__file__ = self._source
			module.__cache__ = self._bytecode

		def get_code(self, fullname):
			try:
				with open(self._bytecode, 'rb') as f:
					# It's the build system's job to make sure everything is updated,
					# so don't bother with headers used by Python's builtin caching system.
					bc = f.read()
			except FileNotFoundError:
				return super().get_code(fullname)

			return self._compile(bc, fullname, self._bytecode, source_path=self._source)

		def set_data(self, *args, **kw):
			raise NotImplementedError("integral modules may not be directly updated using the loader")

	def __init__(self,
			groups,
			python_bytecode_variants,
			extension_variants,
			integral_container_name='__f-int__'
		):
		"""
		# Initialize a finder instance for use with the given variants.
		"""
		self.routes = set()
		self.index = dict()
		self.groups = groups
		self.integral_container_name = integral_container_name
		self.python_bytecode_variants = python_bytecode_variants
		self.extension_variants = extension_variants

		# These (following) properties are not the direct parameters of &IntegralFinder
		# as it is desired that the configuration of the finder to be introspectable.
		# There is some potential for selecting a usable finder out of meta_path
		# using the above fields. The below fields are the cache used by &find_spec.

		self._ext = self._init_segment(groups, extension_variants)
		self._pbc = self._init_segment(groups, python_bytecode_variants)

	@staticmethod
	def _init_segment(groups, variants):
		from fault.project import library as libproject
		v = dict(variants)
		v['name'] = '{0}'

		segments = (libproject.compose(groups, v))
		final = segments[-1] + '.i'
		del segments[-1]

		leading = libroutes.Segment.from_sequence(segments)
		assert '{0}' in final # &groups must have 'name' in the final path identifier.

		return leading, final, final.format

	def connect(self, route):
		"""
		# Add the route to finder connecting its subdirectories for import purposes.

		# Similar to adding a filesystem path to Python's &sys.path.
		"""

		# Only package module roots are identified.
		dirs = route.subdirectories()
		roots = [(x.identifier, route) for x in dirs]
		self.index.update(roots)
		self.routes.add(route)

		return self

	def disconnect(self, route):
		"""
		# Remove a route from the finder's set eliminating any relevant index entries.
		"""

		keys = []
		for k, v in self.index:
			if v == route:
				keys.append(k)
		for k in keys:
			del self.index[k]
		self.routes.discard(route)

		return self

	@classmethod
	def invalidate_caches(self):
		pass

	def find_spec(self, name, path, target=None):
		"""
		# Using the &index, check for the presence of &name's initial package.
		# If found, the integrals contained by the connected directory will be
		# used to load either an extension module or a Python bytecode module.
		"""

		soa = name.find('.')
		if soa == -1:
			if name not in self.index:
				# Must be root package if there is no leading name.
				return None
			else:
				soa = len(name)

		prefix = name[:soa]
		if prefix not in self.index:
			return None

		root = self.index[prefix]

		ipath = name.split('.')
		route = root.extend(ipath)

		final = ipath[-1]
		leading, filename, fformat = self._pbc
		pkg = False

		if route.is_directory():
			# Alawys presume __init__.
			pysrc = route / '__init__.py'
			final = '__init__'
			origin = str(pysrc)
			pkg = True
			idir = route / self.integral_container_name
		else:
			idir = route.container / self.integral_container_name
			pysrc = route.suffix('.py')
			if pysrc.is_regular_file():
				# Definitely Python bytecode.
				origin = str(pysrc)
			else:
				# No Python, presume extension
				origin = None
				pysrc = None
				leading, filename, fformat = self._ext

		if pysrc is not None:
			cached = idir.extend(leading) / fformat(final)
			l = self.Loader(str(cached), name, str(pysrc))
			spec = self.ModuleSpec(name, l, origin=origin, is_package=pkg)
			spec.cached = str(cached)
		else:
			# It's not Python source, check for a C-API extension.
			cur = idir
			while not (cur/'extensions').exists():
				cur = cur.container
				if str(cur) == '/':
					return None
			segment = (cur >> idir)[1]
			rroute = (cur/'extensions').extend(segment)

			path = str(rroute.extend(leading)/fformat(final))

			l = self.ExtensionFileLoader(name, path)
			spec = self.ModuleSpec(name, l, origin=path, is_package=False)

		return spec
