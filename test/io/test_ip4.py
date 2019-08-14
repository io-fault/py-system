import os
import errno
from ... import io
from ... import network
from . import common

localhost = ('127.0.0.1', 0)

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

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
