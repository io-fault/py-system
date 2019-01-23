import os

from .. import process as library

class Trapped(Exception):
	"""
	# Exception Fixture for test_critical.
	"""
	pass

def test_critical(test):
	"""
	# &.library.critical
	"""
	global Trapped
	test.issubclass(Trapped, Exception) # sanity

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
			test.isinstance(exc.__cause__, Trapped)
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
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
