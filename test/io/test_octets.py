"""
# Check io.Octets interfaces.
"""
import os
import os.path
import errno
import time
import tempfile

from ... import io
from . import common

def test_anonymous_endpoints_socketpair(test):
	J = io.Array()
	try:
		channels = common.allocsockets(J)
		try:
			s = list(set([x.endpoint() for x in channels]))
			test/len(s) == 1
			test.isinstance(s[0], tuple)
			test.isinstance(s[0][0], int)
			test.isinstance(s[0][1], int)
		finally:
			for x in channels:
				x.terminate()
	finally:
		J.terminate()
		common.cycle(J)

def test_anonymous_endpoints_pipe(test):
	J = io.Array()
	try:
		channels = common.allocpipe(J)
		try:
			for eps in [x.endpoint() for x in channels]:
				test/eps == None
		finally:
			for x in channels:
				x.terminate()
	finally:
		J.terminate()
		common.cycle(J)

def test_buffer_write_location(test):
	am = common.ArrayActionManager()
	with am.thread():
		# constructors
		r, w = map(common.Events, common.allocpipe(am.array))
		ba = bytearray(512)
		ba[:len('text!')] = b'\x00' * len('text!')
		view = memoryview(ba)[256:]

		with am.manage(r), am.manage(w):
			# setup_read doesn't take buffers
			r.channels[0].acquire(view)

			w.setup_write(b'text!')
			for x in am.delta():
				if w.exhaustions:
					break
			for x in am.delta():
				if r.data == b'text!':
					break

		# make sure the channel is writing into the proper offset
		test/ba[0:len('text!')] != b'text!'
		test/ba[256:256+len('text!')] == b'text!'
	test/am.array.terminated == True

def test_channel_force(test):
	# Array.force() causes the user filter to be triggered
	# in order to interrupt any waiting kevent() call.

	# Channel.force() causes an empty transfer to occur on the
	# channel given that the channel's resource is not exhausted.
	j = io.Array()
	try:
		channels = common.allocsockets(j)
		for x in channels:
			j.acquire(x)
		channels[0].acquire(bytearray(1))
		with j:
			test/list(j.transfer()) == []
			pass

		channels[0].force()
		with j:
			test/list(j.transfer()) == [channels[0]]
			test/channels[0].slice() == slice(0,0)
			test/channels[0].exhausted == False
	finally:
		j.terminate()
		with j:
			pass

def test_full_buffer_forced_write(test):
	"""
	# Test the force method on lose-octets with a full write buffer.
	"""
	am = common.ArrayActionManager()
	with am.thread():
		r, w = map(common.Events, common.allocpipe(am.array))
		r.channels[0].resize_exoresource(64)
		w.channels[0].resize_exoresource(64)

		with am.manage(r), am.manage(w):
			w.setup_write(b'bytes' * (1024 * 100))
			for x in am.delta():
				if w.events:
					break

			# let one cycle pass to pickup any events, then clear.
			for x in am.delta():
				break
			w.clear()

			w.channels[0].force()
			for x in am.delta():
				if w.events:
					break
			test/bytes(w.events[0].transferred) == b''
			test/w.channels[0].terminated == False
			test/w.channels[0].exhausted == False

def test_multiple_arrays(test, number_to_check = 128):
	arrays = []
	try:
		for x in range(number_to_check):
			arrays.append(io.Array())
	finally:
		for x in arrays:
			test/x.terminated == False
			x.terminate()
			test/x.terminated == True
			with x:
				test/x.terminated == True
			test/x.terminated == True

def test_objects(test):
	am = common.ArrayActionManager()

	with am.thread():
		for exchange in common.object_transfer_cases:
			cxn = common.allocsockets(am.array)
			server = common.Objects(cxn[:2])
			client = common.Objects(cxn[2:])

			with am.manage(server), am.manage(client):
				exchange(test, am, client, server)

			test/server.channels[0].terminated == True
			test/server.channels[1].terminated == True
			test/client.channels[0].terminated == True
			test/client.channels[1].terminated == True

			if None:
				test/server.channels[0].transfer() == None
				test/server.channels[1].transfer() == None
				test/client.channels[0].transfer() == None
				test/client.channels[1].transfer() == None

			test/server.channels[0].exhausted == False
			test/server.channels[1].exhausted == False
			test/client.channels[0].exhausted == False
			test/client.channels[1].exhausted == False

def test_array_void(test):
	j = io.Array()
	test/j.terminated == False
	j.void()

	# now inside a cycle
	j = io.Array()
	j.force()
	with j:
		j.void()
	test/j.terminated == False

	quad = common.allocsockets(j)
	for x in quad:
		j.acquire(x)
	test/j.volume == 4
	test/j.terminated == False

	with j:
		test/j.volume == 4
		j.void()
		test/j.volume == 0
	test/j.volume == 0
	test/j.terminated == False

	for x in quad:
		test/x.terminated == True
		test/x.exhausted == False

	test/j.terminated == False
	j.terminate()
	test/j.terminated == True
	with test/io.TransitionViolation as exc:
		with j:
			pass

def test_octets_acquire_after_terminate(test):
	j = io.Array()
	test/j.sizeof_transfer() == 0

	r, w = common.allocpipe(j)
	test/r.transfer() == None
	test/w.transfer() == None
	test/r.sizeof_transfer() == 0
	test/w.sizeof_transfer() == 0
	r.terminate()
	w.terminate()
	# We don't throw terminated errors here as there is
	# a race condition involved with parallel event collection.
	# Termination is noted in loop before the exhaust event
	# is processed by its receiver.
	test/r.acquire(bytearray(0)) == None
	test/w.acquire(bytearray(0)) == None

