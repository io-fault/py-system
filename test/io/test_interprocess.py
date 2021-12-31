import os
from ... import kernel
from ... import network
from ... import io
from . import common

def fork_and_circulate(test, am, channels):
	# fork before am.manage() for ease.
	pid = os.fork()
	if pid == 0:
		try:
			objects = common.Objects(channels[2:])
			am.array.void()
			with am.thread(), am.manage(objects):
				channels[0].port.shatter()
				common.child_echo(am, objects)
		except:
			import traceback
			traceback.print_exc()
			os._exit(7)
		finally:
			os._exit(0)
	else:
		parent = common.Objects(channels[:2])
		echos = [
			2,
			3,
			[x**2 for x in range(200)],
			"foo",
			"bar",
			"data!" * 200,
		]

		with am.thread(), am.manage(parent):
			channels[2].port.shatter()

			for echo in echos:
				parent.send(echo)
				for x in am.delta():
					if parent.read_channel.terminated:
						_pid, code = os.waitpid(pid, 0)
						test/os.WEXITSTATUS(code) == 0
						test/"child exited" == "too early"
					if parent.received_objects:
						break

				test/echo == parent.received_objects[0]
				parent.clear()
			else:
				parent.send(None)
				am.array.force()
				_pid, code = os.waitpid(pid, 0)
				test/os.WEXITSTATUS(code) == 0
		test/parent.channels[0].terminated == True
		test/parent.channels[1].terminated == True
		if None:
			test/parent.channels[0].transfer() == None
			test/parent.channels[1].transfer() == None

def test_bidirectional(test):
	"""
	# Check for IPC via bidirectional spawns
	"""
	am = common.ArrayActionManager()
	channels = common.allocsockets(am.array)
	fork_and_circulate(test, am, channels)

def test_unidirectional(test):
	"""
	# Check for IPC via unidirectional spawns
	"""
	am = common.ArrayActionManager()

	r, w = common.allocpipe(am.array)
	rr, ww = common.allocpipe(am.array)

	channels = (r, ww, rr, w)
	fork_and_circulate(test, am, channels)

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
