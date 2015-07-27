from .. import libhazmat as lib
from .. import core

##
# Containment
##

def test_partial(test):
	return

	def foo(bar):
		return "meh" + bar
	fooref = lib.partial(foo, "foo")
	test/fooref().open() == "mehfoo"

	def raises(bar):
		raise Exception(bar)
	fooref = lib.partial(raises, "msg")
	test/fooref().failed == True
	test/fooref().exception() != None
	test/str(fooref().exception()) == "msg"

def test_chain(test):
	class MyException(Exception):
		pass
	ME = MyException("foo")

	def G1():
		arg = (yield None)
		arg = (yield arg+('g1',))
		ME.arg = arg
		raise ME
	g1 = G1()
	next(g1)
	def G2():
		arg = (yield None)
		arg = (yield arg+('g2',))
		arg = (yield "fin")
	g2 = G2()
	next(g2)

	r = lib.chain((g1, g2), core.contain(lambda: ('initial',)))
	test/r.open() == ('initial', 'g1', 'g2')

	# check for exception propagation
	astring = "STRING"
	r = lib.chain((g1, g2), core.contain(lambda: astring))
	test/r.failed == True
	test/r[0] == ME
	test/r[0].arg == astring

def test_Delivery(test):
	D = lib.Delivery()
	test/TypeError ^ D.commit
	receiver = []
	D.endpoint(receiver.append)
	pkg = 'pkg'
	D.send(pkg)
	test/receiver[0] == pkg

	# now where the package comes before the address
	D = lib.Delivery()
	receiver = []
	pkg = 'pkg'
	D.send(pkg)
	test/TypeError ^ D.commit
	D.endpoint(receiver.append)
	test/receiver[0] == pkg

def test_Switch(test):
	s = lib.Switch()
	cell = []
	def inc():
		cell.append(1)
	def dec():
		cell.append(-1)
	s.acquire(inc)
	test/sum(cell) == 1
	s.acquire(dec)
	s.release()
	test/sum(cell) == 0

def test_EQueue(test):
	q = lib.EQueue()
	events = []
	def getevent(x):
		events.append(('get', x))
	q.get(getevent)
	q.put("payload")
	test/events == [('get', 'payload')]
	test/q.backlog == 0
	q.put("payload")
	q.get(getevent)
	test/events == [('get', 'payload')]*2
	q.put("payload")
	q.put("payload")
	test/q.backlog == 2
	q.get(getevent)
	test/q.backlog == 1
	q.get(getevent)
	test/events == [('get', 'payload')]*4
	test/q.backlog == 0

	q.get(getevent)
	test/q.backlog == -1
	q.get(getevent)
	test/q.backlog == -2
	q.put("payload")
	test/q.backlog == -1
	q.put("payload")
	test/q.backlog == 0
	test/events == [('get', 'payload')]*6

def test_EQueue_fasten(test):
	sink = []
	source = lib.EQueue()
	terminal = lib.EQueue()
	terminal.get(sink.append)

	terminal.fasten(source)
	source.put("data")
	test/["data"] == sink
	source.put("data2")
	terminal.get(sink.append)
	test/["data", "data2"] == sink

if __name__ == '__main__':
	import sys; from ...development import libtest
	libtest.execute(sys.modules[__name__])
