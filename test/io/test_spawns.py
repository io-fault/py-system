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
	jam = common.ArrayActionManager()

	with jam.thread():
		# constructors
		for exchange in common.transfer_cases:
			p1 = os.pipe()
			p2 = os.pipe()

			cr = jam.junction.rallocate(req + ('input',), p1[0])
			sr = jam.junction.rallocate(req + ('input',), p2[0])
			sw = jam.junction.rallocate(req + ('output',), p1[1])
			cw = jam.junction.rallocate(req + ('output',), p2[1])

			server = common.Endpoint((sr, sw))
			test/server.write_transit.polarity == lib.polarity.output
			test/server.read_transit.polarity == lib.polarity.input

			client = common.Endpoint((cr, cw))
			test/client.write_transit.polarity == lib.polarity.output
			test/client.read_transit.polarity == lib.polarity.input

			with jam.manage(server), jam.manage(client):
				exchange(test, jam, client, server)

			test/server.transits[0].terminated == True
			test/server.transits[1].terminated == True
			test/client.transits[0].terminated == True
			test/client.transits[1].terminated == True

			if False:
				test/server.transits[0].transfer() == None
				test/server.transits[1].transfer() == None
				test/client.transits[0].transfer() == None
				test/client.transits[1].transfer() == None

			test/server.transits[0].exhausted != True
			test/server.transits[1].exhausted != True
			test/client.transits[0].exhausted != True
			test/client.transits[1].exhausted != True
	test/jam.junction.terminated == True

# two pipe()'s
def test_unidirectional(test, req = ('octets', 'spawn', 'unidirectional')):
	jam = common.ArrayActionManager()

	with jam.thread():
		for exchange in common.transfer_cases:
			cr, sw = jam.junction.rallocate(req)
			sr, cw = jam.junction.rallocate(req)
			server = common.Endpoint((sr, sw))
			test/server.write_transit.polarity == lib.polarity.output
			test/server.read_transit.polarity == lib.polarity.input

			client = common.Endpoint((cr, cw))
			test/client.write_transit.polarity == lib.polarity.output
			test/client.read_transit.polarity == lib.polarity.input

			with jam.manage(server), jam.manage(client):
				exchange(test, jam, client, server)

			test/server.transits[0].terminated == True
			test/server.transits[1].terminated == True
			test/client.transits[0].terminated == True
			test/client.transits[1].terminated == True

			if False:
				test/server.transits[0].transfer() == None
				test/server.transits[1].transfer() == None
				test/client.transits[0].transfer() == None
				test/client.transits[1].transfer() == None

			test/server.transits[0].exhausted != True
			test/server.transits[1].exhausted != True
			test/client.transits[0].exhausted != True
			test/client.transits[1].exhausted != True
	test/jam.junction.terminated == True

# one socketpair()'s
def test_bidirectional(test, req = ('octets', 'spawn', 'bidirectional')):
	jam = common.ArrayActionManager()

	with jam.thread():
		for exchange in common.transfer_cases:
			cxn = jam.junction.rallocate(req)
			server = common.Endpoint(cxn[:2])
			test/server.write_transit.polarity == lib.polarity.output
			test/server.read_transit.polarity == lib.polarity.input

			client = common.Endpoint(cxn[2:])
			test/client.write_transit.polarity == lib.polarity.output
			test/client.read_transit.polarity == lib.polarity.input

			with jam.manage(server), jam.manage(client):
				exchange(test, jam, client, server)

			test/server.transits[0].terminated == True
			test/server.transits[1].terminated == True
			test/client.transits[0].terminated == True
			test/client.transits[1].terminated == True

			if False:
				test/server.transits[0].transfer() == None
				test/server.transits[1].transfer() == None
				test/client.transits[0].transfer() == None
				test/client.transits[1].transfer() == None

			test/server.transits[0].exhausted != True
			test/server.transits[1].exhausted != True
			test/client.transits[0].exhausted != True
			test/client.transits[1].exhausted != True

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