def test_array_flush_release(test):
	J = io.Array()

	r, w = common.allocpipe(J)
	J.acquire(r)
	J.acquire(w)
	w.acquire(b'1')
	r.acquire(bytearray(1))
	exhausted = 0
	while exhausted < 2:
		for x in common.cycle(J):
			if x.demand is not None:
				exhausted += 1
	# New resource is not acquired in loop, but exhaust
	# needs to irelease the resource in the __exit__/flush.
	test/r.resource == None
	test/w.resource == None

	# now the termination branch
	w.acquire(b'1')
	r.acquire(bytearray(1))
	test/r.resource != None
	test/w.resource != None
	r.terminate()
	w.terminate()
	terms = 0
	while terms < 2:
		for x in common.cycle(J):
			if x.termination is not None:
				terms += 1
			# also fail if there is a demand reference.
			test/x.demand == None
	# validate that termination causes the resources to be released.
	test/r.resource == None
	test/w.resource == None

def test_octets_resource_error(test):
	j = io.Array()
	r, w = common.allocpipe(j)

	# needs mutable buffer
	with test/BufferError as exc:
		r.acquire(b'')

	# already acquired, writer
	w.acquire(b'')
	with test/io.TransitionViolation as exc:
		w.acquire(b'')

	# already acquired, reader
	test/r.exhausted == True
	r.acquire(bytearray(0))

	test/r.exhausted == False
	with test/io.TransitionViolation as exc:
		r.acquire(bytearray(0))

	r.terminate()
	w.terminate()
	j.terminate()
	with j:
		pass

def test_terminating_exhaust(test):
	j = io.Array()
	r, w = common.allocpipe(j)
	r.terminate()
	w.terminate()
	test/r.exhausted == False
	test/w.exhausted == False
	j.terminate()
	with j:
		pass

def array_termination(test, J):
	J.terminate()
	# array termination cascades to channels
	with J:
		channels = set(J.transfer())
		ports = [x.port for x in channels]
	test/J.terminated == True
	for x in channels:
		test/x.terminated == True
	return channels

def test_octets_acquire_badfd_detect(test):
	r, w = os.pipe()
	J = io.Array()
	try:
		xr = io.alloc_input(w)
		xr.port.error_code in test/(errno.EBADF, 0)
		xr.port.call in test/('read', None)

		xw = io.alloc_output(r)
		xw.port.error_code in test/(errno.EBADF, 0)
		xw.port.call in test/('write', None)

		xs, xsw = io.alloc_octets(r)
		test/xs.port.error_code == errno.EBADF
		test/xs.port.call == 'identify' # local call
	finally:
		os.close(r)
		os.close(w)
		J.void()

def test_ip4(test):
	common.stream_listening_connection(test, 'ip4', ('127.0.0.1', 0))

def test_ip6(test):
	common.stream_listening_connection(test, 'ip6', ('::1', 0))

def file_test(test, am, path, apath):
	count = 0
	wrote_data = []
	thedata = b'\xF1'*128

	fd = os.open(apath, os.O_APPEND|os.O_WRONLY|os.O_CREAT)
	wr = io.alloc_output(fd)
	wr.port.raised()

	writer = common.Events(wr)
	writer.setup_write(thedata)
	test/wr.resource == thedata

	i = 0
	with am.manage(writer):
		for x in am.delta():
			test/writer.terminated == False
			if writer.exhaustions:
				writer.clear()
				i += 1
				if i > 32:
					writer.terminate()
					break
				writer.setup_write(thedata)
				am.force()
	os.chmod(apath, 0o700)

	# validate expectations
	expected = (i * thedata)
	with open(path, 'rb') as f:
		actual = f.read()
	test/actual == expected
	data_size = len(thedata) * i

	# read it back
	fd = os.open(apath, os.O_RDONLY)
	rd = io.alloc_input(fd)
	rd.port.raised() # check exception

	out = []
	reader = common.Events(rd)
	reader.setup_read(37)
	xfer_len = 0
	with am.manage(reader):
		for x in am.delta():

			if reader.events:
				xfer = reader.data
				out.append(xfer)
				xfer_len += len(xfer)

				if reader.exhaustions:
					reader.clear()
					reader.setup_read(93)
					am.force()
			if reader.terminated or xfer_len >= data_size:
				reader.raised()
				break

	test/(bytearray(0).join(out)) == expected

	somedata = b'0' * 256

	fd = os.open(apath, os.O_WRONLY)
	wr = io.alloc_output(fd)
	wr.port.raised()
	writer = common.Events(wr)
	writer.setup_write(somedata)
	test/wr.resource == somedata
	i = 0
	with am.manage(writer):
		for x in am.delta():
			if writer.exhaustions:
				writer.clear()
				i += 1
				if i > 32:
					writer.terminate()
					break
				writer.setup_write(somedata)
				am.force()

	expected = (i * somedata)
	with open(path, 'rb') as f:
		actual = f.read()
	test/actual == expected

	# read it back
	data_size = len(expected)
	xfer_len = 0
	fd = os.open(apath, os.O_RDONLY)
	rd = io.alloc_input(fd)
	out = []
	reader = common.Events(rd)
	reader.setup_read(17)
	with am.manage(reader):
		for x in am.delta():
			if reader.events:
				xfer = reader.data
				out.append(xfer)
				xfer_len += len(xfer)

				if reader.exhaustions:
					reader.clear()
					reader.setup_read(73)
					am.force()
			if reader.terminated or xfer_len >= data_size:
				reader.raised()
				break
	test/(bytearray(0).join(out)) == expected

def test_file(test):
	am = common.ArrayActionManager()
	with am.thread(), tempfile.TemporaryDirectory() as d:
		path = os.path.join(d, "wfile")
		file_test(test, am, path, path)

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
