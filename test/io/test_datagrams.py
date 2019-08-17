from . import common
from ... import io
from ... import network

service_alloc = network.Endpoint.from_ip4(('127.0.0.1', 0), 'udp', 'datagrams')
def mksrv():
	return io.alloc_datagrams(network.bind(service_alloc))

def error_cases(test, dg, idx):
	with test/IndexError:
		dg[idx]

	with test/IndexError:
		dg.endpoint(idx)

	with test/IndexError:
		dg.set_endpoint(idx, network.Endpoint.from_ip4(('127.0.0.1', 123)))

	with test/IndexError:
		dg.payload(idx)

def test_DatagramArray_errors(test):
	empty = io.DatagramArray("ip4", 0, 0)
	one = io.DatagramArray("ip4", 0, 1)
	error_cases(test, empty, 0)
	error_cases(test, empty, 1)

	with test/TypeError:
		io.DatagramArray(123)

	with test/TypeError:
		empty.payload("foo")

	with test/TypeError:
		empty.endpoint("foo")

	with test/TypeError:
		empty.set_endpoint("foo")

	with test/TypeError:
		one.set_endpoint(0, network.Endpoint.from_ip6(("::1", 0)))

	with test/TypeError:
		one[::2]

	with test/TypeError:
		one[::-1]

	with test/TypeError:
		io.DatagramArray("local", 0, 0)

def test_DatagramArray(test):
	empty = io.DatagramArray("ip4", 0, 0)
	test/len(empty) == 0
	test/len(memoryview(empty)) == 0
	# slice of empty is empty
	test/(empty is empty[0:]) == True
	test/list(empty) == []

	dga = io.DatagramArray("ip4", 16, 3)

	test/len(dga[0:0]) == 0
	test/len(dga[1:1]) == 0
	test/len(dga[2:2]) == 0
	test/len(dga[2:1]) == 0
	test/len(dga[500:1000]) == 0
	test/len(dga[3:1000]) == 0

	test.isinstance(dga[0:0], io.DatagramArray)
	test.isinstance(dga[1:1], io.DatagramArray)
	test.isinstance(dga[2:2], io.DatagramArray)
	test.isinstance(dga[2:1], io.DatagramArray)
	test.isinstance(dga[500:1000], io.DatagramArray)
	test.isinstance(dga[3:1000], io.DatagramArray)

	dga.set_endpoint(0, network.Endpoint.from_ip4(('127.0.0.2', 2323)))
	dga.set_endpoint(1, ('127.0.0.9', 3232))

	test/dga.endpoint(0) == network.Endpoint.from_ip4(('127.0.0.2', 2323))
	test/dga.endpoint(1) == network.Endpoint.from_ip4(('127.0.0.9', 3232))

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
	test/first.endpoint(0) == network.Endpoint.from_ip4(('127.0.0.2', 2323))
	test/first.payload(0) == dga.payload(0)

	second = dga[1:]
	test/len(second) == 2
	test/second.endpoint(0) == network.Endpoint.from_ip4(('127.0.0.9', 3232))
	test/second.payload(0) == dga.payload(1)

	# subarray slices
	test/memoryview(empty) == memoryview(dga[0:0])
	test/memoryview(empty) == memoryview(dga[1:1])
	test/memoryview(empty) == memoryview(dga[2:2])

	test/len(empty) == len(second[0:0])
	test/len(empty) == len(second[1:1])
	test/len(empty) == len(second[2:2])

	empty = io.DatagramArray("ip6", 0, 0)
	test/len(empty) == 0
	test/len(memoryview(empty)) == 0

	# subarrays use the same space.
	five = io.DatagramArray("ip4", 64, 5)
	endpoints = [network.Endpoint.from_ip4(("127.0.0.1", 7777 + i)) for i in range(5)]

	for x, e in zip(range(5), endpoints):
		five[x:x+1].set_endpoint(0, e)

	for x, e in zip(range(5), endpoints):
		test/five.endpoint(x) == e

def alloc(q):
	return io.DatagramArray("ip4", 512, q)

def test_Datagrams_transfer_one(test):
	J = io.Array()
	try:
		r, w = mksrv()
		J.acquire(r)
		J.acquire(w)
		ep = r.endpoint()

		dga = alloc(1)
		dga.set_endpoint(0, ep)
		dga.payload(0)[:6] = b'foobar'

		rdga = alloc(1)
		r.acquire(rdga)
		w.acquire(dga)
		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/rdga.endpoint(0) == ep
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'foobar'
	finally:
		J.void()

def test_Datagrams_transfer_one_of_two(test):
	J = io.Array()
	try:
		r, w = mksrv()
		J.acquire(r)
		J.acquire(w)
		ep = r.endpoint()
		dga = alloc(1)
		dga.set_endpoint(0, ep)
		dga.payload(0)[:6] = b'foobar'

		# trigger EAGAIN
		rdga = alloc(2)
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
	J = io.Array()
	try:
		r, w = mksrv()
		J.acquire(r)
		J.acquire(w)
		ep = r.endpoint()

		dga = alloc(2)
		dga.set_endpoint(0, ep)
		dga.set_endpoint(1, ep)
		dga.payload(0)[:6] = b'foobar'
		dga.payload(1)[:6] = b'barfoo'

		rdga = alloc(1)
		r.acquire(rdga)
		w.acquire(dga)

		while True:
			with J:
				# break on r transfer
				if r in list(J.transfer()):
					test/len(r.transfer()) == 1
					break

		test/r.exhausted == True # one of two datagrams
		test/rdga.endpoint(0) == ep
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'foobar'

		rdga = alloc(1)
		r.acquire(rdga)

		while True:
			with J:
				# break on r transfer
				if r in list(J.transfer()):
					test/len(r.transfer()) == 1
					break
		test/r.exhausted == True # one of two datagrams
		test/rdga.endpoint(0) == ep
		test/bytes(rdga.payload(0)).strip(b'\x00') == b'barfoo'

		test/w.exhausted == True # one of two datagrams
	finally:
		J.void()

def test_Datagrams_invalid(test):
	J = io.Array()
	try:
		r, w = mksrv()
		J.acquire(r)
		J.acquire(w)
		ep = r.endpoint()

		w.acquire(b'')
		while not w.exhausted:
			with J:
				pass
		test/w.exhausted == True

		dga = alloc(1)
		dga.set_endpoint(0, ep)
		w.acquire(dga)
		r.acquire(bytearray(0))
		while not r.exhausted:
			with J:
				pass
		test/r.exhausted == True
	finally:
		J.void()

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
