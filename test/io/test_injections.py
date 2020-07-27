import sys
import os
import errno
from ... import io

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
def test_cannot_allocate_array(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(sys.platform == 'linux')
	try:
		io.__ERRNO_RECEPTACLE__['port_kqueue'] = errno_retry_callback
		j = io.Array()
		test/j.port.error_code == errno.EINTR
	finally:
		io.__ERRNO_RECEPTACLE__.clear()

def test_array_force_eintr(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = io.Array()

		g = io.__ERRNO_RECEPTACLE__['port_kevent'] = error(256, errno.EINTR)
		J.force()
		test/J.port.error_code == errno.EINTR
		test/J.port.call == 'kevent'
		test/g(0) == (errno.EINTR,)
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_array_retry_fail(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = io.Array()

		# exercise change's unlimited retry

		fr, fw = os.pipe()
		r = io.alloc_input(fr)
		w = io.alloc_output(fw)
		J.acquire(r)
		J.acquire(w)

		g = io.__ERRNO_RECEPTACLE__['port_kevent'] = error(256, errno.EINTR)
		with J:
			pass
		test/J.port.error_code == 0
		test/J.port.call == None
		test/g(0) == False

		del io.__ERRNO_RECEPTACLE__['port_kevent']

		J.force()

		# limited retry on kevent collection success
		g = io.__ERRNO_RECEPTACLE__['port_kevent'] = error(8, errno.EINTR)

		with J:
			pass
		test/J.port.error_code == 0
		test/J.port.call == None
		test/g(0) == False

		# limited retry on kevent collection gave up
		del io.__ERRNO_RECEPTACLE__['port_kevent']
		J.force()

		g = io.__ERRNO_RECEPTACLE__['port_kevent'] = error(512, errno.EINTR)

		with J:
			pass
		test/J.port.error_code == errno.EINTR
		test/J.port.call == 'kevent'
		test/g(0) == (errno.EINTR,)

	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_acquire_retry_fail(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = io.Array()
		io.__ERRNO_RECEPTACLE__['port_identify_type'] = errno_retry_callback

		for typ in [io.alloc_input, io.alloc_output, io.alloc_service]:
			s = typ(-14)
			test/s.port.error_code == errno.EINTR
			test/s.port.call == 'fstat'
			J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 3
			for x in J.transfer():
				test/x.terminated == True
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_io_retry(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	# Should *not* trigger the limit with EINTR
	from ... import network
	J = io.Array()
	try:
		g1 = io.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(8)
		g2 = io.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(6)

		ep = network.Endpoint.from_ip4(('127.0.0.1', 0), type='datagrams')
		s = network.bind(ep)
		r, w = io.alloc_datagrams(s)
		J.acquire(r)
		J.acquire(w)

		rdga = io.DatagramArray('ip4', 1024, 1)
		r.acquire(rdga)
		dga = io.DatagramArray('ip4', 1024, 1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/g1(0) == False
		test/g2(0) == False

		# doesn't give up
		e1 = io.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(128)
		e2 = io.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(128)

		rdga = io.DatagramArray('ip4', 1024, 1)
		r.acquire(rdga)
		dga = io.DatagramArray('ip4', 1024, 1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.exhausted and not w.exhausted:
			with J:
				pass
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_io_nomem_retry(test):
	from ... import network
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	# Should *not* trigger the limit with EINTR
	J = io.Array()
	try:
		g1 = io.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(8, errno.ENOMEM)
		g2 = io.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(6, errno.ENOMEM)

		ep = network.Endpoint.from_ip4(('127.0.0.1', 0), type='datagrams')
		s = network.bind(ep)
		r, w = io.alloc_datagrams(s)
		J.acquire(r)
		J.acquire(w)

		rdga = io.DatagramArray('ip4', 1024, 1)
		r.acquire(rdga)
		dga = io.DatagramArray('ip4', 1024, 1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.exhausted or not w.exhausted:
			with J:
				pass

		test/g1(0) == False
		test/g2(0) == False

		# gives up
		e1 = io.__ERRNO_RECEPTACLE__['port_input_datagrams'] = error(256, errno.ENOMEM)
		e2 = io.__ERRNO_RECEPTACLE__['port_output_datagrams'] = error(256, errno.ENOMEM)

		rdga = io.DatagramArray('ip4', 1024, 1)
		r.acquire(rdga)
		dga = io.DatagramArray('ip4', 1024, 1)
		dga.set_endpoint(0, r.endpoint())
		w.acquire(dga)

		while not r.terminated and not w.terminated:
			with J:
				pass
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagrams_io_again(test):
	"""
	# Trigger EAGAIN on datagrams output.
	"""
	from ... import network
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	J = io.Array()
	try:
		# send one and then trigger EAGAIN
		def eagain(errno = errno.EAGAIN):
			yield None
			yield False
			while True:
				yield (errno,)
		g = eagain()
		next(g)
		io.__ERRNO_RECEPTACLE__['port_output_datagrams'] = g.send

		ep = network.Endpoint.from_ip4(('127.0.0.1', 0), type='datagrams')
		s = network.bind(ep)
		r, w = io.alloc_datagrams(s)
		J.acquire(r)
		J.acquire(w)

		rdga = io.DatagramArray('ip4', 1024, 1)
		r.acquire(rdga)
		dga = io.DatagramArray('ip4', 1024, 2)
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
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_resize_error(test):
	import socket
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		J = io.Array()
		g1 = io.__ERRNO_RECEPTACLE__['port_set_socket_option'] = error(8, errno.EINTR)
		s1, s2 = socket.socketpair()
		r, w = io.alloc_octets(s1.fileno())
		r1, w1 = io.alloc_octets(s2.fileno())
		J.acquire(r)
		J.acquire(w)
		J.acquire(r1)
		J.acquire(w1)
		r.resize_exoresource(1024)
		test/g1(0) == False

		g1 = io.__ERRNO_RECEPTACLE__['port_set_socket_option'] = error(256, errno.EINTR)
		r.resize_exoresource(1024)
		# setsockopt gave up, *but* we don't mark an error
		test/g1(0) == (errno.EINTR,)
		test/r.port.error_code == 0

		g1 = io.__ERRNO_RECEPTACLE__['port_set_socket_option'] = error(1, errno.EINVAL)
		r.resize_exoresource(1024)
		# setsockopt gave up, *but* we don't mark an error
		test/g1(0) == False
		test/r.port.error_code == errno.EINVAL
		test/r.port.call == 'setsockopt'
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_array_alloc_port_memory_errors(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		io.__PYTHON_RECEPTACLE__['alloc_port'] = nomem

		with test/MemoryError as exc:
			io.alloc_input(8)

		with test/MemoryError as exc:
			io.alloc_output(8)

		with test/MemoryError as exc:
			io.alloc_octets(8)

		with test/MemoryError as exc:
			io.alloc_service(8)
	finally:
		io.__PYTHON_RECEPTACLE__.clear()

def test_array_alloc_i_o_memory_errors(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		io.__PYTHON_RECEPTACLE__['alloci'] = nomem
		io.__PYTHON_RECEPTACLE__['alloco'] = nomem

		with test/MemoryError as exc:
			io.alloc_input(8)

		with test/MemoryError as exc:
			io.alloc_output(8)
	finally:
		io.__PYTHON_RECEPTACLE__.clear()

def test_array_allocio_memory_errors(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		io.__PYTHON_RECEPTACLE__['allocio.alloc_pair'] = nomem
		io.__PYTHON_RECEPTACLE__['alloci'] = nomem

		for i in range(4):
			with test/MemoryError as exc:
				io.alloc_octets(8)
			with test/MemoryError as exc:
				io.alloc_service(8)
	finally:
		io.__PYTHON_RECEPTACLE__.clear()

def test_nosigpipe(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(not getattr(io, 'F_SETNOSIGPIPE', None))
	try:
		J = io.Array()

		io.__ERRNO_RECEPTACLE__['port_nosigpipe'] = lambda x: (errno.EINTR,)

		r, w = os.pipe()
		o = io.alloc_output(w)
		o.terminate()
		test/o.port.error_code == errno.EINTR
		test/o.port.call == 'fcntl'
		os.close(r)
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_close_retry(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		J = io.Array()

		io.__ERRNO_RECEPTACLE__['port_unlatch'] = lambda x: (errno.EINTR,)
		r, w = os.pipe()
		i = io.alloc_input(r)
		o = io.alloc_output(w)

		# close gives up, so validate that the fd is still good.
		i.terminate()
		test/i.terminated == True
		test/i.port.error_code == 0
		test/i.port.call == None
		with test.trap():
			os.fstat(i.port.id)
		os.close(i.port.id)

		# close eventually succeeds. validate that the fd is closed
		io.__ERRNO_RECEPTACLE__['port_unlatch'] = error(16)
		o.terminate()
		with test/OSError:
			os.fstat(o.port.id)
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.void()

def test_octets_acquire_retry(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(sys.platform == 'linux')
	# Should trigger the limit.
	try:
		J = io.Array()

		g1 = io.__ERRNO_RECEPTACLE__['port_identify_type'] = error(8)

		s = io.alloc_input(-14)
		test/g1(0) == False
		test/s.port.error_code == errno.EBADF
		test/s.port.call == 'fstat'
		J.acquire(s)

		g1 = io.__ERRNO_RECEPTACLE__['port_identify_type'] = error(14)

		s = io.alloc_output(-14)
		test/g1(0) == False
		test/s.port.error_code == errno.EBADF
		test/s.port.call == 'fstat'
		J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 2
			for x in J.transfer():
				test/x.terminated == True
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_octets_acquire_mustblock(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	test.skip(sys.platform == 'linux')
	try:
		J = io.Array()
		r, w = os.pipe()

		g1 = io.__ERRNO_RECEPTACLE__['port_noblocking'] = error(256)

		s = io.alloc_input(r)
		test/g1(0) == (errno.EINTR,)
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		J.acquire(s)

		g1 = io.__ERRNO_RECEPTACLE__['port_noblocking'] = error(256)

		s = io.alloc_output(w)
		test/g1(0) == (errno.EINTR,)
		test/s.port.error_code == errno.EINTR
		test/s.port.call == 'fcntl'
		J.acquire(s)

		with J:
			test/J.sizeof_transfer() == 2
			for x in J.transfer():
				test/x.terminated == True
	finally:
		io.__ERRNO_RECEPTACLE__.clear()
		J.terminate()
		with J:
			pass

def test_datagramarray_nomem(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		io.__PYTHON_RECEPTACLE__['allocdga.tp_alloc'] = nomem
		io.__PYTHON_RECEPTACLE__['allocdga.new_ba'] = nomem
		io.__PYTHON_RECEPTACLE__['slicedga'] = nomem

		with test/MemoryError as exc:
			io.DatagramArray("ip4", 128, 1)

		del io.__PYTHON_RECEPTACLE__['allocdga.tp_alloc']
		with test/MemoryError as exc:
			io.DatagramArray("ip4", 128, 1)

		del io.__PYTHON_RECEPTACLE__['allocdga.new_ba']

		dga = io.DatagramArray("ip4", 128, 2)
		with test/MemoryError as exc:
			x = dga[:1]
	finally:
		io.__PYTHON_RECEPTACLE__.clear()

def test_datagramarray_index_nomem(test):
	test.skip(not '__ERRNO_RECEPTACLE__' in dir(io))
	try:
		io.__PYTHON_RECEPTACLE__['datagramarray_getitem.new_tuple'] = nomem
		io.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_endpoint'] = nomem
		io.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_memory'] = nomem

		dga = io.DatagramArray("ip4", 128, 1)

		with test/MemoryError as exc:
			x = dga[0]

		del io.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_endpoint']
		with test/MemoryError as exc:
			x = dga[0]

		del io.__PYTHON_RECEPTACLE__['datagramarray_getitem.get_memory']
		with test/MemoryError as exc:
			x = dga[0]

		del io.__PYTHON_RECEPTACLE__['datagramarray_getitem.new_tuple']
		with test.trap():
			x = dga[0]
	finally:
		io.__PYTHON_RECEPTACLE__.clear()

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
