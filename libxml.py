"""
XML interface set for &..system.

[ Properties ]

/(&typing.Mapping)namespaces
	The namespace label (schema module basename) associated with the namespace URI.
"""
import itertools
import typing
from ..xml import library as libxml
from . import schemas
from .library import Reference

namespaces = libxml.index_namespace_labels(schemas)

class Spawn(typing.Final):
	"""
	System invocation descriptor.

	! DEVELOPMENT: Pending
		Composition of fields and environment settings from elements
		is not yet supported.

	! DEVELOPMENT: Testing
		There are few or no tests for many of the features implemented here.
	"""

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
		Extract a service's invocation configuration from an XML file.
		"""

		global namespaces

		compose = Spawn.compose
		construct_param = Spawn.parameter

		find = lambda x: document.find(x, namespaces)
		findall = lambda x: document.findall(x, namespaces)

		typ = document.attrib.get("type")
		doc = document.attrib.get("abstract")

		exe_element = find("spawn:executable")
		exe = exe_element.attrib.get("path", None)
		exe_name = exe_element.attrib.get("program.name", None)

		env_element = find("spawn:environment")
		alt = env_element.attrib.get('alteration')
		params = find("spawn:parameters")

		env = {
			x.attrib["name"]: x.attrib["value"]
			for x in list(env_element.findall("spawn:setting", namespaces))
		}

		defaults = {
			x.attrib["name"]: x.attrib["value"]
			for x in list(env_element.findall("spawn:default", namespaces))
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
			namespace=namespaces['spawn'],
			chain=itertools.chain.from_iterable
		):
		"""
		Construct an XML configuration for a service spawn.
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
					for k, v in struct.get('defaults').items()
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

		return xmlctx.root('spawn',
			chain((env, exe, params)),
			('type', struct["type"]),
			('abstract', struct.get('abstract')),
			namespace = namespace,
		)
