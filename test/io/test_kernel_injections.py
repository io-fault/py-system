import sys
import os
import errno
from .. import kernel

def __test__(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(kernel))

def nomem(x):
	raise MemoryError(x)

def _error(n, _en = errno.EINTR):
	yield None
	for x in range(n):
		yield (_en,)
	# signal real call
	while True:
		yield False

def error(*args, **kw):
	g = _error(*args, **kw)
	next(g)
	return g.send

def errno_retry_callback(ctx):
	return (errno.EINTR,)

# These tests primarily exist
# for coverage purposes. They use __ERRNO_RECEPTACLE__ to exercise
# error cases.
def test_cannot_allocate_junction(test):
	test.skip(sys.platform == 'linux')
	try:
		kernel.__ERRNO_RECEPTACLE__['port_kqueue'] = errno_retry_callback
		j = kernel.Array()
		test/j.port.error_code == errno.EINTR
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()

def test_junction_force_eintr(test):
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g = kernel.__ERRNO_RECEPTACLE__['port_kevent'] = error(256, errno.EINTR)
		J.force()
		test/J.port.error_code == errno.EINTR
		test/J.port.call == 'kevent'
		test/g(0) == (errno.EINTR,)
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_junction_retry_fail(test):
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = kernel.Array()

		# exercise change's unlimited retry

		r, w = J.rallocate('octets://spawn/unidirectional')
		J.acquire(r)
		J.acquire(w)

		g = kernel.__ERRNO_RECEPTACLE__['port_kevent'] = error(256, errno.EINTR)
		with J:
			pass
		test/J.port.error_code == 0
		test/J.port.call == None
		test/g(0) == False

		del kernel.__ERRNO_RECEPTACLE__['port_kevent']

		J.force()

		# limited retry on kevent collection success
		g = kernel.__ERRNO_RECEPTACLE__['port_kevent'] = error(8, errno.EINTR)

		with J:
			pass
		test/J.port.error_code == 0
		test/J.port.call == None
		test/g(0) == False

		# limited retry on kevent collection gave up
		del kernel.__ERRNO_RECEPTACLE__['port_kevent']
		J.force()

		g = kernel.__ERRNO_RECEPTACLE__['port_kevent'] = error(512, errno.EINTR)

		with J:
			pass
		test/J.port.error_code == errno.EINTR
		test/J.port.call == 'kevent'
		test/g(0) == (errno.EINTR,)

	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_sockets_retry_fail(test):
	# Should trigger the limit.
	J = kernel.Array()
	try:
		s = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		s.port.leak()
		sock = s.port.id
		s.terminate()

		kernel.__ERRNO_RECEPTACLE__['port_listen'] = \
		kernel.__ERRNO_RECEPTACLE__['port_bind'] = \
		kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = \
		kernel.__ERRNO_RECEPTACLE__['port_socket'] = \
			errno_retry_callback

		s = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'socket'

		del kernel.__ERRNO_RECEPTACLE__['port_socket']
		s = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'

		s = J.rallocate('sockets://acquire', sock)
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'

		del kernel.__ERRNO_RECEPTACLE__['port_noblocking']
		s = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'bind'

		del kernel.__ERRNO_RECEPTACLE__['port_bind']
		s = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'listen'

	finally:
		os.close(sock)
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_retry_fail(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		kernel.__ERRNO_RECEPTACLE__['port_bind'] = \
		kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = \
		kernel.__ERRNO_RECEPTACLE__['port_socket'] = \
			errno_retry_callback

		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		test/r.port.error_code == errno.EINTR
		test/w.port.call == 'socket'
		test/r.port.error_code == errno.EINTR
		test/w.port.call == 'socket'
		r.terminate()
		w.terminate()

		del kernel.__ERRNO_RECEPTACLE__['port_socket']
		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		test/r.port.error_code == errno.EINTR
		test/w.port.call == 'fcntl'
		test/r.port.error_code == errno.EINTR
		test/w.port.call == 'fcntl'
		r.terminate()
		w.terminate()

		del kernel.__ERRNO_RECEPTACLE__['port_noblocking']
		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		test/r.port.error_code == errno.EINTR
		test/w.port.call == 'bind'
		test/r.port.error_code == errno.EINTR
		test/w.port.call == 'bind'
		r.terminate()
		w.terminate()
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_tcpip_retry_fail(test):
	J = kernel.Array()
	try:
		kernel.__ERRNO_RECEPTACLE__['port_connect'] = \
		kernel.__ERRNO_RECEPTACLE__['port_bind'] = \
		kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = \
		kernel.__ERRNO_RECEPTACLE__['port_socket'] = \
			errno_retry_callback

		s, _ = J.rallocate('octets://ip4', ('127.0.0.1', 100))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'socket'
		s, _ = J.rallocate(('octets', 'ip4', 'tcp', 'bind'), (('127.0.0.1', 100), ('127.0.0.1', 0)))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'socket'

		del kernel.__ERRNO_RECEPTACLE__['port_socket']
		s, _ = J.rallocate('octets://ip4', ('127.0.0.1', 100))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		s, _ = J.rallocate(('octets', 'ip4', 'tcp', 'bind'), (('127.0.0.1', 100), ('127.0.0.1', 0)))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'

		del kernel.__ERRNO_RECEPTACLE__['port_noblocking']

		s, _ = J.rallocate(('octets', 'ip4', 'tcp', 'bind'), (('127.0.0.1', 100), ('127.0.0.1', 0)))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'bind'

		del kernel.__ERRNO_RECEPTACLE__['port_bind']

		s, _ = J.rallocate('octets://ip4', ('127.0.0.1', 100))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'connect'
		s, _ = J.rallocate(('octets', 'ip4', 'tcp', 'bind'), (('127.0.0.1', 100), ('127.0.0.1', 0)))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'connect'
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_udpip_retry_fail(test):
	J = kernel.Array()
	try:
		kernel.__ERRNO_RECEPTACLE__['port_connect'] = \
		kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = \
		kernel.__ERRNO_RECEPTACLE__['port_socket'] = \
			errno_retry_callback

		s, _ = J.rallocate('octets://ip4:udp', ('127.0.0.1', 200))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'socket'

		del kernel.__ERRNO_RECEPTACLE__['port_socket']
		s, _ = J.rallocate('octets://ip4:udp', ('127.0.0.1', 200))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'

		del kernel.__ERRNO_RECEPTACLE__['port_noblocking']
		s, _ = J.rallocate('octets://ip4:udp', ('127.0.0.1', 200))
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'connect'

	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_pipe_retry_fail(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		kernel.__ERRNO_RECEPTACLE__['port_pipe'] = \
		kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = \
			errno_retry_callback

		s, _ = J.rallocate('octets://spawn/unidirectional')
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'pipe'

		del kernel.__ERRNO_RECEPTACLE__['port_pipe']
		s, _ = J.rallocate('octets://spawn/unidirectional')
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'

	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_socketpair_retry_fail(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		kernel.__ERRNO_RECEPTACLE__['port_socketpair'] = \
		kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = \
			errno_retry_callback

		s, _, s2, _2 = J.rallocate('octets://spawn/bidirectional')
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'socketpair'
		test/s2.port.error_code == errno.EINTR
		test/s2.port.call == 'socketpair'

		del kernel.__ERRNO_RECEPTACLE__['port_socketpair']
		s, _, s2, _2 = J.rallocate('octets://spawn/bidirectional')
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		test/s2.port.error_code == errno.EINTR
		test/s2.port.call == 'fcntl'

	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_acquire_retry_fail(test):
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = kernel.Array()
		kernel.__ERRNO_RECEPTACLE__['port_identify_type'] = errno_retry_callback

		for typ in ['octets://acquire/output', 'octets://acquire/input', 'sockets://acquire']:
			s = J.rallocate(typ, -14)
			test/s.port.error_code == errno.EINTR
			test/s.port.call == 'fstat'
			J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 3
			for x in J.transfer():
				test/x.terminated == True
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_file_retry_fail(test):
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = kernel.Array()

		kernel.__ERRNO_RECEPTACLE__['port_open'] = \
		kernel.__ERRNO_RECEPTACLE__['port_identify_type'] = \
			errno_retry_callback

		s = J.rallocate('octets://file/overwrite', '/dev/null')
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'open'
		J.acquire(s)

		del kernel.__ERRNO_RECEPTACLE__['port_open']
		s = J.rallocate('octets://file/overwrite', '/dev/null')
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fstat'
		J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 2
			for x in J.transfer():
				test/x.terminated == True
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

#
# These tests check that it can recover from EINTR
#

def test_sockets_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_listen'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_bind'] = error(12)
		g3 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(6)
		g4 = kernel.__ERRNO_RECEPTACLE__['port_socket'] = error(14)

		s = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		test/s.port.error_code == 0
		test/s.port.call == None
		s.terminate()

		test/g4(0) == False
		test/g1(0) == False
		test/g2(0) == False
		test/g3(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_tcpip_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_connect'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(6)
		g3 = kernel.__ERRNO_RECEPTACLE__['port_socket'] = error(14)

		s, s_ = J.rallocate('octets://ip4', ('127.0.0.1', 100))
		test/s.port.error_code == 0
		test/s.port.call == None
		s.terminate()
		s_.terminate()

		test/g1(0) == False
		test/g2(0) == False
		test/g3(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_udpip_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_connect'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(6)
		g3 = kernel.__ERRNO_RECEPTACLE__['port_socket'] = error(14)

		s, s_ = J.rallocate('octets://ip4:udp', ('127.0.0.1', 100))
		test/s.port.error_code == 0
		test/s.port.call == None
		s.terminate()
		s_.terminate()

		test/g1(0) == False
		test/g2(0) == False
		test/g3(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_bind'] = error(16)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(24)
		g3 = kernel.__ERRNO_RECEPTACLE__['port_socket'] = error(2)

		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)
		test/r.port.error_code == 0
		test/r.port.call == None
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_octets_file_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_open'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_identify_type'] = error(6)

		s = J.rallocate('octets://file/read', '/dev/null')
		test/s.port.error_code == 0
		test/s.port.call == None
		s.terminate()

		test/g1(0) == False
		test/g2(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_spawn_u_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_pipe'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(6)

		r, w = J.rallocate('octets://spawn/unidirectional')
		test/r.port.error_code == 0
		test/r.port.call == None
		test/w.port.error_code == 0
		test/w.port.call == None
		r.terminate()
		w.terminate()

		test/g1(0) == False
		test/g2(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_spawn_b_retry(test):
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_socketpair'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(6)

		r, w, r1, w1 = J.rallocate('octets://spawn/bidirectional')
		test/r.port.error_code == 0
		test/r.port.call == None
		test/w.port.error_code == 0
		test/w.port.call == None

		test/r1.port.error_code == 0
		test/r1.port.call == None
		test/w1.port.error_code == 0
		test/w1.port.call == None

		r.terminate()
		w.terminate()
		r1.terminate()
		w1.terminate()

		test/g1(0) == False
		test/g2(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_io_retry(test):
	# Should *not* trigger the limit with EINTR
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_octets'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_octets'] = error(6)

		r, w = J.rallocate('octets://spawn/unidirectional')
		J.acquire(r)
		J.acquire(w)

		r.acquire(r.rallocate(100))
		w.acquire(b'x' * 100)

		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/g1(0) == False
		test/g2(0) == False

		# octets io doesn't give up. should it?
		e1 = kernel.__ERRNO_RECEPTACLE__['port_input_octets'] = error(128)
		e2 = kernel.__ERRNO_RECEPTACLE__['port_output_octets'] = error(128)

		r.acquire(r.rallocate(100))
		w.acquire(b'x' * 100)
		while not r.exhausted and not w.exhausted:
			with J:
				pass
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_io_nomem_retry(test):
	# Should trigger the limit with ENOMEM
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_octets'] = error(12, errno.ENOMEM)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_octets'] = error(6, errno.ENOMEM)

		r, w = J.rallocate('octets://spawn/unidirectional')
		J.acquire(r)
		J.acquire(w)

		r.acquire(r.rallocate(100))
		w.acquire(b'x' * 100)

		while not r.exhausted or not w.exhausted:
			with J:
				test/any((r.terminated, w.terminated)) == False

		test/g1(0) == False
		test/g2(0) == False

		# trigger limited retry
		e1 = kernel.__ERRNO_RECEPTACLE__['port_input_octets'] = error(256, errno.ENOMEM)
		e2 = kernel.__ERRNO_RECEPTACLE__['port_output_octets'] = error(256, errno.ENOMEM)

		r.acquire(r.rallocate(100))
		w.acquire(b'x' * 100)
		while not r.terminated and not w.terminated:
			with J:
				pass
		test/e1(0) == (errno.ENOMEM,)
		test/e2(0) == (errno.ENOMEM,)
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_io_retry(test):
	# Should *not* trigger the limit with EINTR
	J = kernel.Array()
	try:
		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(8)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(6)

		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)

		r.acquire(r.rallocate(1))
		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/g1(0) == False
		test/g2(0) == False

		# doesn't give up
		e1 = kernel.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(128)
		e2 = kernel.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(128)

		r.acquire(r.rallocate(1))
		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.exhausted and not w.exhausted:
			with J:
				pass
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_io_nomem_retry(test):
	# Should *not* trigger the limit with EINTR
	J = kernel.Array()
	try:
		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(8, errno.ENOMEM)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(6, errno.ENOMEM)

		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)

		r.acquire(r.rallocate(1))
		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/g1(0) == False
		test/g2(0) == False

		# gives up
		e1 = kernel.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(256, errno.ENOMEM)
		e2 = kernel.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(256, errno.ENOMEM)

		r.acquire(r.rallocate(1))
		dga = w.rallocate(1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.terminated and not w.terminated:
			with J:
				pass
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_io_again(test):
	'trigger EAGAIN on datagrams output'
	J = kernel.Array()
	try:
		# send one and then trigger EAGAIN
		def eagain(errno = errno.EAGAIN):
			yield None
			yield False
			while True:
				yield (errno,)
		g = eagain()
		next(g)
		kernel.__ERRNO_RECEPTACLE__['port_output_datagrams'] = g.send

		r, w = J.rallocate('datagrams://ip4', ('127.0.0.1', 0))
		J.acquire(r)
		J.acquire(w)

		rdga = r.rallocate(1)
		r.acquire(rdga)
		dga = w.rallocate(2)
		dga.set_endpoint(0, r.endpoint())
		dga.payload(0)[:6] = b'foobar'
		dga.payload(1)[:] = b'x' * len(dga.payload(1))
		w.acquire(dga)

		while not r.exhausted:
			with J:
				pass

		# only writes one
		test/w.exhausted == False
		test/r.exhausted == True
		test/rdga.endpoint(0) == r.endpoint()
		test/rdga.payload(0)[:6] == b'foobar'
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_sockets_io_retry(test):
	test.skip(sys.platform == 'linux')
	# Should *not* trigger the limit with EINTR
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_sockets'] = error(256)

		r = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		r.acquire(r.rallocate(100))
		r._xtransfer = True
		J.acquire(r)

		with J:
			test/J.sizeof_transfer() == 1
			test/r.sizeof_transfer() == 0
		test/r._xtransfer == False

		test/g1(0) == False
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_sockets_io_nomem(test):
	# ENOMEM retry in accept()
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_sockets'] = error(256, errno.ENOMEM)

		r = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		J.acquire(r)

		fdset = r.rallocate(128)
		r.acquire(fdset)

		transits = J.rallocate('octets://ip4', r.endpoint())
		J.acquire(transits[0])
		J.acquire(transits[1])

		while not r.terminated:
			with J:
				pass

		test/g1(0) == (errno.ENOMEM,)
		transits[0].terminate()
		transits[1].terminate()
		with J:
			pass

		# same deal, but success this time.
		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_sockets'] = error(6, errno.ENOMEM)

		r = J.rallocate('sockets://ip4', ('127.0.0.1', 0))
		J.acquire(r)

		fdset = r.rallocate(128)
		r.acquire(fdset)

		transits = J.rallocate('octets://ip4', r.endpoint())
		J.acquire(transits[0])
		J.acquire(transits[1])

		while fdset[0] == -1:
			with J:
				pass
			test/any(x.terminated for x in J.resource) == False
		test/g1(0) == False
		os.close(fdset[0])

		with J:
			pass

	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_octets_resize_error(test):
	try:
		J = kernel.Array()
		g1 = kernel.__ERRNO_RECEPTACLE__['port_set_socket_option'] = error(8, errno.EINTR)
		r, w, r1, w1 = J.rallocate('octets://spawn/bidirectional')
		J.acquire(r)
		J.acquire(w)
		J.acquire(r1)
		J.acquire(w1)
		r.resize_exoresource(1024)
		test/g1(0) == False

		g1 = kernel.__ERRNO_RECEPTACLE__['port_set_socket_option'] = error(256, errno.EINTR)
		r.resize_exoresource(1024)
		# setsockopt gave up, *but* we don't mark an error
		test/g1(0) == (errno.EINTR,)
		test/r.port.error_code == 0

		g1 = kernel.__ERRNO_RECEPTACLE__['port_set_socket_option'] = error(1, errno.EINVAL)
		r.resize_exoresource(1024)
		# setsockopt gave up, *but* we don't mark an error
		test/g1(0) == False
		test/r.port.error_code == errno.EINVAL
		test/r.port.call == 'setsockopt'
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_ports_io_nomem(test):
	# This is a repeat of test_ports.test_io with one difference: error injection.
	J = kernel.Array()
	files = []
	try:
		files = [
			os.open('/dev/null', 0)
			for x in range(128)
		]

		transits = J.rallocate('ports://spawn/bidirectional')
		for x in transits:
			J.acquire(x)

		parent = transits[:2]
		child = transits[2:]

		buf = parent[1].rallocate(64)
		for x in range(64):
			buf[x] = files[x]

		parent[1].acquire(buf)

		# doesn't give up
		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_ports'] = error(512)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_ports'] = error(512)

		# cover the exhaustion case
		cbuf = child[0].rallocate(32)
		child[0].acquire(cbuf)
		while not child[0].exhausted:
			with J:
				pass
		test/g1(0) == False
		test/g2(0) == False

		# It's okay provided it's not a bunch of invalids.
		# Closing the original FD should close whatever is here.
		test/set(cbuf) != set((-1,))

		# doesn't give up for such low occurrences of memory errors
		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_ports'] = error(8, errno.ENOMEM)

		# consume the parent's buffer
		cbuf = child[0].rallocate(64)
		child[0].acquire(cbuf)
		while not parent[1].exhausted:
			with J:
				pass

		while child[0].resource[0] == -1:
			with J:
				pass
		test/g1(0) == False
		test/set(cbuf) != set((-1,))

		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_ports'] = error(16, errno.ENOMEM)
		one = parent[1].rallocate(1)
		one[0] = files[127]
		parent[1].acquire(one)

		while not parent[1].exhausted:
			with J:
				pass
		test/g2(0) == False

		# and now failure from MEM errors
		g1 = kernel.__ERRNO_RECEPTACLE__['port_input_ports'] = error(512, errno.ENOMEM)
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_ports'] = error(512, errno.ENOMEM)

		# send in the other direction as well
		buf = child[1].rallocate(64)
		for x in range(64, 128):
			buf[x-64] = files[x]
		parent[0].acquire(parent[0].rallocate(64))
		child[1].acquire(buf)

		while not child[1].terminated and not parent[0].terminated:
			with J:
				pass
		test/g1(0) == (errno.ENOMEM,)
		test/g2(0) == (errno.ENOMEM,)
	finally:
		J.void()
		for x in files:
			os.close(x)

def test_ports_io_again(test):
	# This is a repeat of test_ports.test_io with one difference: error injection.
	test.skip(sys.platform == 'linux')
	J = kernel.Array()
	try:
		transits = J.rallocate('ports://spawn/bidirectional')
		for x in transits:
			J.acquire(x)

		parent = transits[:2]
		child = transits[2:]

		buf = parent[1].rallocate(64)
		parent[1].acquire(buf)

		# gives up immediately
		g2 = kernel.__ERRNO_RECEPTACLE__['port_output_ports'] = error(2, errno.EAGAIN)
		with J:
			# an empty transfer
			test/J.sizeof_transfer() == 1

		test/g2(0) == (errno.EAGAIN,)
		test/g2(0) == False
	finally:
		J.void()

def test_junction_alloc_port_memory_errors(test):
	try:
		J = kernel.Array()

		kernel.__PYTHON_RECEPTACLE__['alloc_port'] = nomem

		with test/MemoryError as exc:
			J.rallocate('octets://ip4', ('127.0.1.45', 34))

		with test/MemoryError as exc:
			J.rallocate('sockets://ip4', ('127.0.1.45', 34))

		with test/MemoryError as exc:
			J.rallocate('datagrams://ip4', ('127.0.1.45', 34))

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/input', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/output', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/socket', 8)

		with test/MemoryError as exc:
			J.rallocate('sockets://acquire/socket', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://spawn/unidirectional')

		with test/MemoryError as exc:
			J.rallocate('ports://spawn/bidirectional')

		with test/MemoryError as exc:
			J.rallocate('ports://acquire/socket', 8)
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()
		J.void()

def test_junction_alloc_i_o_memory_errors(test):
	try:
		J = kernel.Array()

		kernel.__PYTHON_RECEPTACLE__['alloci'] = nomem
		kernel.__PYTHON_RECEPTACLE__['alloco'] = nomem

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/input', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/output', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://file/read', '/dev/null')

		with test/MemoryError as exc:
			J.rallocate('octets://spawn/unidirectional')

		del kernel.__PYTHON_RECEPTACLE__['alloci']
		with test/MemoryError as exc:
			J.rallocate('octets://spawn/unidirectional')
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()
		J.void()

def test_junction_allocio_memory_errors(test):
	try:
		J = kernel.Array()

		kernel.__PYTHON_RECEPTACLE__['allocio.alloc_pair'] = nomem
		kernel.__PYTHON_RECEPTACLE__['allociopair.alloc_pair'] = nomem

		with test/MemoryError as exc:
			J.rallocate('octets://spawn/unidirectional')

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/socket', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://ip4', ('127.0.0.1', 123))

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/socket', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://ip4', ('127.0.0.1', 123))

		with test/MemoryError as exc:
			J.rallocate('octets://acquire/socket', 8)

		with test/MemoryError as exc:
			J.rallocate('octets://ip4', ('127.0.0.1', 123))
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()
		J.void()

def test_junction_allocioio_memory_errors(test):
	try:
		J = kernel.Array()
		# identifiers used inside allocioio.
		seq = [
			'alloc_quad',
			'alloc_porta',
			'alloc_portb',
			'alloc_isubtype1',
			'alloc_osubtype1',
			'alloc_isubtype2',
			'alloc_osubtype2',
		]

		for x in seq:
			kernel.__PYTHON_RECEPTACLE__['allocioio.'+x] = nomem

		for x in seq:
			with test/MemoryError as exc:
				J.rallocate('octets://spawn/bidirectional')
			del kernel.__PYTHON_RECEPTACLE__['allocioio.'+x]

		test/bool(kernel.__PYTHON_RECEPTACLE__) == False
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()
		J.void()

def test_nosigpipe(test):
	test.skip(not getattr(kernel, 'F_SETNOSIGPIPE', None))
	try:
		J = kernel.Array()

		cases = [
			('octets://ip4', ('127.0.1.45', 0)),
			('octets://ip4:udp', ('127.0.1.45', 0)),
			(('octets', 'ip4', 'tcp', 'bind'), (('127.0.1.45', 0), ('127.0.0.1', 0))),
			('octets://ip6', ('::1', 0)),
			('octets://ip6:udp', ('::1', 0)),
			('datagrams://ip4', ('127.0.1.45', 0)),
			('ports://spawn/bidirectional',),
			('octets://spawn/unidirectional',),
		]

		kernel.__ERRNO_RECEPTACLE__['port_nosigpipe'] = lambda x: (errno.EINTR,)

		r, w = os.pipe()
		o = J.rallocate('octets://acquire/output', w)
		o.terminate()
		test/o.port.error_code == errno.EINTR
		test/o.port.call == 'fcntl'
		os.close(r)

		r, w = os.pipe()

		transits = J.rallocate('octets://spawn/bidirectional')
		transits[0].port.leak()
		transits[2].port.shatter()
		sp = transits[0].port.id
		for x in transits:
			x.terminate()
		cases.append(('octets://acquire/socket', sp))

		# trip failure in nosigpipe
		for x in cases:
			transits = J.rallocate(*x)
			for y in transits[1::2]:
				test/y.port.error_code == errno.EINTR
				test/y.port.call == 'fcntl'
			for y in transits:
				y.terminate()
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_close_retry(test):
	try:
		J = kernel.Array()

		kernel.__ERRNO_RECEPTACLE__['port_unlatch'] = lambda x: (errno.EINTR,)
		i, o = J.rallocate('octets://spawn/unidirectional')

		# close gives up, so validate that the fd is still good.
		i.terminate()
		test/i.terminated == True
		test/i.port.error_code == 0
		test/i.port.call == None
		with test.trap():
			os.fstat(i.port.id)
		os.close(i.port.id)

		# close eventually succeeds. validate that the fd is closed
		kernel.__ERRNO_RECEPTACLE__['port_unlatch'] = error(16)
		o.terminate()
		with test/OSError:
			os.fstat(o.port.id)
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_octets_acquire_retry(test):
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = kernel.Array()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_identify_type'] = error(8)

		s = J.rallocate('octets://acquire/input', -14)
		test/g1(0) == False
		test/s.port.error_code == errno.EBADF
		test/s.port.call == 'fstat'
		J.acquire(s)

		g1 = kernel.__ERRNO_RECEPTACLE__['port_identify_type'] = error(14)

		s = J.rallocate('octets://acquire/output', -14)
		test/g1(0) == False
		test/s.port.error_code == errno.EBADF
		test/s.port.call == 'fstat'
		J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 2
			for x in J.transfer():
				test/x.terminated == True
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_acquire_mustblock(test):
	test.skip(sys.platform == 'linux')
	try:
		J = kernel.Array()
		r, w = os.pipe()

		g1 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(256)

		s = J.rallocate('octets://acquire/input', r)
		test/g1(0) == (errno.EINTR,)
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		J.acquire(s)

		g1 = kernel.__ERRNO_RECEPTACLE__['port_noblocking'] = error(256)

		s = J.rallocate('octets://acquire/output', w)
		test/g1(0) == (errno.EINTR,)
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		J.acquire(s)

		s = J.rallocate('octets://file/read', '/dev/null')
		test/g1(0) == (errno.EINTR,)
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 3
			for x in J.transfer():
				test/x.terminated == True
	finally:
		kernel.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_endpoint_nomem(test):
	try:
		s = kernel.Array.rallocate("sockets://ip4", ('127.0.0.1', 0))
		try:
			kernel.__PYTHON_RECEPTACLE__['endpoint_create'] = nomem

			with test/MemoryError as exc:
				s.endpoint()
		finally:
			s.terminate()
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()

def test_datagramarray_nomem(test):
	try:
		kernel.__PYTHON_RECEPTACLE__['allocdga.tp_alloc'] = nomem
		kernel.__PYTHON_RECEPTACLE__['allocdga.new_ba'] = nomem
		kernel.__PYTHON_RECEPTACLE__['slicedga'] = nomem

		with test/MemoryError as exc:
			kernel.DatagramArray("ip4", 128, 1)

		del kernel.__PYTHON_RECEPTACLE__['allocdga.tp_alloc']
		with test/MemoryError as exc:
			kernel.DatagramArray("ip4", 128, 1)

		del kernel.__PYTHON_RECEPTACLE__['allocdga.new_ba']

		dga = kernel.DatagramArray("ip4", 128, 2)
		with test/MemoryError as exc:
			x = dga[:1]
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()

def test_datagramarray_index_nomem(test):
	try:
		kernel.__PYTHON_RECEPTACLE__['datagramarray_getitem.new_tuple'] = nomem
		kernel.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_endpoint'] = nomem
		kernel.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_memory'] = nomem

		dga = kernel.DatagramArray("ip4", 128, 1)

		with test/MemoryError as exc:
			x = dga[0]

		del kernel.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_endpoint']
		with test/MemoryError as exc:
			x = dga[0]

		del kernel.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_memory']
		with test/MemoryError as exc:
			x = dga[0]

		del kernel.__PYTHON_RECEPTACLE__['datagramarray_getitem.new_tuple']
		with test.trap():
			x = dga[0]
	finally:
		kernel.__PYTHON_RECEPTACLE__.clear()

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])