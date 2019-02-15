import errno
import os.path
import tempfile
from .. import library as lib
from .. import kernel
from . import common

def test_invalid_address(test):
	J = kernel.Array()
	try:
		with test/SystemError as exc: # XXX: should probably should this one
			J.rallocate('octets://local', 123)
		with test/TypeError as exc:
			J.rallocate('octets://local', ())
		with test/TypeError as exc:
			J.rallocate('octets://local', (321,123))
	finally:
		J.void()

def test_endpoints(test):
	ep = kernel.Endpoint('local', ('/some/dir/', 'sock'))
	test/ep.port == 'sock'
	test/ep.interface == '/some/dir/'

	ep = kernel.Endpoint('local', ('/', 's'))
	test/ep.port == 's'
	test/ep.interface == '/'

	# XXX: currently silent trunc; throw error.
	dir = '/some/dir' * 70
	ep = kernel.Endpoint('local', (dir, 'sock'))
	test/ep.interface != dir
	test/ep.port != 'sock'

def test_array_rallocate(test):
	pairs = [
		('octets', 'local'),
		'octets://local',
	]
	singles = [
		('sockets', 'local'),
		'sockets://local',
	]

	J = kernel.Array()
	try:
		for x in pairs:
			t = J.rallocate(x, '/')
			for x in t:
				x.terminate()
				J.acquire(x)
		for x in singles:
			t = J.rallocate(x, '/')
			t.terminate()
			J.acquire(t)
	finally:
		J.terminate()
		with J:
			pass

def test_failure_on_bind(test, tri = 'sockets://local'):
	with tempfile.TemporaryDirectory() as d:
		J = kernel.Array()
		sf = J.rallocate(tri, (d, 'port'))
		J.acquire(sf)
		fail = J.rallocate(tri, (d, 'port'))

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

def test_io(test):
	with tempfile.TemporaryDirectory() as d:
		common.stream_listening_connection(test, 'local', (d, 's'))
		common.stream_listening_connection(test, 'local', os.path.join(d, 'y'))

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
