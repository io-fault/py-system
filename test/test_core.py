from .. import core as library

def test_ContainedReturn(test):
	c = library.ContainedReturn((None,))
	test/c.failed == False
	test/None == c.contained
	test/None == c.open()
	test/None == c()

def test_ContainedException(test):
	class Foo(Exception):
		pass
	exc = Foo('bar')
	c = library.ContainedRaise((exc,None,(None,None)))
	test/c.failed == True
	test/exc == c.contained
	test/library.Containment ^ c.open
	test/library.Containment ^ c

def test_Contain(test):
	rob = object()
	def job():
		return rob
	c = library.contain(job)
	test/rob == c.open()

	class Foo(Exception):
		pass
	exc = Foo()
	def job():
		raise exc
	c = library.contain(job)
	test/c.contained / Foo
	test/c.contained == exc
	test/library.Containment ^ c.open
	test/library.Containment ^ c

def test_ContainedContainer(test):
	inner = library.Container((None,))
	outer = library.Container((inner,))
	outerouter = library.Container((outer,))
	test/True == (outerouter.shed() is inner)

if __name__ == '__main__':
	import sys; from ...development import libtest
	libtest.execute(sys.modules[__name__])
