import os
import time
from .. import events as module

def test_alarm_units(test):
	ki = module.Interface()
	try:
		ki.alarm(1, 0, 'n')
		ki.alarm(2, 0, 's')
		ki.alarm(3, 0, 'm')
		ki.alarm(4, 0, 'u')
		with test/TypeError:
			ki.alarm(5, 0, 492492)
		with test/TypeError:
			ki.alarm(6, 0, 'microsecond')
		with test/ValueError:
			ki.alarm(7, 0, 'f')

		test/len(ki.wait()) == 4
	finally:
		ki.void()
		test.garbage()

def test_alarm(test):
	ki = module.Interface()
	try:
		ob = "foobar"
		ki.alarm(ob, 1, 'n')
		test/ki.wait() << ('alarm', ob)
	finally:
		ki.void()
		test.garbage()

def test_alarm_time(test):
	ki = module.Interface()
	try:
		ob = "foobar"
		a=time.time()
		ki.alarm(ob, 1, 's')
		test/ki.wait() << ('alarm', ob)
		b=time.time()
		test/(b-a-1) < 0.3
	finally:
		ki.void()
		test.garbage()

def test_ignore_force(test):
	ki = module.Interface()
	try:
		ob = "foobar"
		a=time.time()
		ki.alarm(ob, 1200, 'm')
		ki.force()
		# forced while not waiting outside block validate that it's not drop through.
		ki.wait()
		b=time.time()
		test/(b-a) >= 1
	finally:
		ki.void()
		test.garbage()

def test_force(test):
	ki = module.Interface()
	try:
		ob = "foobar"
		a=time.time()
		ki.alarm(ob, 5, 's')
		test/ki.force() == None

		ki._set_waiting()
		test/ki.force() == True
		ki.force()
		# signals that it was already tripped
		test/ki.force() == False
		test/ki.force() == False
		# forced while not waiting outside block validate that it's not drop through.
		ki.wait()

		test/ki.force() == None
		b=time.time()
		test/(b-a) < 5
	finally:
		ki.void()
		test.garbage()

def test_recur(test):
	ki = module.Interface()
	try:
		ob = "foo"
		a=time.time()
		# recur about 500ms
		ki.recur(ob, 100, 'm')
		for x in range(5):
			test/ki.wait() << ('recur', ob)
		b=time.time()
		# approx half a second
		test/(b-a) < 0.8

		# cancellation
		ki.cancel(ob)
		test/ki.wait(0) == []
	finally:
		ki.void()
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

	test/tuple(k.wait()) == ()

	# Task still functions.
	k.enqueue((lambda: None))
	test/k.execute(None) == 0
	test/k.execute(None) == 1

def test_execute_error_trap_exceptions(test):
	"""
	# - &module.Interface.execute
	"""

	test.explicit()
	out = []
	def etrap(ob, err):
		out.append((ob, err))
		raise RuntimeError("exception during exception")

	k = module.Interface()
	k.enqueue(None)
	test/k.execute(etrap) == 0
	test/k.execute(etrap) == 1

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
