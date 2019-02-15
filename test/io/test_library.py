import time
import os
import os.path
from ... import core
from ... import io
from ... import library as lib
from . import common

def test_adapter_properties(test):
	a = lib.Adapter.create("endpoint", "transformer")
	test/a.endpoint == "endpoint"
	test/a.transformer == "transformer"

def test_delta(test):
	d = lib.Delta.construct()
	test/d.endpoint == None
	test/d.terminal == False
	test/d.demand == None
	test/d.payload == None
	test.isinstance(repr(d), str)
	test.isinstance(str(d), str)

	class Channel(object):
		def __init__(self):
			self.polarity = 1
			self.terminated = False
			self.resource = None
			self.mslice = None
			self.exhausted = False
			self.port = None
			self.mendpoint = None

		def slice(self):
			return self.mslice

		def transfer(self):
			return self.resource[self.mslice]

		def sizeof_transfer(self):
			return self.mslice.stop - self.mslice.start

		def acquire(self, res):
			self.resource = res
			return self

		def endpoint(self):
			return self.mendpoint

	T = Channel()
	T.mendpoint = "END"
	T.terminated = True
	T.acquire(b'foo')
	T.mslice = slice(0,3)
	d = lib.Delta.snapshot(T)
	test/d.terminal == True
	test/d.payload == b'foo'
	test/d.demand == None
	test.isinstance(str(d), str)
	test/d.endpoint == "END"

	T = Channel()
	T.polarity = -1
	T.acquire(b'bar')
	T.exhausted = True
	T.mslice = slice(0,3)
	d = lib.Delta.snapshot(T)
	test/d.terminal == False
	test/d.payload == b'bar'
	test/d.demand == T.acquire
	test.isinstance(str(d), str)
	test/d.endpoint == None

def test_anonymous_endpoints_socketpair(test):
	J = io.Array()
	try:
		channels = J.rallocate('octets://spawn/bidirectional')
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
		channels = J.rallocate('octets://spawn/unidirectional')
		try:
			for eps in [x.endpoint() for x in channels]:
				test/eps == None
		finally:
			for x in channels:
				x.terminate()
	finally:
		J.terminate()
		common.cycle(J)

def test_buffer_write_location(test, req = ('octets', 'spawn', 'unidirectional')):
	am = common.ArrayActionManager()
	with am.thread():
		# constructors
		r, w = map(common.Events, am.array.rallocate(req))
		ba = bytearray(512)
		ba[:len('foobar!')] = b'\x00' * len('foobar!')
		view = memoryview(ba)[256:]

		with am.manage(r), am.manage(w):
			# setup_read doesn't take buffers
			r.channels[0].acquire(view)

			w.setup_write(b'foobar!')
			for x in am.delta():
				if w.exhaustions:
					break
			for x in am.delta():
				if r.data == b'foobar!':
					break

		# make sure the channel is writing into the proper offset
		test/ba[0:len('foobar!')] != b'foobar!'
		test/ba[256:256+len('foobar!')] == b'foobar!'
	test/am.array.terminated == True

def test_channel_force(test):
	# Array.force() causes the user filter to be triggered
	# in order to interrupt any waiting kevent() call.

	# Channel.force() causes an empty transfer to occur on the
	# channel given that the channel's resource is not exhausted.
	j = io.Array()
	try:
		channels = j.rallocate(('octets', 'spawn', 'bidirectional'))
		for x in channels:
			j.acquire(x)
		channels[0].acquire(channels[0].rallocate(1))
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
		r, w = map(common.Events,am.array.rallocate(('octets', 'spawn', 'unidirectional')))
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

def test_multiarray(test, number_to_check = 128):
	'Validate that multiple arrays can exist.'
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

def test_objects(test, req = ('octets', 'spawn', 'bidirectional')):
	'common.Objects sanity'
	am = common.ArrayActionManager()

	with am.thread():
		for exchange in common.object_transfer_cases:
			cxn = am.array.rallocate(req)
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

def test_void(test):
	j = io.Array()
	test/j.terminated == False
	j.void()

	# now inside a cycle
	j = io.Array()
	j.force()
	with j:
		j.void()
	test/j.terminated == False

	quad = j.rallocate('octets://spawn/bidirectional')
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

def test_acquire_after_terminate(test):
	j = io.Array()
	test/j.sizeof_transfer() == 0
	r, w = j.rallocate('octets://spawn/unidirectional')
	test/r.transfer() == None
	test/w.transfer() == None
	test/r.sizeof_transfer() == 0
	test/w.sizeof_transfer() == 0
	r.terminate()
	w.terminate()
	# We don't throw terminated errors here as there is
	# a race condition involved with parallel event collection.
	# Termination is noted in traffic loop before the exhaust event
	# is processed by its receiver.
	test/r.acquire(r.rallocate(0)) == None
	test/w.acquire(w.rallocate(0)) == None

def test_array_flush_release(test):
	"Validates the Channel's resource is released on flush"
	J = io.Array()
	r, w = J.rallocate('octets://spawn/unidirectional')
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
	r, w = j.rallocate('octets://spawn/unidirectional')

	# needs mutable buffer
	with test/BufferError as exc:
		r.acquire(b'')

	# already acquired, writer
	w.acquire(b'')
	with test/io.TransitionViolation as exc:
		w.acquire(b'')

	# already acquired, reader
	test/r.exhausted == True
	r.acquire(r.rallocate(0))

	test/r.exhausted == False
	with test/io.TransitionViolation as exc:
		r.acquire(r.rallocate(0))

	r.terminate()
	w.terminate()
	j.terminate()
	with j:
		pass

def test_terminating_exhaust(test):
	j = io.Array()
	r, w = j.rallocate('octets://spawn/unidirectional')
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

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
