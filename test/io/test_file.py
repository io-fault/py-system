import os
import os.path
import tempfile
import sys
from .. import kernel
from .. import library as lib
from . import common

def test_invalid_address(test):
	try:
		J = kernel.Array()
		with test/TypeError as exc:
			J.rallocate('octets://file/read', 123)
		with test/TypeError as exc:
			J.rallocate('octets://file/read', ())
	finally:
		J.void()

def test_endpoints(test):
	ep = kernel.Endpoint('file', '/')
	test/str(ep) == '/'
	test/ep.port == None

	ep = kernel.Endpoint('file', '/foo')
	test/str(ep) == '/foo'
	test/ep.interface == '/foo'

def test_junction_rallocate(test):
	requests = [
		('octets', 'file', 'read'),
		('octets', 'file', 'overwrite'),
		('octets', 'file', 'append'),
		'octets://file/read',
		'octets://file/overwrite',
		'octets://file/append',
	]

	# be sure to hit the root path here
	# as file addressing always uses O_CREAT
	J = kernel.Array()
	for x in requests:
		t = J.rallocate(x, '/')
		test/t.port.error_name == 'EISDIR'
		J.acquire(t)

	ep = kernel.Endpoint('file', '/')
	for x in requests:
		t = J.rallocate(x, ep)
		test/t.port.error_name == 'EISDIR'
		J.acquire(t)

	J.terminate()
	with J:
		pass

def file_test(test, jam, path, apath):
	count = 0
	wrote_data = []
	thedata = b'\xF1'*128

	wr = jam.junction.rallocate(('octets', 'file', 'append'), apath)
	wr.port.raised()

	writer = common.Events(wr)
	writer.setup_write(thedata)
	test/wr.resource == thedata

	i = 0
	with jam.manage(writer):
		for x in jam.delta():
			test/writer.terminated == False
			if writer.exhaustions:
				writer.clear()
				i += 1
				if i > 32:
					writer.terminate()
					break
				writer.setup_write(thedata)
				jam.force()
	os.chmod(apath, 0o700)

	# validate expectations
	expected = (i * thedata)
	with open(path, 'rb') as f:
		actual = f.read()
	test/actual == expected
	data_size = len(thedata) * i

	# now read it back in.
	rd = jam.junction.rallocate(('octets', 'file', 'read'), apath)
	rd.port.raised() # check exception

	out = []
	reader = common.Events(rd)
	reader.setup_read(37)
	xfer_len = 0
	with jam.manage(reader):
		for x in jam.delta():

			if reader.events:
				xfer = reader.data
				out.append(xfer)
				xfer_len += len(xfer)

				if reader.exhaustions:
					reader.clear()
					reader.setup_read(93)
					jam.force()
			if reader.terminated or xfer_len >= data_size:
				out.append(reader.data)
				reader.raised()
				break
	out.append(reader.data)

	test/(bytearray(0).join(out)) == expected

	somedata = b'0' * 256

	wr = jam.junction.rallocate(('octets', 'file', 'overwrite'), apath)
	wr.port.raised()
	writer = common.Events(wr)
	writer.setup_write(somedata)
	test/wr.resource == somedata
	i = 0
	with jam.manage(writer):
		for x in jam.delta():
			if writer.exhaustions:
				writer.clear()
				i += 1
				if i > 32:
					writer.terminate()
					break
				writer.setup_write(somedata)
				jam.force()

	expected = (i * somedata)
	with open(path, 'rb') as f:
		actual = f.read()
	test/actual == expected

	# now read it back in.
	data_size = len(expected)
	xfer_len = 0
	rd = jam.junction.rallocate(('octets', 'file', 'read'), apath)
	out = []
	reader = common.Events(rd)
	reader.setup_read(17)
	with jam.manage(reader):
		for x in jam.delta():
			if reader.events:
				xfer = reader.data
				out.append(xfer)
				xfer_len += len(xfer)

				if reader.exhaustions:
					reader.clear()
					reader.setup_read(73)
					jam.force()
			if reader.terminated or xfer_len >= data_size:
				out.append(reader.data)
				reader.raised()
				break
	out.append(reader.data)
	test/(bytearray(0).join(out)) == expected

def test_file(test):
	jam = common.ArrayActionManager()
	with jam.thread(), tempfile.TemporaryDirectory() as d:
		path = os.path.join(d, "wfile")
		file_test(test, jam, path, path)

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
