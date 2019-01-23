"""
# Deprecating.
"""
import os

__shortname__ = 'libsys'

class Reference(object):
	"""
	# Field reference for passing by reference.

	# Primarily used in cases where the origin of a value should be retained
	# for structural purposes or origin tracking.

	# ! DEVELOPMENT: Location
		# May not be the appropriate location for this class.
		# While an environment variable references are the primary use-case,
		# there are certainly others.

		# Also, &libroutes.Route might be a more appropriate baseclass;
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
