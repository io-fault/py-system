"""
# Sockets interfaces.
"""
import struct
from ... import io
from ... import network

def test_sockets_accept_filter(test):
	J = io.Array()
	try:
		ep = network.Endpoint.from_ip4(('127.0.0.1', 0))
		s = io.alloc_service(network.service(ep))
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
