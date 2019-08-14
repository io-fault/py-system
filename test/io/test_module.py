"""
# Check various expectations.
"""

def test_module_protocol(test):
	from ... import io

	"Port" in test/dir(io)

	"Channel" in test/dir(io)
	"Octets" in test/dir(io)
	"Sockets" in test/dir(io)
	"Ports" in test/dir(io)
	"Array" in test/dir(io)

	test.issubclass(io.Octets, io.Channel)
	test.issubclass(io.Sockets, io.Channel)
	test.issubclass(io.Ports, io.Channel)
	test.issubclass(io.Array, io.Channel)

def test_no_subtyping(test):
	from ... import io

	types = (
		io.Array,
		io.Octets,
		io.Sockets,
		io.Ports,
	)

	for x in types:
		with test/TypeError as t:
			# Channel types extend the storage internally.
			# Discourage subtyping.
			# XXX: Channel can still be subclassed?
			class Foo(x):
				pass

def test_no_array(test):
	import sys

	kpath = '.'.join(__name__.split('.')[:-2]) + '.io'
	test.skip(kpath in sys.modules)

	import array
	import types

	t = types.ModuleType("array")
	try:
		sys.modules['array'] = t

		try:
			from ... import io
			test.fail("import did not raise expected error")
		except (ImportError, AttributeError):
			pass

		def err(*args):
			raise Exception("nothin")
		t.array = err

		try:
			from ... import io
			test.fail("import did not raise expected error")
		except Exception as exc:
			test/str(exc) == "nothin"
	finally:
		sys.modules['array'] = array

	with test.trap():
		from ... import io

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
