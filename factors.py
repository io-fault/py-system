"""
# Module finder and loader for Factored Projects.

# [ Engineering ]
# Currently, the finder can load extensions, but it will not properly
# modify paths in leading packages so that they can be found.
"""
import importlib.machinery

from . import files
from . import identity

from ..project import root

def compose_image_path(variants, default='void', groups=[["system", "architecture"], ["name"]]):
	"""
	# Create a variant path according to the given &groups and &variants.
	"""
	segments = []

	for g in groups[:-1]:
		fields = ([(variants.get(x) or default) for x in g])
		segment = '-'.join(fields)
		segments.append(segment)

	# Name must be '.' separated.
	fields = ([(variants.get(x) or default) for x in groups[-1]])
	segment = '.'.join(fields)
	segments.append(segment)

	return segments

class IntegralFinder(object):
	"""
	# Select an integral based on the configured variants querying the connected factor paths.
	"""

	suffixes = ['.py']
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
			module.__file__ = self._source
			module.__cache__ = self._bytecode

			spec = module.__spec__
			if spec.submodule_search_locations:
				module.__path__ = spec.submodule_search_locations

			super().exec_module(module)

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
		self.context = root.Context()
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
		from ..route.types import Segment
		v = dict(variants)
		v['name'] = '{0}'

		# polynomial-1
		segments = (compose_image_path(v, groups=groups))
		final = segments[-1] + '.i'
		del segments[-1]

		leading = Segment.from_sequence(segments)
		assert '{0}' in final # &groups must have 'name' in the final path identifier.

		return leading, final, final.format

	def connect(self, route:files.Path):
		"""
		# Add the route to finder connecting its subdirectories for import purposes.

		# Similar to adding a filesystem path to Python's &sys.path.
		"""

		pd = self.context.connect(route)
		self.index.update((x, pd) for x in map(str, pd.roots) if x not in self.index)
		return self

	def disconnect(self, route):
		"""
		# Remove a route from the finder's set eliminating any relevant index entries.
		"""

		keys = []
		pds = set()

		for k, v in self.index:
			if v.route == route:
				keys.append(k)
				pds.add(v)

		for k in keys:
			del self.index[k]

		for pd in pds:
			self.context.product_sequence.remove(pd)

		ids = set()
		for key, instance in self.context.instance_cache.items():
			typ, id = key
			if typ == 'project':
				pd = instance.product
			else:
				pd = instance

			if pd in pds:
				ids.add(key)

		for key in ids:
			del self.context.instance_cache[key]

		return self

	@classmethod
	def invalidate_caches(self):
		pass

	def find(self, name):
		"""
		# Retrieve the product with a root that matches the start of the given &name.
		"""
		soa = name.find('.')
		if soa == -1:
			if name not in self.index:
				# Must be root package if there is no leading name.
				return None
			else:
				soa = len(name)

		# Accessible modules are expected to be anchored to the product directory.
		prefix = name[:soa]
		if prefix not in self.index:
			return None

		return self.index[prefix]

	def find_spec(self, name, path, target=None):
		"""
		# Using the &index, check for the presence of &name's initial package.
		# If found, the integrals contained by the connected directory will be
		# used to load either an extension module or a Python bytecode module.
		"""

		# Project Protocols aren't used as they may require recursive imports.

		pd = self.find(name)
		if pd is None:
			return None

		route = pd.route + name.split('.')
		ftype = route.fs_type()
		parent = route.container

		final = route.identifier
		pkg = False

		if ftype == 'void':
			# Check for `extensions` factor.

			cur = parent
			while (cur/'extensions').fs_type() == 'void':
				cur = cur.container
				if str(cur) in (str(pd.route), '/'):
					# No extensions.
					break
			else:
				xpath = route.segment(cur)
				exts = cur/'extensions'
				ints = parent/self.integral_container_name
				rroute = exts//ints.segment(cur)
				extfactor = exts//xpath

				if extfactor.fs_type() != 'void':
					# .extension entry is present
					leading, filename, fformat = self._ext
					path = rroute//leading/fformat(final)

					path = str(path)
					l = self.ExtensionFileLoader(name, path)
					spec = self.ModuleSpec(name, l, origin=path, is_package=False)
					return spec

		if ftype == 'directory':
			# Not an extension; path is selecting a directory.
			pkg = True
			pysrc = route / '__init__.py'
			module__path__ = str(pysrc.container)
			final = '__init__'
			idir = route / self.integral_container_name

			if pysrc.fs_type() == 'void':
				# Handle context enclosure case.
				ctx = (route/'context')
				if ctx.fs_type() == 'void':
					return None

				module__path__ = str(route)
				pysrc = ctx/'root.py'
				final = 'root'
				idir = pysrc * self.integral_container_name
			else:
				module__path__ = str(pysrc.container)

			origin = str(pysrc)
		else:
			# Regular Python module or nothing.
			idir = parent / self.integral_container_name
			for x in self.suffixes:
				pysrc = route.suffix_filename(x)
				if pysrc.fs_type() == 'data':
					break
			else:
				# No recognized sources.
				return None

			module__path__ = str(pysrc.container)
			origin = str(pysrc)

		leading, filename, fformat = self._pbc
		cached = idir//leading/fformat(final)

		l = self.Loader(str(cached), name, str(pysrc))
		spec = self.ModuleSpec(name, l, origin=origin, is_package=pkg)
		spec.cached = str(cached)

		if pkg:
			spec.submodule_search_locations = [module__path__]

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

def activate(intention='debug', paths=None):
	"""
	# Install loaders for the (envvar)`FACTORPATH` products.
	"""
	global Activated
	import os

	sfif = IntegralFinder.create(intention)
	Activated = sfif
	if paths is None:
		paths = os.environ.get('FACTORPATH', '').split(':')

	for x in paths:
		if not x:
			# Ignore empty fields.
			continue
		x = files.Path.from_absolute(x)
		sfif.connect(x)

	import sys
	sys.meta_path.insert(0, sfif)
