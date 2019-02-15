# These are primarily for coverage.

def test_no_array(test):
	import sys

	kpath = '.'.join(__name__.split('.')[:-2]) + '.kernel'
	test.skip(kpath in sys.modules)

	import array
	import types

	t = types.ModuleType("array")
	try:
		sys.modules['array'] = t

		try:
			from .. import kernel
			test.fail("import did not raise expected error")
		except (ImportError, AttributeError):
			pass

		def err(*args):
			raise Exception("nothin")
		t.array = err

		try:
			from .. import kernel
			test.fail("import did not raise expected error")
		except Exception as exc:
			test/str(exc) == "nothin"
	finally:
		sys.modules['array'] = array

	with test.trap():
		from .. import kernel

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
