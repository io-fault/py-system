from . import common
from .. import kernel

def error_cases(test, dg, idx):
	with test/IndexError:
		dg[idx]

	with test/IndexError:
		dg.endpoint(idx)

	with test/IndexError:
		dg.set_endpoint(idx, kernel.Endpoint("ip4", ('127.0.0.1', 123)))

	with test/IndexError:
		dg.payload(idx)

def test_DatagramArray_errors(test):
	empty = kernel.DatagramArray("ip4", 0, 0)
	one = kernel.DatagramArray("ip4", 0, 1)
	error_cases(test, empty, 0)
	error_cases(test, empty, 1)

	with test/TypeError:
		kernel.DatagramArray(123)

	with test/TypeError:
		empty.payload("foo")

	with test/TypeError:
		empty.endpoint("foo")

	with test/TypeError:
		empty.set_endpoint("foo")

	with test/TypeError:
		one.set_endpoint(0, kernel.Endpoint("ip6", ("::1", 0)))

	with test/TypeError:
		one[::2]

	with test/TypeError:
		one[::-1]

	with test/TypeError:
		kernel.DatagramArray("local", 0, 0)

def test_DatagramArray(test):
	empty = kernel.DatagramArray("ip4", 0, 0)
	test/len(empty) == 0
	test/len(memoryview(empty)) == 0
	# slice of empty is empty
	test/(empty is empty[0:]) == True
	test/list(empty) == []

	dga = kernel.DatagramArray("ip4", 16, 3)

	test/len(dga[0:0]) == 0
	test/len(dga[1:1]) == 0
	test/len(dga[2:2]) == 0
	test/len(dga[2:1]) == 0
	test/len(dga[500:1000]) == 0
	test/len(dga[3:1000]) == 0

	test.isinstance(dga[0:0], kernel.DatagramArray)
	test.isinstance(dga[1:1], kernel.DatagramArray)
	test.isinstance(dga[2:2], kernel.DatagramArray)
	test.isinstance(dga[2:1], kernel.DatagramArray)
	test.isinstance(dga[500:1000], kernel.DatagramArray)
	test.isinstance(dga[3:1000], kernel.DatagramArray)

	dga.set_endpoint(0, kernel.Endpoint("ip4", ('127.0.0.2', 2323)))
	dga.set_endpoint(1, ('127.0.0.9', 3232))

	test/dga.endpoint(0) == kernel.Endpoint("ip4", ('127.0.0.2', 2323))
	test/dga.endpoint(1) == kernel.Endpoint("ip4", ('127.0.0.9', 3232))

	mv = dga.payload(0)
	test/len(mv) == 16
	mv[:] = b'1' * 16

	x = dga[-2]
	test/x[0] == dga.endpoint(1)
	test/x[1] == dga.payload(1)

	x = dga[-3]
	test/x[0] == dga.endpoint(0)
	test/x[1] == dga.payload(0)

	mv = dga.payload(1)
	test/len(mv) == 16
	mv[:] = b'2' * 16

	first = dga[:1]
	test/len(first) == 1
	test/first.endpoint(0) == kernel.Endpoint("ip4", ('127.0.0.2', 2323))
	test/first.payload(0) == dga.payload(0)

	second = dga[1:]
	test/len(second) == 2
	test/second.endpoint(0) == kernel.Endpoint("ip4", ('127.0.0.9', 3232))
	test/second.payload(0) == dga.payload(1)

	# subarray slices
	test/memoryview(empty) == memoryview(dga[0:0])
	test/memoryview(empty) == memoryview(dga[1:1])
	test/memoryview(empty) == memoryview(dga[2:2])

	test/len(empty) == len(second[0:0])
	test/len(empty) == len(second[1:1])
	test/len(empty) == len(second[2:2])

	empty = kernel.DatagramArray("ip6", 0, 0)
	test/len(empty) == 0
	test/len(memoryview(empty)) == 0

	# subarrays use the same space.
	five = kernel.DatagramArray("ip4", 64, 5)
	endpoints = [kernel.Endpoint("ip4", ("127.0.0.1", 7777 + i)) for i in range(5)]

	for x, e in zip(range(5), endpoints):
		five[x:x+1].set_endpoint(0, e)

	for x, e in zip(range(5), endpoints):
		test/five.endpoint(x) == e

def test_rallocate(test):
	J = kernel.Array()
	try:
		r, w = J.rallocate(('datagrams', 'ip4'), ('127.0.0.1', 0))
		test/r.transfer() == None
		test/w.transfer() == None
		test.isinstance(r, kernel.Datagrams)
		test.isinstance(w, kernel.Datagrams)
		test.isinstance(r.rallocate(2, 512), kernel.DatagramArray)
		test.isinstance(w.rallocate(2, 512), kernel.DatagramArray)
		with test/TypeError:
			r.rallocate("n")
		test/len(memoryview(r.rallocate(2, 512))) == len(memoryview(kernel.DatagramArray("ip4", 512, 2)))
		r.terminate()
		w.terminate()
	finally:
		J.void()

def test_Datagrams_transfer_one(test):
	J = kernel.Array()
	try:
		r, w = J.rallocate(('datagrams', 'ip4'), ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)
		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		dga.payload(0)[:6] = b'foobar'

		rdga = r.rallocate(1)
		r.acquire(rdga)
		w.acquire(dga)
		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/rdga.endpoint(0) == r.endpoint()
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'foobar'
	finally:
		J.void()

def test_Datagrams_transfer_one_of_two(test):
	J = kernel.Array()
	try:
		r, w = J.rallocate(('datagrams', 'ip4'), ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)
		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		dga.payload(0)[:6] = b'foobar'

		# trigger EAGAIN
		rdga = r.rallocate(2)
		r.acquire(rdga)
		w.acquire(dga)

		while True:
			with J:
				# break on r transfer
				if r in list(J.transfer()):
					break

		test/r.exhausted == False # one of two datagrams
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'foobar'
	finally:
		J.void()

def test_Datagrams_transfer_two_of_one(test):
	J = kernel.Array()
	try:
		r, w = J.rallocate(('datagrams', 'ip4'), ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)
		dga = w.rallocate(2)
		dga.set_endpoint(0, r.endpoint())
		dga.set_endpoint(1, r.endpoint())
		dga.payload(0)[:6] = b'foobar'
		dga.payload(1)[:6] = b'barfoo'

		rdga = r.rallocate(1)
		r.acquire(rdga)
		w.acquire(dga)

		while True:
			with J:
				# break on r transfer
				if r in list(J.transfer()):
					test/len(r.transfer()) == 1
					break

		test/r.exhausted == True # one of two datagrams
		test/rdga.endpoint(0) == r.endpoint()
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'foobar'

		rdga = r.rallocate(1)
		r.acquire(rdga)

		while True:
			with J:
				# break on r transfer
				if r in list(J.transfer()):
					test/len(r.transfer()) == 1
					break
		test/r.exhausted == True # one of two datagrams
		test/rdga.endpoint(0) == r.endpoint()
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'barfoo'

		test/w.exhausted == True # one of two datagrams
	finally:
		J.void()

def test_Datagrams_invalid(test):
	J = kernel.Array()
	try:
		r, w = J.rallocate(('datagrams', 'ip4'), ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)

		w.acquire(b'')
		while not w.exhausted:
			with J:
				pass
		test/w.exhausted == True

		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)
		r.acquire(bytearray(0))
		while not r.exhausted:
			with J:
				pass
		test/r.exhausted == True
	finally:
		J.void()

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
