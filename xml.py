"""
# XML interface set for &..system.

# [ Properties ]

# /(&typing.Mapping)namespaces
	# The namespace label (schema module basename) associated with the namespace URI.
"""
import itertools
import typing
from ..xml import library as libxml
from . import schemas
from .library import Reference
from . import libfactor

namespaces = libxml.document.index_namespace_labels(schemas)

class Execute(libxml.document.Interface):
	"""
	# System invocation descriptor.

	# ! DEVELOPMENT: Pending
		# Composition of fields and environment settings from elements
		# is not yet supported.

	# ! DEVELOPMENT: Testing
		# There are few or no tests for many of the features implemented here.
	"""
	from .schemas import execute as schema
	schema = libfactor.package_inducted(schema) / 'pf.lnk'
	namespace = namespaces['execute']

	type = typing.NamedTuple(
		'ExecutionFrame', [
			('type', typing.Text),
			('abstract', typing.Text),

			('environment', typing.Mapping[str, str]),
			('alteration', str),

			('executable', str),
			('program_name', str),
			('parameters', typing.Sequence[typing.Tuple[str, str]]),
		]
	)

	@staticmethod
	def parameter(reference, element):
		atts = element.attrib

		# Probably generalize this, but only two choices.
		if 'literal' in atts:
			return element.attrib['literal']

		if 'environment' in atts:
			return Reference.environment(atts['environment'], None)

	@staticmethod
	def structure(document, reference=None):
		"""
		# Extract the pertinent data from an execution frame.
		"""

		global namespaces

		construct_param = Execute.parameter

		find = lambda x: document.find(x, namespaces)
		findall = lambda x: document.findall(x, namespaces)

		typ = document.attrib.get("type")
		doc = document.attrib.get("abstract")

		exe_element = find("execute:executable")
		exe = exe_element.attrib.get("path", None)
		exe_name = exe_element.attrib.get("program.name", None)

		env_element = find("execute:environment")
		alt = env_element.attrib.get('alteration')
		params = find("execute:parameters")

		env = {
			x.attrib["name"]: x.attrib["value"]
			for x in list(env_element.findall("execute:setting", namespaces))
		}

		defaults = {
			x.attrib["name"]: x.attrib["value"]
			for x in list(env_element.findall("execute:default", namespaces))
		}

		fields = [construct_param(None, x) for x in list(params)]

		struct = {
			'type': typ,
			'abstract': doc,

			'environment': env,
			'alteration': alt,

			'executable': exe,
			'program_name': exe_name,
			'parameters': fields,
		}
		if defaults:
			struct['defaults'] = defaults

		return struct

	@staticmethod
	def serialize(struct, encoding="ascii",
			namespace=namespaces['execute'],
			chain=itertools.chain.from_iterable
		):
		"""
		# Construct an XML configuration for a service execute.
		"""

		xmlctx = libxml.Serialization()
		alt = struct.get('alteration')

		env = xmlctx.element('environment',
			chain((
				chain(
					xmlctx.element('setting', None, ('name', k), ('value', v))
					for k, v in struct['environment'].items()
				),
				chain(
					xmlctx.element('default', None, ('name', k), ('value', v))
					for k, v in (
						struct['defaults'].items()
						if 'defaults' in struct
						else ()
					)
				),
			)),
			('alteration', alt)
		)

		exe = xmlctx.element('executable', (),
			('path', struct.get('executable'))
		)

		params = xmlctx.element('parameters',
			chain(
				xmlctx.element('field', None, ('literal', x))
				if not isinstance(x, Reference)
				else xmlctx.element('field', None,
					('environment', x.identifier),
					('default', x.default)
				)
				for x in struct['parameters']
			)
		)

		return xmlctx.root('frame',
			chain((env, exe, params)),
			('type', struct["type"]),
			('abstract', struct.get('abstract')),
			namespace = namespace,
		)
