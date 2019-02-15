import os
from .. import library as lib
from . import common
from .. import kernel

def test_invalid_address(test):
	try:
		J = kernel.Array()
		with test/TypeError as exc:
			J.rallocate('octets://acquire/socket', "foobar")
	finally:
		J.void()

# two pipe() created by os.pipe(), but passed into octets/descriptor/input|output
def test_pipe(test, req = ('octets', 'acquire')):
	am = common.ArrayActionManager()

	with am.thread():
		# constructors
		for exchange in common.transfer_cases:
			p1 = os.pipe()
			p2 = os.pipe()

			cr = am.array.rallocate(req + ('input',), p1[0])
			sr = am.array.rallocate(req + ('input',), p2[0])
			sw = am.array.rallocate(req + ('output',), p1[1])
			cw = am.array.rallocate(req + ('output',), p2[1])

			server = common.Endpoint((sr, sw))
			test/server.write_channel.polarity == lib.polarity.output
			test/server.read_channel.polarity == lib.polarity.input

			client = common.Endpoint((cr, cw))
			test/client.write_channel.polarity == lib.polarity.output
			test/client.read_channel.polarity == lib.polarity.input

			with am.manage(server), am.manage(client):
				exchange(test, am, client, server)

			test/server.channels[0].terminated == True
			test/server.channels[1].terminated == True
			test/client.channels[0].terminated == True
			test/client.channels[1].terminated == True

			if False:
				test/server.channels[0].transfer() == None
				test/server.channels[1].transfer() == None
				test/client.channels[0].transfer() == None
				test/client.channels[1].transfer() == None

			test/server.channels[0].exhausted != True
			test/server.channels[1].exhausted != True
			test/client.channels[0].exhausted != True
			test/client.channels[1].exhausted != True
	test/am.array.terminated == True

# two pipe()'s
def test_unidirectional(test, req = ('octets', 'spawn', 'unidirectional')):
	am = common.ArrayActionManager()

	with am.thread():
		for exchange in common.transfer_cases:
			cr, sw = am.array.rallocate(req)
			sr, cw = am.array.rallocate(req)
			server = common.Endpoint((sr, sw))
			test/server.write_channel.polarity == lib.polarity.output
			test/server.read_channel.polarity == lib.polarity.input

			client = common.Endpoint((cr, cw))
			test/client.write_channel.polarity == lib.polarity.output
			test/client.read_channel.polarity == lib.polarity.input

			with am.manage(server), am.manage(client):
				exchange(test, am, client, server)

			test/server.channels[0].terminated == True
			test/server.channels[1].terminated == True
			test/client.channels[0].terminated == True
			test/client.channels[1].terminated == True

			if False:
				test/server.channels[0].transfer() == None
				test/server.channels[1].transfer() == None
				test/client.channels[0].transfer() == None
				test/client.channels[1].transfer() == None

			test/server.channels[0].exhausted != True
			test/server.channels[1].exhausted != True
			test/client.channels[0].exhausted != True
			test/client.channels[1].exhausted != True
	test/am.array.terminated == True

# one socketpair()'s
def test_bidirectional(test, req = ('octets', 'spawn', 'bidirectional')):
	am = common.ArrayActionManager()

	with am.thread():
		for exchange in common.transfer_cases:
			cxn = am.array.rallocate(req)
			server = common.Endpoint(cxn[:2])
			test/server.write_channel.polarity == lib.polarity.output
			test/server.read_channel.polarity == lib.polarity.input

			client = common.Endpoint(cxn[2:])
			test/client.write_channel.polarity == lib.polarity.output
			test/client.read_channel.polarity == lib.polarity.input

			with am.manage(server), am.manage(client):
				exchange(test, am, client, server)

			test/server.channels[0].terminated == True
			test/server.channels[1].terminated == True
			test/client.channels[0].terminated == True
			test/client.channels[1].terminated == True

			if False:
				test/server.channels[0].transfer() == None
				test/server.channels[1].transfer() == None
				test/client.channels[0].transfer() == None
				test/client.channels[1].transfer() == None

			test/server.channels[0].exhausted != True
			test/server.channels[1].exhausted != True
			test/client.channels[0].exhausted != True
			test/client.channels[1].exhausted != True

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
