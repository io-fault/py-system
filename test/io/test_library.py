import time
import os
import os.path
from .. import core
from .. import kernel
from .. import library as lib
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
	J = kernel.Junction()
	try:
		transits = J.rallocate('octets://spawn/bidirectional')
		try:
			s = list(set([x.endpoint() for x in transits]))
			test/len(s) == 1
			test.isinstance(s[0], tuple)
			test.isinstance(s[0][0], int)
			test.isinstance(s[0][1], int)
		finally:
			for x in transits:
				x.terminate()
	finally:
		J.terminate()
		common.cycle(J)

def test_anonymous_endpoints_pipe(test):
	J = kernel.Junction()
	try:
		transits = J.rallocate('octets://spawn/unidirectional')
		try:
			for eps in [x.endpoint() for x in transits]:
				test/eps == None
		finally:
			for x in transits:
				x.terminate()
	finally:
		J.terminate()
		common.cycle(J)

def test_buffer_write_location(test, req = ('octets', 'spawn', 'unidirectional')):
	jam = common.JunctionActionManager()
	with jam.thread():
		# constructors
		r, w = map(common.Events, jam.junction.rallocate(req))
		ba = bytearray(512)
		ba[:len('foobar!')] = b'\x00' * len('foobar!')
		view = memoryview(ba)[256:]

		with jam.manage(r), jam.manage(w):
			# setup_read doesn't take buffers
			r.transits[0].acquire(view)

			w.setup_write(b'foobar!')
			for x in jam.delta():
				if w.exhaustions:
					break
			for x in jam.delta():
				if r.data == b'foobar!':
					break

		# make sure the transit is writing into the proper offset
		test/ba[0:len('foobar!')] != b'foobar!'
		test/ba[256:256+len('foobar!')] == b'foobar!'
	test/jam.junction.terminated == True

def test_transit_force(test):
	# Junction.force() causes the user filter to be triggered
	# in order to interrupt any waiting kevent() call.

	# Channel.force() causes an empty transfer to occur on the
	# transit given that the transit's resource is not exhausted.
	j = kernel.Junction()
	try:
		transits = j.rallocate(('octets', 'spawn', 'bidirectional'))
		for x in transits:
			j.acquire(x)
		transits[0].acquire(transits[0].rallocate(1))
		with j:
			test/list(j.transfer()) == []
			pass

		transits[0].force()
		with j:
			test/list(j.transfer()) == [transits[0]]
			test/transits[0].slice() == slice(0,0)
			test/transits[0].exhausted == False
	finally:
		j.terminate()
		with j:
			pass

def test_full_buffer_forced_write(test):
	"""
	# Test the force method on lose-octets with a full write buffer.
	"""
	jam = common.JunctionActionManager()
	with jam.thread():
		r, w = map(common.Events,jam.junction.rallocate(('octets', 'spawn', 'unidirectional')))
		r.transits[0].resize_exoresource(64)
		w.transits[0].resize_exoresource(64)

		with jam.manage(r), jam.manage(w):
			w.setup_write(b'bytes' * (1024 * 100))
			for x in jam.delta():
				if w.events:
					break

			# let one cycle pass to pickup any events, then clear.
			for x in jam.delta():
				break
			w.clear()

			w.transits[0].force()
			for x in jam.delta():
				if w.events:
					break
			test/bytes(w.events[0].transferred) == b''
			test/w.transits[0].terminated == False
			test/w.transits[0].exhausted == False

def test_multijunction(test, number_to_check = 128):
	'Validate that multiple junctions can exist.'
	junctions = []
	try:
		for x in range(number_to_check):
			junctions.append(kernel.Junction())
	finally:
		for x in junctions:
			test/x.terminated == False
			x.terminate()
			test/x.terminated == True
			with x:
				test/x.terminated == True
			test/x.terminated == True

def test_objects(test, req = ('octets', 'spawn', 'bidirectional')):
	'common.Objects sanity'
	jam = common.JunctionActionManager()

	with jam.thread():
		for exchange in common.object_transfer_cases:
			cxn = jam.junction.rallocate(req)
			server = common.Objects(cxn[:2])
			client = common.Objects(cxn[2:])

			with jam.manage(server), jam.manage(client):
				exchange(test, jam, client, server)

			test/server.transits[0].terminated == True
			test/server.transits[1].terminated == True
			test/client.transits[0].terminated == True
			test/client.transits[1].terminated == True

			if None:
				test/server.transits[0].transfer() == None
				test/server.transits[1].transfer() == None
				test/client.transits[0].transfer() == None
				test/client.transits[1].transfer() == None

			test/server.transits[0].exhausted == False
			test/server.transits[1].exhausted == False
			test/client.transits[0].exhausted == False
			test/client.transits[1].exhausted == False

def test_void(test):
	j = kernel.Junction()
	test/j.terminated == False
	j.void()

	# now inside a cycle
	j = kernel.Junction()
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
	with test/kernel.TransitionViolation as exc:
		with j:
			pass

def test_acquire_after_terminate(test):
	j = kernel.Junction()
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

def test_junction_flush_release(test):
	"Validates the Channel's resource is released on flush"
	J = kernel.Junction()
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
	j = kernel.Junction()
	r, w = j.rallocate('octets://spawn/unidirectional')

	# needs mutable buffer
	with test/BufferError as exc:
		r.acquire(b'')

	# already acquired, writer
	w.acquire(b'')
	with test/kernel.TransitionViolation as exc:
		w.acquire(b'')

	# already acquired, reader
	test/r.exhausted == True
	r.acquire(r.rallocate(0))

	test/r.exhausted == False
	with test/kernel.TransitionViolation as exc:
		r.acquire(r.rallocate(0))

	r.terminate()
	w.terminate()
	j.terminate()
	with j:
		pass

def test_terminating_exhaust(test):
	j = kernel.Junction()
	r, w = j.rallocate('octets://spawn/unidirectional')
	r.terminate()
	w.terminate()
	test/r.exhausted == False
	test/w.exhausted == False
	j.terminate()
	with j:
		pass

def junction_termination(test, J):
	J.terminate()
	# junction termination cascades to transits
	with J:
		transits = set(J.transfer())
		ports = [x.port for x in transits]
	test/J.terminated == True
	for x in transits:
		test/x.terminated == True
	return transits

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
