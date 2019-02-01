"""
# Module finder and loader for Factored Projects.
"""
import importlib.machinery

from ..routes import library as libroutes
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
		# Loader for compiled Python integrals. Compiled modules are not checked
		# against the source unlike Python's builtin loader.
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
			# Factors being explicitly compiled, code objects
			# are stored directly without pycache headers.
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

		segments = (libproject.compose_integral_path(v, groups=groups))
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
		if (route/'Projects').exists():
			dirs = [route/x for x in (route/'Projects').get_text_content().split('\n')]
		else:
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
			# Presume __init__ first.
			pysrc = route / '__init__.py'
			final = '__init__'
			idir = route / self.integral_container_name

			if not pysrc.exists():
				for encpkg in ('context', 'category'):
					# Context Enclosure
					if (route/encpkg).exists():
						pysrc = route/encpkg/'root.py'
						final = 'root'
						idir = pysrc * self.integral_container_name
						break

			origin = str(pysrc)
			pkg = True
		else:
			idir = route.container / self.integral_container_name
			pysrc = route.suffix('.py')
			if pysrc.is_regular_file():
				# Definitely Python bytecode.
				origin = str(pysrc)
			else:
				# No Python, attempt extension at this point.
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

	@classmethod
	def create(Class, intention, rxctx=None):
		"""
		# Construct a standard loader selecting integrals with the given &intention.
		"""

		if rxctx is None:
			rxctx = identity.root_execution_context()
		sys, arc = rxctx

		bc = {
			'system': sys,
			'architecture': identity._python_architecture,
			'intention': intention,
		}

		ext = {
			'system': sys,
			'architecture': arc,
			'intention': intention,
		}

		g = [['system','architecture'],['name','intention']]

		return Class(g, bc, ext)

def activate(intention='debug'):
	"""
	# Install loaders for the (envvar)`FACTORPATH` products.
	"""
	import os

	sfif = IntegralFinder.create(intention)
	paths = os.environ.get('FACTORPATH', '').split(':')
	for x in paths:
		if not x:
			continue
		x = files.Path.from_absolute(x)
		sfif.connect(x)

	import sys
	sys.meta_path.insert(0, sfif)
