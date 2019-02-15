# This file *should* contain traffic.kernel *specific* tests.
# Primarily, invasive tests that rely on implementation specific functionality.

# Arguably, there's quite a bit of redundancy in this file.
# However, cases analyzed here that appear similar often have one-off cases
# that make it rather annoying to generalize.
import time
import os
import socket
import struct
import errno
import sys
from .. import kernel
from .. import core

def test_junction_rtypes(test):
	test/list(kernel.Array.rtypes()) != []

def test_transit_already_acquired(test):
	try:
		J1 = kernel.Array()
		J2 = kernel.Array()
		r = J1.rallocate('octets://file/read', '/dev/null')
		J1.acquire(r)
		with test/kernel.TransitionViolation as exc:
			J2.acquire(r)
	finally:
		J1.void()
		J2.void()

def test_junction_termination(test):
	'termination sequence with no transits'
	J = kernel.Array()
	test/J.terminated == False

	J.terminate()
	test/J.terminated == True # termination must be noted *after* the cycle.
	with J:
		pass
	# one cycle in..
	test/J.terminated == True
	with test/kernel.TransitionViolation:
		with J: pass
	# validate no-op case
	J.terminate()
	test/J.terminated == True

def test_junction_exceptions(test):
	try:
		J = kernel.Array()
		with test/TypeError:
			J.resize_exoresource("foobar") # need an unsigned integer
		with test/TypeError:
			J.acquire("foobar") # not a transit
		r, w = J.rallocate("octets://spawn/unidirectional")
		r.terminate()
		w.terminate()
		J.acquire(r)
		J.acquire(w)
		with J:
			pass
		with test/kernel.TransitionViolation:
			J.acquire(r)
		with test/kernel.TransitionViolation:
			J.acquire(w)
	finally:
		J.void()

def test_junction_terminated(test):
	try:
		J = kernel.Array()
		J.terminate()
		f = J.rallocate('octets://file/read', '/dev/null')
		with J:
			pass

		with test/kernel.TransitionViolation:
			with J: pass

		with test/kernel.TransitionViolation:
			J.acquire(f)
		f.terminate()
	finally:
		J.void()

def test_junction_force(test):
	J = kernel.Array()
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

def test_junction_in_cycle(test):
	try:
		J = kernel.Array()
		J.force()
		test/J.port.exception() == None
		with J:
			with test/RuntimeError:
				J.resize_exoresource(20)
			with test/RuntimeError:
				with J: pass
	finally:
		J.void()

def test_junction_out_of_cycle(test):
	'context manager to terminate on exit'
	try:
		J = kernel.Array()
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

def test_junction_resize_exoresource(test):
	J = kernel.Array()
	try:
		J.force()
		with test/RuntimeError as exc, J:
			J.resize_exoresource(256)
	finally:
		J.terminate()
		with J:
			pass

def test_junction_rallocate_octets(test):
	transits = set()
	try:
		J = kernel.Array()
		connection = J.rallocate(('octets', 'spawn', 'bidirectional'))
		three_four = J.rallocate(('octets', 'spawn', 'unidirectional'))
		transits.update(connection)
		transits.update(three_four)

		for x in connection + three_four:
			test.isinstance(x, kernel.Octets)
	finally:
		# don't leak
		for x in transits:
			x.terminate()
		J.terminate()

def test_junction_new_failure(test):
	test.skip(sys.platform == 'linux')
	test.skip(not 'EOVERRIDE' in dir(kernel))
	try:
		kernel.EOVERRIDE['port_kqueue'] = lambda x: (errno.EINTR,)
		J = kernel.Array()
		test/J.port.error_code == errno.EINTR
		test/J.port.id == -1
	finally:
		kernel.EOVERRIDE.clear()

def test_junction_resize_exoresource(test):
	try:
		J = kernel.Array()
		J.resize_exoresource(1)
		J.resize_exoresource(10)
		J.resize_exoresource(0)
		J.resize_exoresource(200)
	finally:
		J.void()

def test_module_protocol(test):
	"Port" in test/dir(kernel)
	"Endpoint" in test/dir(kernel)

	"Channel" in test/dir(kernel)
	"Octets" in test/dir(kernel)
	"Sockets" in test/dir(kernel)
	"Ports" in test/dir(kernel)
	"Array" in test/dir(kernel)

	test.issubclass(kernel.Octets, kernel.Channel)
	test.issubclass(kernel.Sockets, kernel.Channel)
	test.issubclass(kernel.Ports, kernel.Channel)
	test.issubclass(kernel.Array, kernel.Channel)

