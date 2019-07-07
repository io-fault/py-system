import os
import time
from .. import events as library

def test_alarm_units(test):
	kif = library.Interface()
	try:
		kif.alarm(1, 0, 'n')
		kif.alarm(2, 0, 's')
		kif.alarm(3, 0, 'm')
		kif.alarm(4, 0, 'u')
		with test/TypeError:
			kif.alarm(5, 0, 492492)
		with test/TypeError:
			kif.alarm(6, 0, 'microsecond')
		with test/ValueError:
			kif.alarm(7, 0, 'f')

		test/len(kif.wait()) == 4
	finally:
		kif.void()
		test.garbage()

def test_alarm(test):
	kif = library.Interface()
	try:
		ob = "foobar"
		kif.alarm(ob, 1, 'n')
		test/kif.wait() << ('alarm', ob)
	finally:
		kif.void()
		test.garbage()

def test_alarm_time(test):
	kif = library.Interface()
	try:
		ob = "foobar"
		a=time.time()
		kif.alarm(ob, 1, 's')
		test/kif.wait() << ('alarm', ob)
		b=time.time()
		test/(b-a-1) < 0.3
	finally:
		kif.void()
		test.garbage()

def test_ignore_force(test):
	kif = library.Interface()
	try:
		ob = "foobar"
		a=time.time()
		kif.alarm(ob, 1200, 'm')
		kif.force()
		# forced while not waiting outside block validate that it's not drop through.
		kif.wait()
		b=time.time()
		test/(b-a) >= 1
	finally:
		kif.void()
		test.garbage()

def test_force(test):
	kif = library.Interface()
	try:
		ob = "foobar"
		a=time.time()
		kif.alarm(ob, 5, 's')
		test/kif.force() == None
		with kif:
			test/kif.force() == True
			kif.force()
			# signals that it was already tripped
			test/kif.force() == False
			test/kif.force() == False
			# forced while not waiting outside block validate that it's not drop through.
			kif.wait()
		test/kif.force() == None
		b=time.time()
		test/(b-a) < 5
	finally:
		kif.void()
		test.garbage()

def test_recur(test):
	kif = library.Interface()
	try:
		ob = "foo"
		a=time.time()
		# recur about 500ms
		kif.recur(ob, 100, 'm')
		for x in range(5):
			test/kif.wait() << ('recur', ob)
		b=time.time()
		# approx half a second
		test/(b-a) < 0.8

		# cancellation
		kif.cancel(ob)
		with kif:
			kif.force()
			test/kif.wait() == []
	finally:
		kif.void()
		test.garbage()

def test_track(test):
	import os
	r, w = os.pipe()
	def child(fd = r):
		i = os.read(fd, 1)
		os._exit(3)

	ki = module.Interface()
	try:
		pid = os.fork()

		if pid == 0:
			child()
		else:
			ki.track(pid)
			os.write(w, b'f')

		r = ki.wait()
		test/r[0] == ('process', pid, None)
		_, status = os.waitpid(pid, os.P_WAIT)
		test/os.WIFEXITED(status) == True
		test/os.WEXITSTATUS(status) == 3
	finally:
		ki.void()
		test.garbage()

def test_execute(test):
	"""
	# - &module.Interface.execute
	"""

	k = module.Interface()
	x = False
	def effect():
		nonlocal x
		x = True

	test/k.loaded == False
	k.enqueue(effect)
	test/k.loaded == True
	test/k.execute(None) == 0
	test/k.loaded == True
	test/k.execute(None) == 1
	test/k.loaded == False
	test/x == True

	test/k.execute(None) == 0
	test/k.loaded == False

def test_execute_error_trap(test):
	"""
	# - &module.Interface.execute
	"""

	k = module.Interface()
	x = False
	def trap(ob, err):
		nonlocal x
		x = (ob, err)
	def effect():
		raise ValueError("data")

	k.enqueue(effect)
	test/k.execute(None) == 0
	test/k.execute(trap) == 1

	test/x[0] == effect
	test.isinstance(x[1], ValueError)
	test/x[1].args == ("data",)

def test_execute_nothing(test):
	"""
	# - &module.Interface.execute
	"""

	k = module.Interface()
	for x in range(512):
		test/k.loaded == False
		test/k.execute(None) == 0

def test_enqueue_force_event(test):
	"""
	# - &module.Interface.execute

	# Interface.enqueue should be sensitive to the event wait state.
	# This validates that no timeout event is generated designating that a user event was received.
	"""

	k = module.Interface()
	k._set_waiting()
	k.enqueue((lambda: None))
	test/k.execute(None) == 0
	test/k.wait(2) == []
	test/k.execute(None) == 1

def test_wait_timeout_event(test):
	"""
	# - &module.Interface.execute

	# Interface.enqueue should be sensitive to the event wait state.
	# This validates that no timeout event is generated designating that a user event was received.
	"""

	k = module.Interface()
	test/k.wait(0) == [] # No duration no timeout event.
	test/k.wait(1) == [('timeout', 1)]

def test_interface_close(test):
	"""
	# - &module.Interface.close
	# - &module.Interface.closed
	"""

	k = module.Interface()
	test/k.closed == False
	test/k.close() == True
	test/k.closed == True

	test/k.close() == False # already closed
	test/k.closed == True

	test/RuntimeError ^ k.wait

	# Task still functions.
	k.enqueue((lambda: None))
	test/k.execute(None) == 0
	test/k.execute(None) == 1

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
