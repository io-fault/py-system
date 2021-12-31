"""
# Check various expectations.
"""

def test_module_protocol(test):
	from ... import io

	"Port" in test/dir(io)

	"Channel" in test/dir(io)
	"Octets" in test/dir(io)
	"Array" in test/dir(io)

	test.issubclass(io.Octets, io.Channel)
	test.issubclass(io.Array, io.Channel)

def test_no_subtyping(test):
	from ... import io

	types = (
		io.Array,
		io.Octets,
	)

	for x in types:
		with test/TypeError as t:
			# Channel types extend the storage internally.
			# Discourage subtyping.
			# XXX: Channel can still be subclassed?
			class NotAllowed(x):
				pass

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
