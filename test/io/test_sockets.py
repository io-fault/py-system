"""
# Sockets interfaces.
"""
import struct
from ... import io

def test_sockets_rallocate(test):
	# leveraging knowledge of io.Sockets.rallocate
	# in real code, rallocate against the Sockets *instance*
	test/list(io.Sockets.rallocate(10)) == [-1] * 10
	sb = io.Sockets.rallocate(16)
	mv = memoryview(sb)

	test/sb[0] != 1 # should be -1, but anything aside from 1 is okay.
	# itemsize is 4, so use pack_into.
	struct.pack_into("i", mv, 0, 1)
	test/sb[0] == 1

def test_sockets_accept_filter(test):
	J = io.Array()
	try:
		s = J.rallocate("sockets://ip4", ('127.0.0.1', 0))
		J.acquire(s)
		with test/TypeError as exc:
			s.set_accept_filter()
		with test/TypeError as exc:
			s.set_accept_filter(None)
		with test/TypeError as exc:
			s.set_accept_filter("data_ready", None)
		test/s.set_accept_filter("data_ready") == None
	finally:
		J.void()

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
