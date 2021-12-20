import os
import time
from .. import kernel as module

class Violation(Exception):
	pass

def test_Scheduler_close(test):
	"""
	# - &module.Scheduler.close
	# - &module.Scheduler.closed

	# Validate the effect of closing a scheduler instance.
	"""

	k = module.Scheduler()
	test/k.closed == False
	test/k.close() == True
	test/k.closed == True

	test/k.close() == False # already closed
	test/k.closed == True

	test/k.wait() == 0

	# Task still functions.
	k.enqueue((lambda: None))
	test/k.execute() == 1

def test_Scheduler_execute(test):
	"""
	# - &module.Scheduler.execute

	# Validate the execution of enqueued tasks.
	"""

	k = module.Scheduler()
	x = False
	def effect():
		nonlocal x
		x = True

	test/k.loaded == False
	k.enqueue(effect)
	test/k.loaded == True
	test/k.execute() == 1
	test/k.loaded == False
	test/x == True

	test/k.execute() == 0
	test/k.loaded == False

def test_Scheduler_exceptions(test):
	"""
	# - &module.Scheduler
	# - &module.Event.meta_exception

	# Validate exception signalling.
	"""
	errors = []
	def errlog(*args):
		errors.append(args)

	def raised():
		raise Violation("sample")

	ks = module.Scheduler()
	errctl = module.Link(module.Event.meta_exception(None), errlog)
	ks.dispatch(errctl)

	ks.enqueue(raised)
	test/ks.execute() == 1
	test/errors[0][0] == errctl
	test/errors[0][1] == raised
	err = errors[0][2]
	test.isinstance(err, Violation)
	test/err.__traceback__ != None

	ks.close()

def test_execute_nothing(test):
	"""
	# - &module.Scheduler.execute
	"""

	k = module.Scheduler()
	for x in range(512):
		test/k.loaded == False
		test/k.execute() == 0

def test_enqueue_interrupt(test):
	"""
	# - &module.Scheduler.execute

	# &module.Scheduler.enqueue should be sensitive to the event wait state.
	# This validates that no timeout event is generated designating that a
	# user event was received.
	"""

	k = module.Scheduler()
	k._set_waiting()
	k.enqueue((lambda: None))

	# Clear queue to allow kernelq_receive to be performed.
	test/k.execute() == 1
	test/k.wait(0) == 1

def test_dispatch_actuate(test):
	"""
	# - &module.Event.meta_actuate

	# Check that actuate is dispatched.
	"""

	events = []
	N = module.Event.meta_actuate(None)
	ln = module.Link(N, (lambda x: events.append(x.event)))

	ks = module.Scheduler()
	try:
		ks.dispatch(ln)
		test/ks.wait(0) == 1
		test/ks.execute() == 1
		test/(ln in ks.operations()) == False

		# Too late.
		lnf = module.Link(N, (lambda x: None))
		test/ValueError ^ (lambda: ks.dispatch(lnf))
	finally:
		ks.void()

	test/len(events) == 1
	test/id(events[0]) == id(ln.event)

def test_dispatch_terminate(test):
	"""
	# - &module.Event.meta_terminate

	# Check that terminate is dispatched.
	"""

	events = []
	N = module.Event.meta_terminate(None)
	ln = module.Link(N, (lambda x: events.append(x.event)))

	ks = module.Scheduler()
	try:
		ks.dispatch(ln)
		test/ks.wait(0) == 0
		test/ks.execute() == 0
		test/(ln in ks.operations()) == True

		test/ks.close() == True
		test/ks.execute() == 1
		test/(ln in ks.operations()) == False

		# Nothing more.
		test/ks.execute() == 0
	except:
		ks.void()

	test/len(events) == 1
	test/id(events[0]) == id(ln.event)

def test_Link_never_states(test):
	"""
	# - &module.Event.never

	# Check that never is available and has no immediate effect.
	"""

	events = []
	N = module.Event.never(None)
	ln = module.Link(N, (lambda x: events.append(1)))

	ks = module.Scheduler()
	try:
		ks.dispatch(ln)
		test/ln.dispatched == True
		test/ln.cancelled == False

		test/ks.wait(0) == 0
		test/ks.execute() == 0
		ks.cancel(ln)
		test/ks.wait(0) == 0
		test/ks.execute() == 0
	finally:
		ks.void()

	test/sum(events) == 0

def test_Link_scheduler_states(test):
	"""
	# - &module.Event.time
	"""

	events = []
	N = module.Event.time(0)
	ln = module.Link(N, (lambda x: events.append(x)))

	ks = module.Scheduler()
	try:
		test/ln.dispatched == False
		test/ln.cancelled == False

		ks.dispatch(ln, cyclic=False)
		test/ln.dispatched == True
		test/ln.cancelled == False

		test/ks.wait(1) == 1
		test/ks.execute() == 1

		test/ks.wait(0) == 0
		test/ks.execute() == 0

		test/ln.dispatched == True
		test/ln.cancelled == True
	finally:
		ks.void()

	test/len(events) == 1

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
	test/k.execute(etrap) == 1

def test_interrupt(test):
	"""
	# - &module.Scheduler.interrupt
	"""
	s = (10**9)
	ki = module.Scheduler()
	try:
		a=time.time()
		ki.dispatch(module.Link(module.Event.time(5*s), (lambda x: None)))
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

