import os
import errno
from ... import io
from . import common

localhost = ('127.0.0.1', 0)

def test_invalid_address(test):
	try:
		J = io.Array()
		with test/SystemError as exc:
			J.rallocate('octets://ip4', 123)
		with test/TypeError as exc:
			J.rallocate('octets://ip4', ())
		with test/TypeError as exc:
			J.rallocate('octets://ip4', (1,))
		with test/TypeError as exc:
			J.rallocate('octets://ip4', (321, 'foo'))
		with test/TypeError as exc:
			J.rallocate('octets://ip4', (321, 'foo'))
		with test/OSError as exc:
			J.rallocate('octets://ip6', ('foobar.com', 123))
	finally:
		J.void()

def test_pton_error(test):
	test.skip('EOVERRIDE' not in dir(io))
	try:
		J = io.Array()
		with test/OSError as exc:
			try:
				io.EOVERRIDE['ip4_from_object'] = lambda x: (1,)
				J.rallocate('octets://ip4', ('127.0.0.1', 123))
			finally:
				io.EOVERRIDE.clear()
		test/exc().errno == 1
	finally:
		J.void()

def test_endpoints(test):
	ep = io.Endpoint('ip4', ('127.0.0.1', 100))
	test/ep.port == 100
	test/ep.interface == '127.0.0.1'

	ep = io.Endpoint('ip4', ('0.0.0.0', -1))
	test/ep.port == 0xFFFF
	test/ep.interface == '0.0.0.0'

def test_array_rallocate(test):
	pairs = [
		('octets', 'ip4'),
		('octets', 'ip4', 'udp'),
		('octets', 'ip4', 'tcp'),
		'octets://ip4',
		'octets://ip4:tcp',
		'octets://ip4:udp',
	]
	singles = [
		('sockets', 'ip4'),
		'sockets://ip4',
	]

	J = io.Array()
	try:
		for x in pairs:
			t = J.rallocate(x, (localhost[0], 1))
			J.acquire(t[0])
			J.acquire(t[1])
		for x in singles:
			t = J.rallocate(x, (localhost[0], 1))
			J.acquire(t)
	finally:
		J.terminate()
		with J:
			pass

def test_octets_datagram(test):
	am = common.ArrayActionManager()
	with am.thread():
		rw = am.array.rallocate(('octets', 'ip4', 'udp'), (localhost[0], 1))
		client = common.Endpoint(rw)
		with am.manage(client):
			pass

def test_unreachable(test):
	return
	am = common.ArrayActionManager()
	with am.thread():
		try:
			rw = am.array.rallocate('octets://ip4', ('126.4.4.247', 1))
			test/rw[0].port.error_code != 0
			test/rw[1].port.error_code != 0
		finally:
			test/rw[0].terminate()
			test/rw[1].terminate()

def test_io(test):
	common.stream_listening_connection(test, 'ip4', localhost)

def test_failure_on_bind(test, tri = 'sockets://ip4'):
	J = io.Array()
	sf = J.rallocate(tri, (localhost[0], 0))
	J.acquire(sf)

	port = sf.endpoint().port # zero port, OS selects available port
	fail = J.rallocate(tri, (localhost[0], port))

	port = fail.port
	test/port.call == "bind"
	test/port.error_code == errno.EADDRINUSE
	test/port.error_name == "EADDRINUSE"
	test/fail.terminated == False # hasn't been processed

	J.acquire(fail)
	J.force()
	with J:
		fail in test/list(J.transfer()) # expecting a terminate from fail
		test/fail.terminated == True

	# exercise code path
	x = str(port)
	x = port.error_description
	x = port._posix_description

	J.terminate()
	with J:
		pass
	test/J.terminated == True

def test_number_based_ip(test):
	return
	sstreams = [
		(1, 0),
		(None, 0)
	]

	# validate overflow occurs
	with test/OverflowError as exc:
		accept_stream_ip4((2**33, 0))

def test_ipaddress_objects(test):
	import ipaddress
	addr = (ipaddress.ip_address("127.0.0.1"), 0)

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
