from .. import core
from .. import libhazmat as library

def test_partial(test):
	return

	def func(param):
		return "meh" + param
	funcref = library.partial(func, "func")
	test/funcref().open() == "mehfunc"

	def raises(bar):
		raise Exception(bar)
	fooref = library.partial(raises, "msg")
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

	r = library.chain((g1, g2), core.contain(lambda: ('initial',)))
	test/r.open() == ('initial', 'g1', 'g2')

	# check for exception propagation
	astring = "STRING"
	r = library.chain((g1, g2), core.contain(lambda: astring))
	test/r.failed == True
	test/r[0] == ME
	test/r[0].arg == astring

def test_Delivery(test):
	D = library.Delivery()
	test/TypeError ^ D.commit
	receiver = []
	D.endpoint(receiver.append)
	pkg = 'pkg'
	D.send(pkg)
	test/receiver[0] == pkg

	# now where the package comes before the address
	D = library.Delivery()
	receiver = []
	pkg = 'pkg'
	D.send(pkg)
	test/TypeError ^ D.commit
	D.endpoint(receiver.append)
	test/receiver[0] == pkg

def test_Switch(test):
	s = library.Switch()
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
	q = library.EQueue()
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
	source = library.EQueue()
	terminal = library.EQueue()
	terminal.get(sink.append)

	terminal.fasten(source)
	source.put("data")
	test/["data"] == sink
	source.put("data2")
	terminal.get(sink.append)
	test/["data", "data2"] == sink

def test_Segments(test):
	import mmap
	closed = []

	class submap(mmap.mmap):
		def close(self, *args):
			nonlocal closed
			closed.append(id(self))
			super().close(*args)

	with open(__file__, 'rb') as f:
		data = f.read()

	def new():
		f = open(__file__, 'rb')
		m = submap(f.fileno(), 0, access=mmap.ACCESS_READ)
		return m

	# del iseg, s, seg
	del closed[:]
	m = new()
	cur = id(m)
	seg = library.Segments(m, size=512)
	iseg = iter(seg)
	s = next(iseg)
	test/seg.weaks << s
	del s, seg
	test/closed == []
	del iseg
	test/closed << cur

	# test that Segments()
	del closed[:]
	m = new()
	cur = id(m)
	seg = library.Segments(m, size=128)
	iseg = iter(seg)
	s = next(iseg)
	test/seg.weaks << s
	del seg, iseg
	# should not be closed
	test/closed == []
	# XXX: check referrers
	del s
	test/closed << cur

	# del s, seg, iseg
	del closed[:]
	m = new()
	cur = id(m)
	seg = library.Segments(m, size=64)
	iseg = iter(seg)
	s = list(iseg)
	test/set(seg.weaks) == set(s)
	del seg, iseg
	# should not be closed
	test/closed == []
	del s
	test/closed << cur

	# validate that range slicing is appropriate
	del closed[:]
	ba = bytearray()
	m = new()
	cur = id(m)
	seg = library.Segments(m, size=64)
	x = None
	for x in seg:
		ba += x
	test/closed == []
	del x
	del seg
	test/closed << cur
	test/data == ba

	# test empty weaks close and Segments.open path.
	del closed[:]
	class SSegments(library.Segments):
		MemoryMap=submap
	seg = SSegments.open(__file__)
	cur = id(seg.memory)
	del seg
	test/closed == [cur]

	# test empty weaks close and Segments.open path.
	del closed[:]
	seg = SSegments.open(__file__)
	cur = id(seg.memory)
	for x in seg:
		del x
		break
	del seg
	test/closed == [cur]

if __name__ == '__main__':
	import sys; from ...development import libtest
	libtest.execute(sys.modules[__name__])
