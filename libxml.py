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

namespaces = libxml.index_namespace_labels(schemas)

class Spawn(typing.Final):
	"""
	System invocation descriptor.
	"""

	@staticmethod
	def structure(document):
		"""
		Extract a service's invocation configuration from an XML file.
		"""
		global namespaces
		find = lambda x: document.find(x, namespaces)

		typ = document.attrib["type"]
		exe_element = find("spawn:executable")
		exe = exe_element.attrib.get("path", None)

		env_element = find("spawn:environment")
		params = find("spawn:parameters")

		doc = document.attrib.get("abstract")

		env = {
			x.attrib["name"]: x.attrib["value"]
			for x in list(env_element)
		}

		fields = [x.attrib["literal"] for x in list(params)]

		struct = {
			'type': typ,
			'abstract': doc,
			'executable': exe,
			'environment': env,
			'parameters': fields,
		}

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

		env = xmlctx.element('environment',
			chain(
				xmlctx.element('setting', None, ('name', k), ('value', v))
				for k, v in struct['environment'].items()
			)
		)

		exe = xmlctx.element('executable', (),
			('path', struct.get('executable'))
		)

		params = xmlctx.element('parameters',
			chain(
				xmlctx.element('field', None, ('literal', x))
				for x in struct['parameters']
			)
		)

		return xmlctx.root('spawn',
			chain((env, exe, params)),
			('type', struct["type"]),
			('abstract', struct.get('abstract')),
			namespace = namespace,
		)