def test_Event_time_units(test):
	"""
	# - &module.Event

	# Time events are distinct regardless of their timing.
	"""
	evt = module.Event.time
	test/evt(0) != evt(0)
	test/evt(1) != evt(1)

	ns = evt(200)
	test/ns == ns
	test.isinstance(ns, module.Event)
	test/ns.type == 'time'

def test_Event_process_pid(test):
	"""
	# - &module.Event.process_exit
	"""
	allocproc = module.Event.process_exit

	# Expect argument requirement.
	test/TypeError ^ allocproc

	try:
		xev = allocproc(0)
	except OSError:
		# linux
		import os
		xev = allocproc(os.getpid())
		xev2 = allocproc(os.getpid())
		test/id(xev2) != id(xev)
		test/xev2 != xev
	else:
		test.isinstance(xev, module.Event)
		xev2 = allocproc(0)

		test/id(xev2) != id(xev)
		test/xev2 == xev
		test/allocproc(0) != allocproc(1)
		test/allocproc(1) == allocproc(1)

def test_Event_time(test):
	ki = module.Scheduler()
	try:
		ln = module.Link(module.Event.time(1), (lambda ln: None))
		ki.dispatch(ln, cyclic=False)
		test/ki.wait(1) == 1
		test/ki.wait(0) == 0
		test/ki.wait(0) == 0
		test/ki.execute() == 1
	finally:
		ki.void()
		test.garbage()

def test_interrupt_ignored(test):
	"""
	# Check that interrupt outside of wait has no effect.
	"""
	ms = 10 ** 6 # ns units
	ki = module.Scheduler()
	ev = module.Event.time(1200*ms)
	try:
		a=time.time()
		ln = module.Link(ev, (lambda x: None))
		ki.dispatch(ln)
		ki.interrupt()
		ki.wait(2)
		b=time.time()
		test/(b-a) >= 1
	finally:
		ki.void()
		test.garbage()

def test_time_event_periodic(test):
	ms = 10 ** 6 # ns units
	ki = module.Scheduler()
	events = []
	try:
		a=time.time()
		# recur about 500ms
		ln = module.Link(module.Event.time(100*ms), (lambda x: events.append(x)))
		ki.dispatch(ln)
		for x in range(5):
			ki.wait()
			ki.execute()

		b=time.time()
		# approx half a second
		test/(b-a) < 0.8

		# cancellation
		ki.cancel(ln)
		test/ki.wait(0)
		test/ki.execute() == 0
	finally:
		ki.void()
		test.garbage()

def test_signal_trap(test):
	"""
	# - &module.Event.process_signal

	# Validate the execution of system process exit events.
	"""
	import os
	import signal

	events = []
	ki = module.Scheduler()
	try:
		ln = module.Link(module.Event.process_signal(signal.SIGUSR2), (lambda x: events.append(x)))

		ki.dispatch(ln)
		test/ki.wait(0) == 0
		test/ki.execute() == 0

		os.kill(os.getpid(), signal.SIGUSR2)
		test/ki.wait(0) == 1
		test/ki.execute() == 1
	finally:
		ki.void()
		test.garbage()

	test/events == [ln]

def test_process_exit(test):
	"""
	# - &module.Event.process_exit

	# Validate the execution of system process exit events.
	"""
	import os
	status = pid = None
	r, w = os.pipe()
	def child(fd = r):
		i = os.read(fd, 1)
		os._exit(3)

	def subprocess_exit_cb(link):
		nonlocal pid, status
		status = os.waitpid(pid, os.P_WAIT)[1]

	ki = module.Scheduler()
	try:
		pid = os.fork()

		if pid == 0:
			child()
		else:
			ln = module.Link(module.Event.process_exit(pid), subprocess_exit_cb)
			ki.dispatch(ln)
			os.write(w, b'f')

		# Initial executing queue is empty.
		ki.wait()
		ki.execute()

		test/os.WIFEXITED(status) == True
		test/os.WEXITSTATUS(status) == 3
	finally:
		ki.void()
		test.garbage()

def test_Scheduler_io_pipe(test):
	"""
	# - &module.Event.io_transmit
	# - &module.Event.io_receive
	# - &module.Scheduler.wait
	# - &module.Scheduler.dispatch

	# Validate transmit and receive signals on regular pipes.
	"""
	import os
	import fcntl
	ioe = []

	r, w = os.pipe()
	for x in [r, w]:
		flags = fcntl.fcntl(x, fcntl.F_GETFL)
		fcntl.fcntl(x, fcntl.F_SETFL, flags | os.O_NONBLOCK)

	rxe = module.Event.io_receive(None, r)
	txe = module.Event.io_transmit(None, w)

	ki = module.Scheduler()
	try:
		data = b''
		rx = module.Link(rxe, lambda x: ioe.append(os.read(x.event.port, 2048)))
		tx = module.Link(txe, lambda x: os.write(x.event.port, data))
		ki.dispatch(rx)
		ki.dispatch(tx)

		data = b'first'
		ki.wait(0)
		ki.execute()
		# Allow receive to occur.
		data = b''
		ki.wait(0)
		ki.execute()

		data = b'second'
		ki.wait(0)
		ki.execute()
		tx()
		# Allow receive to occur.
		data = b''
		ki.wait(0)
		ki.execute()
	finally:
		ki.void()
		test.garbage()

	ioe = [x for x in ioe if x]
	test/ioe[0] == b'first'
	ioe[1] in test/{b'second', b'second'*2}

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