def test_no_subtyping(test):
	types = (
		kernel.Array,
		kernel.Octets,
		kernel.Sockets,
		kernel.Ports,
	)

	for x in types:
		with test/TypeError as t:
			# Channel types extend the storage internally.
			# Discourage subtyping.
			# XXX: Channel can still be subclassed?
			class Foo(x):
				pass

def test_port(test):
	f = kernel.Port(
		call = "kevent", error_code = 10, id = -1,
	)
	test/f.id == -1
	test/f.error_code == 10
	test/f.error_name == "ECHILD"
	test/f.call == "kevent"

	f = kernel.Port(
		call = "read", error_code = 100, id = 100,
	)
	test/f.id == 100
	test/f.error_code == 100
	test/f.call == "read"

	f = kernel.Port(
		call = "x", error_code = 1000, id = 1000,
	)
	test/f.id == 1000
	test/f.error_code == 1000
	test/f.call == 'INVALID'
	with test/TypeError as exc:
		kernel.Port(id = "nonanumber")
	f = kernel.Port(
		call = "read", error_code = 10, id = 1000,
	)
	with test/OSError as exc:
		f.raised()
	test.isinstance(f.exception(), OSError)

	repr(f)
	f.leak()
	f.shatter()

def test_sockets_rallocate(test):
	# leveraging knowledge of kernel.Sockets.rallocate
	# in real code, rallocate against the Sockets *instance*
	test/list(kernel.Sockets.rallocate(10)) == [-1] * 10
	sb = kernel.Sockets.rallocate(16)
	mv = memoryview(sb)

	test/sb[0] != 1 # should be -1, but anything aside from 1 is okay.
	# itemsize is 4, so use pack_into.
	struct.pack_into("i", mv, 0, 1)
	test/sb[0] == 1

def test_ports_rallocate(test):
	# leveraging knowledge of kernel.Sockets.rallocate
	# in real code, rallocate against the Sockets *instance*
	test/list(kernel.Ports.rallocate(10)) == [-1] * 10
	sb = kernel.Ports.rallocate(16)
	mv = memoryview(sb)

	test/sb[0] != 1 # should be -1, but anything aside from 1 is okay.
	# itemsize is 4, so use pack_into.
	struct.pack_into("i", mv, 0, 1)
	test/sb[0] == 1

def test_octets_rallocate(test):
	test/list(bytes(kernel.Octets.rallocate(20))) == list(b'\x00' * 20)
	mb = kernel.Octets.rallocate(20)
	mb[0:5] = b'fffff'
	test/memoryview(mb).tobytes() == b'fffff' + (b'\x00' * (20 - 5))
	mv = memoryview(mb)
	b = bytes(mv[11])
	mb[10:15] = b'fffff'

def test_junction_rallocate_errors(test):
	J = kernel.Array()
	try:
		with test/LookupError as exc:
			J.rallocate("")
		with test/TypeError as exc:
			J.rallocate()
	finally:
		J.void()

def test_junction_collection_countdown(test):
	J = kernel.Array()
	try:
		J.resize_exoresource(2)
		data = b'SOME DATA'
		bufs = []
		for x in range(6):
			transits = J.rallocate("octets://spawn/bidirectional")
			reads = transits[0::2]
			writes = transits[1::2]

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

def test_sockets_accept_filter(test):
	J = kernel.Array()
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

def test_octets_acquire_badfd_detect(test):
	r, w = os.pipe()
	J = kernel.Array()
	try:
		xr = J.rallocate('octets://acquire/input', w)
		xr.port.error_code in test/(errno.EBADF, 0)
		xr.port.call in test/('read', None)

		xw = J.rallocate('octets://acquire/output', r)
		xw.port.error_code in test/(errno.EBADF, 0)
		xw.port.call in test/('write', None)

		xs, xsw = J.rallocate('octets://acquire/socket', r)
		test/xs.port.error_code == errno.EBADF
		test/xs.port.call == 'identify' # traffic local call

		xs = J.rallocate('sockets://acquire/socket', w)
		test/xs.port.error_code == errno.EBADF
		test/xs.port.call == 'identify' # traffic local call
	finally:
		os.close(r)
		os.close(w)
		J.void()

def test_octets_bind(test):
	s = kernel.Array.rallocate("sockets://ip4", ('127.0.0.1', 0))
	r, w = kernel.Array.rallocate(('octets', 'ip4', 'tcp', 'bind'), (s.endpoint(), ('127.0.0.1', 0)))
	try:
		test/r.port.error_code == 0
		test/w.endpoint() == s.endpoint()
	finally:
		s.terminate()
		r.terminate()
		w.terminate()

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
