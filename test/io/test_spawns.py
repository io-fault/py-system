import os
from . import common
from ... import io

# two pipe()'s
def test_unidirectional(test):
	am = common.ArrayActionManager()

	with am.thread():
		for exchange in common.transfer_cases:
			cr, sw = common.allocpipe(am.array)
			sr, cw = common.allocpipe(am.array)
			server = common.Endpoint((sr, sw))
			test/server.write_channel.polarity == -1
			test/server.read_channel.polarity == 1

			client = common.Endpoint((cr, cw))
			test/client.write_channel.polarity == -1
			test/client.read_channel.polarity == 1

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
def test_bidirectional(test):
	am = common.ArrayActionManager()

	with am.thread():
		for exchange in common.transfer_cases:
			cxn = common.allocsockets(am.array)
			server = common.Endpoint(cxn[:2])
			test/server.write_channel.polarity == -1
			test/server.read_channel.polarity == 1

			client = common.Endpoint(cxn[2:])
			test/client.write_channel.polarity == -1
			test/client.read_channel.polarity == 1

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
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
