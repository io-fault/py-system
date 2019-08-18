"""
# Referring to the Channel, .io.Ports.
# Test the Transfer of Ports, file descriptors.
"""
import os
import struct

from . import common
from ... import kernel
from ... import io

def test_io(test):
	J = io.Array()
	files = []
	try:
		files = [
			os.open('/dev/null', 0)
			for x in range(64)
		]

		channels = common.allocports(J)
		for x in channels:
			J.acquire(x)

		parent = channels[:2]
		child = channels[2:]

		test/parent[0].port.freight == "ports"
		test/parent[1].port.freight == "ports"
		test/child[0].port.freight == "ports"
		test/child[1].port.freight == "ports"

		buf = kernel.Ports.allocate(64)
		for x in range(64):
			buf[x] = files[x]

		parent[1].acquire(buf)
		with J:
			pass
		# cover the exhaustion case
		cbuf = kernel.Ports.allocate(32)
		child[0].acquire(cbuf)
		while not child[0].exhausted:
			with J:
				pass

		# It's okay provided it's not a bunch of invalids.
		# Closing the original FD should close whatever is here.
		test/set(cbuf) != set((-1,))

		# cover the EAGAIN case
		cbuf = kernel.Ports.allocate(64)
		child[0].acquire(cbuf)
		while not parent[1].exhausted:
			with J:
				pass

		while child[0].resource[0] == -1:
			with J:
				pass

		test/set(cbuf) != set((-1,))
		child[0].terminate()
		child[1].terminate()
		parent[0].terminate()
		parent[1].terminate()
		while J.resource:
			with J:
				pass
	finally:
		J.void()
		for x in files:
			os.close(x)

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
