from .. import libmemory as library

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
