import os

from .. import library
from .test_kernel import perform_cat

class Trapped(Exception):
	"""
	# Exception Fixture for test_critical.
	"""
	pass

def test_PInvocation(test):
	data = b'data sent through a cat pipeline\n'
	for count in range(0, 16):
		s = library.PInvocation.from_commands(
			*([('/bin/cat', 'cat')] * count)
		)
		pl = s()
		out, status = perform_cat(pl.process_identifiers, pl.input, pl.output, data, *pl.standard_errors.values())
		test/out == data
		test/len(status) == count

	os.wait()

def test_critical(test):
	"""
	# &.library.critical
	"""
	global Trapped
	test/Trapped - Exception # sanity

	# Check that critical returns.
	# It's only fatal when an exception is raised.
	def return_obj(*args, **kw):
		return (args, kw)
	result = library.critical(None, return_obj, "positional", keyword='value')
	test/result[0] == ("positional",)
	test/result[1] == {"keyword":'value'}

	def raise_trap():
		global Trapped
		raise Trapped("exception")

	raised_called = False
	def raised(replacement):
		nonlocal raised_called
		raised_called = True
		replacement()

	# Interject is not being tested here, so override
	# it to derive the effect that we're looking for.
	original = library.interject
	try:
		library.interject = raised
		try:
			library.critical(None, raise_trap)
		except library.Panic as exc:
			test/exc.__cause__ / Trapped
		except:
			test.fail("critical did not raise panic")
		else:
			test.fail("critical did not raise panic")
	finally:
		library.interject = original

	test/raised_called == True

def test_interject(test):
	"""
	# Validate that interject manages to run a callable in a reasonable number of cycles.
	"""
	executed = False
	def call():
		nonlocal executed
		executed = True

	test/executed == False # sanity
	library.interject(call)
	for x in range(32):
		pass

	test/executed == True

if __name__ == '__main__':
	import sys; from ...development import libtest
	libtest.execute(sys.modules[__name__])
