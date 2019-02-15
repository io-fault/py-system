"""
# Referring to the Channel, traffic.kernel.Ports.

# Test the Transfer of Ports, file descriptors.
"""
import os
from .. import kernel

def test_io(test):
	J = kernel.Array()
	files = []
	try:
		files = [
			os.open('/dev/null', 0)
			for x in range(64)
		]

		transits = J.rallocate('ports://spawn/bidirectional')
		for x in transits:
			J.acquire(x)

		parent = transits[:2]
		child = transits[2:]

		test/parent[0].port.freight == "ports"
		test/parent[1].port.freight == "ports"
		test/child[0].port.freight == "ports"
		test/child[1].port.freight == "ports"

		buf = parent[1].rallocate(64)
		for x in range(64):
			buf[x] = files[x]

		parent[1].acquire(buf)
		with J:
			pass
		# cover the exhaustion case
		cbuf = child[0].rallocate(32)
		child[0].acquire(cbuf)
		while not child[0].exhausted:
			with J:
				pass

		# It's okay provided it's not a bunch of invalids.
		# Closing the original FD should close whatever is here.
		test/set(cbuf) != set((-1,))

		# cover the EAGAIN case
		cbuf = child[0].rallocate(64)
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
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
