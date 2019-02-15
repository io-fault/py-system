import os
from ... import io
from ... import library as lib
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

def test_bidirectional(test, req = ('octets', 'spawn', 'bidirectional')):
	'Check for IPC via bidirectional spawns'
	am = common.ArrayActionManager()
	channels = am.array.rallocate(req)
	fork_and_circulate(test, am, channels)

def test_unidirectional(test, req = ('octets', 'spawn', 'unidirectional')):
	'Check for IPC via unidirectional spawns'
	am = common.ArrayActionManager()
	r, w = am.array.rallocate(req)
	rr, ww = am.array.rallocate(req)
	channels = (r, ww, rr, w)
	fork_and_circulate(test, am, channels)

def test_ports_files(test):
	import tempfile
	J = io.Array()
	try:
		pairs = J.rallocate('ports://spawn/bidirectional')
		parent, child = pairs[:2], pairs[2:]

		with tempfile.TemporaryFile(mode='w+b') as resource:
			resource.write(b'DATA')
			resource.flush()

			for x in parent + child:
				J.acquire(x)
			pbuf = parent[1].rallocate(1)
			cbuf = child[0].rallocate(1)
			pbuf[0] = resource.fileno()

			pid = os.fork()
			if pid == 0:
				parent[0].port.shatter()

				common.cycle(J)
				child[0].acquire(cbuf)
				while cbuf[0] == -1:
					common.cycle(J)

				try:
					child_file = open(cbuf[0], 'w+b')
					child_file.seek(0, 2)
					child_file.write(b'CHILD!')
					child_file.flush()
				except:
					os._exit(15)
				os._exit(20)
			else:
				child[0].port.shatter()
				common.cycle(J)
				parent[1].acquire(pbuf)
				common.cycle(J)
			pid, code = os.waitpid(pid, 0)
			status = os.WEXITSTATUS(code)

			test/status == 20

			resource.seek(0)
			test/resource.read() == b'DATACHILD!'
	finally:
		J.terminate()
		common.cycle(J)

def test_ports_sockets(test):
	"""
	# Send a listening socket file descriptor to a child process.
	"""
	am = common.ArrayActionManager()

	channels = am.array.rallocate('ports://spawn/bidirectional')

	pid = os.fork()
	if pid == 0:
		try:
			am.array.void()
			child = common.Endpoint(channels[2:])
			channels[0].port.shatter()
			del channels

			with am.thread(), am.manage(child):
				# read a descriptor from the parent

				# get sockets port
				child.setup_read(1)
				for x in am.delta():
					if child.read_payload_int:
						break

				fd = child.read_payload_int[0]
				test/fd != -1

				sockets = am.array.rallocate('sockets://acquire/socket', fd)
				sockets.port.raised()
				listen = common.Events(sockets)
				listen.setup_read(1)
				with am.manage(listen):
					channels = am.array.rallocate('octets://ip4', sockets.endpoint())
					client = common.Endpoint(channels)
					with am.manage(client):
						for x in am.delta():
							if listen.sockets:
								break
		except:
			import traceback
			traceback.print_exc()
			os._exit(7)
		finally:
			os._exit(0)
	else:
		parent = common.Endpoint(channels[:2])
		channels[2].port.shatter() # child's copy
		del channels

		with am.thread(), am.manage(parent):
			sockets = am.array.rallocate('sockets://ip4', ('127.0.0.1', 0))

			r = parent.write_channel.rallocate(1)
			r[0] = sockets.port.id
			parent.setup_write(r)

			for x in am.delta():
				if parent.units_written:
					break

			# check that the child is still around
			_pid, code = os.waitpid(pid, 0)
			test/os.WEXITSTATUS(code) == 0

def test_ports_spawned_octets(test):
	test.skip(True)
	# On mac, the child process thrashes in the am cycle with bogus EVFILT_WRITE events.
	# It's as if the EV_CLEAR flag was ignored for a socket sent over the socketpair().
	am = common.ArrayActionManager()

	channels = am.array.rallocate('ports://spawn/bidirectional')

	# fork
	# spawn descriptors
	# manage in each
	# repeat:
	# spawn octets in parent
	# send to child, acquire local
	# child acquires new octets descriptors common.Objects
	# parent sends objects to child
	# child echos until

	pid = os.fork()
	if pid == 0:
		try:
			am.array.void()
			child = common.Endpoint(channels[2:])
			channels[0].port.shatter()
			del channels

			with am.thread(), am.manage(child):
				# read a descriptor from the parent

				child.setup_read(32)
				for x in am.delta():
					if child.read_payload_int:
						break

				sock = child.read_payload_int[0]
				ours = os.dup(sock)
				channels = am.array.rallocate('octets://acquire/socket', ours)
				# echo everything they send us.
				channels[0].port.raised()
				objects = common.Objects(channels)
				with am.manage(objects):
					common.child_echo(am, objects)
		except:
			import traceback
			traceback.print_exc()
			os._exit(7)
		finally:
			os._exit(0)
	else:
		parent = common.Endpoint(channels[:2])
		channels[2].port.shatter() # child's copy
		del channels

		echos = [
			2, 3,
			[x**2 for x in range(200)],
			"foo", "bar",
			"data!" * 200,
		]

		with am.thread(), am.manage(parent):
			comchannels = am.array.rallocate('octets://spawn/bidirectional')

			ours = comchannels[:2]
			theirs = comchannels[2:]
			objects = common.Objects(ours)

			r = parent.write_channel.rallocate(1)
			r[0] = theirs[0].port.id
			parent.setup_write(r)
			theirs[0].port.leak()
			del theirs

			for x in am.delta():
				if parent.units_written:
					break

			for echo in echos:
				# check that the child is still around
				_pid, code = os.waitpid(pid, os.P_NOWAIT)
				if os.WIFEXITED(code):
					test/os.WEXITSTATUS(code) == 0
					test/"child exited" == "too early"
					break
				objects.send(echo)

				for x in am.delta():
					if objects.read_channel.terminated:
						_pid, code = os.waitpid(pid, 0)
						test/os.WEXITSTATUS(code) == 0
					if objects.received_objects:
						break

					test/echo == objects.received_objects[0]
					objects.clear()
			else:
				# exit child at end of "echos"
				objects.send(None)
				am.array.force()
				_pid, code = os.waitpid(pid, 0)
				test/os.WEXITSTATUS(code) == 0

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
