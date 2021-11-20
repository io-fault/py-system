import os
import time
from .. import kernel as module

def test_defer_units(test):
	ki = module.Scheduler()
	try:
		ki.defer(1, 0, 'n')
		ki.defer(2, 0, 's')
		ki.defer(3, 0, 'm')
		ki.defer(4, 0, 'u')
		with test/TypeError:
			ki.defer(5, 0, 492492)
		with test/TypeError:
			ki.defer(6, 0, 'microsecond')
		with test/ValueError:
			ki.defer(7, 0, 'f')

		test/len(ki.wait()) == 4
	finally:
		ki.void()
		test.garbage()

def test_defer(test):
	ki = module.Scheduler()
	try:
		ob = "alarm-event-string"
		ki.defer(ob, 1, 'n')
		test/ki.wait() << ('alarm', ob)
	finally:
		ki.void()
		test.garbage()

def test_defer_time(test):
	ki = module.Scheduler()
	try:
		ob = "alarm-event-string"
		a=time.time()
		ki.defer(ob, 1, 's')
		test/ki.wait() << ('alarm', ob)
		b=time.time()
		test/(b-a-1) < 0.3
	finally:
		ki.void()
		test.garbage()

def test_ignore_force(test):
	ki = module.Scheduler()
	try:
		ob = "alarm-event-string"
		a=time.time()
		ki.defer(ob, 1200, 'm')
		ki.interrupt()
		# forced while not waiting outside block validate that it's not drop through.
		ki.wait()
		b=time.time()
		test/(b-a) >= 1
	finally:
		ki.void()
		test.garbage()

def test_force(test):
	ki = module.Scheduler()
	try:
		ob = "alarm-event-string"
		a=time.time()
		ki.defer(ob, 5, 's')
		test/ki.interrupt() == None

		ki._set_waiting()
		test/ki.interrupt() == True
		ki.interrupt()
		# signals that it was already tripped
		test/ki.interrupt() == False
		test/ki.interrupt() == False
		# forced while not waiting outside block validate that it's not drop through.
		ki.wait()

		test/ki.interrupt() == None
		b=time.time()
		test/(b-a) < 5
	finally:
		ki.void()
		test.garbage()

def test_recur(test):
	ki = module.Scheduler()
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

	ki = module.Scheduler()
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
	# - &module.Scheduler.execute
	"""

	k = module.Scheduler()
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
	# - &module.Scheduler.execute
	"""

	k = module.Scheduler()
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
	# - &module.Scheduler.execute
	"""

	k = module.Scheduler()
	for x in range(512):
		test/k.loaded == False
		test/k.execute(None) == 0

def test_enqueue_force_event(test):
	"""
	# - &module.Scheduler.execute

	# Interface.enqueue should be sensitive to the event wait state.
	# This validates that no timeout event is generated designating that a user event was received.
	"""

	k = module.Scheduler()
	k._set_waiting()
	k.enqueue((lambda: None))
	test/k.execute(None) == 0
	test/k.wait(2) == []
	test/k.execute(None) == 1

def test_interface_close(test):
	"""
	# - &module.Scheduler.close
	# - &module.Scheduler.closed
	"""

	k = module.Scheduler()
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
	# - &module.Scheduler.execute
	"""

	test.explicit()
	out = []
	def etrap(ob, err):
		out.append((ob, err))
		raise RuntimeError("exception during exception")

	k = module.Scheduler()
	k.enqueue(None)
	test/k.execute(etrap) == 0
	test/k.execute(etrap) == 1

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
