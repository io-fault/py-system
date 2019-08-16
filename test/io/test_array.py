"""
# Array qualities
"""
import time
import errno
import sys
import os

from ... import io

def test_array_already_acquired(test):
	try:
		J1 = io.Array()
		J2 = io.Array()
		rfd, wfd = os.pipe()
		r = J1.rallocate('octets://acquire/input', rfd)
		J1.acquire(r)
		with test/io.TransitionViolation as exc:
			J2.acquire(r)
	finally:
		J1.void()
		J2.void()

def test_array_rtypes(test):
	test/list(io.Array.rtypes()) != []

def test_array_set(test, count=128):
	"""
	# Sanity/smoke test validating the capacity to create some number of arrays.
	"""
	arrays = []
	try:
		for x in range(count):
			arrays.append(io.Array())
	finally:
		for x in arrays:
			test/x.terminated == False
			x.terminate()
			test/x.terminated == True
			with x:
				test/x.terminated == True
			test/x.terminated == True

def test_array_termination(test):
	J = io.Array()
	test/J.terminated == False

	J.terminate()
	test/J.terminated == True
	with J:
		pass

	test/J.terminated == True
	test/io.TransitionViolation ^ J.__enter__

	# No transition violation for double terminate.
	J.terminate()
	test/J.terminated == True

def test_array_exceptions(test):
	try:
		J = io.Array()
		with test/TypeError:
			J.resize_exoresource("foobar") # need an unsigned integer
		with test/TypeError:
			J.acquire("foobar") # not a channel
		r, w = J.rallocate("octets://spawn/unidirectional")
		r.terminate()
		w.terminate()
		J.acquire(r)
		J.acquire(w)
		with J:
			pass
		with test/io.TransitionViolation:
			J.acquire(r)
		with test/io.TransitionViolation:
			J.acquire(w)
	finally:
		J.void()

def test_array_terminated(test):
	try:
		J = io.Array()
		J.terminate()
		rfd, wfd = os.pipe()
		f = J.rallocate('octets://acquire/input', rfd)

		with J:
			pass

		with test/io.TransitionViolation:
			with J: pass

		with test/io.TransitionViolation:
			J.acquire(f)
		f.terminate()
	finally:
		J.void()

def test_array_force(test):
	J = io.Array()
	# once per second
	# this gives J a kevent
	J.force()
	start = time.time()
	with J:
		period = time.time() - start
		test/J.sizeof_transfer() == 0
	# three second wait time builtin
	test/period < 0.5 # XXX: high load could cause this to fail...
	J.terminate()
	with J:
		pass

def test_array_in_cycle(test):
	try:
		J = io.Array()
		J.force()
		test/J.port.exception() == None
		with J:
			with test/RuntimeError:
				J.resize_exoresource(20)
			with test/RuntimeError:
				with J: pass
	finally:
		J.void()

def test_array_out_of_cycle(test):
	'context manager to terminate on exit'
	try:
		J = io.Array()
		test/J.sizeof_transfer() == 0
		test/len(J.transfer()) == 0
		test/J.port.exception() == None

		r, w = J.rallocate('octets://spawn/unidirectional')
		J.acquire(w)
		J.acquire(r)
		w.acquire(b'')
		J.force()
		with J:
			i = J.transfer()
		with test/RuntimeError as exc:
			next(i)
	finally:
		J.terminate()
		with J:
			pass

def test_array_resize_exoresource(test):
	J = io.Array()
	try:
		J.force()
		with test/RuntimeError as exc, J:
			J.resize_exoresource(256)
	finally:
		J.terminate()
		with J:
			pass

def test_array_rallocate_octets(test):
	channels = set()
	try:
		J = io.Array()
		connection = J.rallocate(('octets', 'spawn', 'bidirectional'))
		three_four = J.rallocate(('octets', 'spawn', 'unidirectional'))
		channels.update(connection)
		channels.update(three_four)

		for x in connection + three_four:
			test.isinstance(x, io.Octets)
	finally:
		# don't leak
		for x in channels:
			x.terminate()
		J.terminate()

def test_array_new_failure(test):
	test.skip(sys.platform == 'linux')
	test.skip(not 'EOVERRIDE' in dir(io))
	try:
		io.EOVERRIDE['port_kqueue'] = lambda x: (errno.EINTR,)
		J = io.Array()
		test/J.port.error_code == errno.EINTR
		test/J.port.id == -1
	finally:
		io.EOVERRIDE.clear()

def test_array_resize_exoresource(test):
	try:
		J = io.Array()
		J.resize_exoresource(1)
		J.resize_exoresource(10)
		J.resize_exoresource(0)
		J.resize_exoresource(200)
	finally:
		J.void()

def test_array_rallocate_errors(test):
	J = io.Array()
	try:
		with test/LookupError as exc:
			J.rallocate("")
		with test/TypeError as exc:
			J.rallocate()
	finally:
		J.void()

def test_array_collection_countdown(test):
	J = io.Array()
	try:
		J.resize_exoresource(2)
		data = b'SOME DATA'
		bufs = []
		for x in range(6):
			channels = J.rallocate("octets://spawn/bidirectional")
			reads = channels[0::2]
			writes = channels[1::2]

			for y in writes:
				J.acquire(y)
				y.acquire(data)

			for t in reads:
				b = t.rallocate(len(data))
				bufs.append(b)
				t.acquire(b)
				J.acquire(t)

		# transfer until everything is exhausted
		while any((not x.exhausted for x in J.resource)):
			with J:
				pass
		for x in bufs:
			test/x == b'SOME DATA'
	finally:
		J.terminate()
		with J:
			pass

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
