from ... import core
from ... import io
from ... import library as lib

def new():
	l = []
	def add(x, l = l):
		l.append(x)
	a = lib.Adapter(add, lib.Delta.construct)
	return l, a

def test_interchange_repr(test):
	l, a = new()
	test.isinstance(repr(lib.Interchange(a)), str)

def test_interchange_empty(test):
	l, a = new()
	ix = lib.Interchange(a)
	'io' in test/ix.routes
	test/len(ix.arrays) == 0

def test_interchange_routes(test):
	l, a = new()
	ix = lib.Interchange(a)
	ix.route(foo = 'bar')
	test/ix.routes['foo'] == 'bar'

def test_interchange_termination(test):
	def null(x):
		pass
	ix = lib.Interchange((null, null))
	with ix.xact() as alloc:
		alloc('octets://spawn/unidirectional')
	ix.terminate()

def test_idle_array_terminates(test):
	import time

	# we use the adapter's callbacks to increment our counter and to trip j.force()
	loops = 0
	def incloops(x):
		nonlocal loops
		loops += 1
		#test/isinstance(x, io.Array) == True
		test/list(x.transfer()) == []
		x.force()

	def testevents(events):
		test/events == None

	adapter = (testevents, incloops)

	ix = lib.Interchange(adapter)
	try:
		ix._getj() # kicks off the array's thread

		# we use j.force to rapidly trip the countdown.
		# there are no channels, so we should be able to trigger it quickly
		ix.force()
		while loops < 128:
			# encourage switch
			time.sleep(0.00001)
			if len(list(ix._iterarrays())) == 0:
				break

		test/len(list(ix._iterarrays())) == 0
	finally:
		ix.terminate()

def test_active_array_continues(test):
	import time

	# in the previous test, we validate that the thread terminates an idle array.

	# in this test, we want to validate the inverse, an active array
	# is never terminated

	loops = 0
	j = None
	def incloops(x):
		nonlocal loops
		loops += 1
		#test/isinstance(x, io.Array) == True
		x.force()

	def testevents(events):
		test/events == None

	adapter = (testevents, incloops)

	ix = lib.Interchange(adapter)
	try:
		ix._getj() # kicks off the array's thread
		with ix.xact() as st:
			r, w = st('octets://spawn/unidirectional')

		# we use j.force to rapidly trip the countdown.
		# there are channels, so we should never break;
		# rather the loop condition should fail.
		ix.force()
		while loops < 256:
			# encourage switch
			if not list(ix._iterarrays()):
				break
		test/list(ix._iterarrays()) != []

		ix.terminate()

		while list(ix._iterarrays()):
			time.sleep(0.000001)
		test/w.terminated == True
		test/r.terminated == True
	finally:
		ix.terminate()

def test_interchange_transfer(test):
	l, a = new()
	ix = lib.Interchange(a)
	try:
		with ix.xact() as alloc:
			r, w = alloc('octets://spawn/unidirectional')

		buf = r.rallocate(60)
		r.acquire(buf)
		w.acquire(b'4' * 60)
		while not w.exhausted or not r.exhausted:
			pass
		r.terminate()
		w.terminate()
		while not w.terminated or not r.terminated:
			pass

		# trigger the array recovery code
		for x in range(8):
			ix.force(a)

		with ix.xact() as alloc:
			r, w = alloc('octets://spawn/unidirectional')
		# Couple cycles should trigger the exit_at_zero reset.
		ix.force()
		ix.force()
		r.terminate()
		w.terminate()

		while not list(ix._iterarrays()):
			ix.force()
		test/bytes(buf) == b'4' * 60
	finally:
		ix.terminate()

def test_alloc_single_interchange(test):
	# single channel allocations have a distinct branch
	import time
	l, a = new()
	ix = lib.Interchange(a)
	try:
		with ix.xact() as alloc:
			r = alloc(('octets', 'file', 'read'), '/dev/zero')
			r2 = alloc(('octets', 'file', 'read'), '/dev/zero')
		r.acquire(bytearray(10))
		r2.acquire(bytearray(10))
		while not r.exhausted:
			time.sleep(0.00001)
		while not r2.exhausted:
			time.sleep(0.00001)
		r.terminate()
	finally:
		ix.terminate()

def test_interchange_overflow(test):
	# test the effect of the limit attribute
	l, a = new()
	ix = lib.Interchange(a)
	ix.channels_per_array = 0
	try:
		with ix.xact() as alloc:
			for i in range(20):
				alloc(('octets', 'file', 'read'), '/dev/zero')
		# checking for per array limits
		# ix.acquire allows the entire overflow to spill into the new array.
		test/len(list(ix._iterarrays())) == 2
	finally:
		ix.terminate()

def test_interchange_void(test):
	l, a = new()
	ix = lib.Interchange(a)
	# empty, safe to run.
	ix.void()
	# XXX: needs to be tested in a fork

def test_interchange_xact_fail(test):
	'.xact() context block that raises an exception'
	l, a = new()

	ix = lib.Interchange(a)
	# empty, safe to run.
	try:
		with test/Exception as e:
			with ix.xact() as alloc:
				r = alloc('octets://file/read', '/dev/zero')
				raise Exception("foo")
		test/r.terminated == True
	finally:
		ix.terminate()

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
