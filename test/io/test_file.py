import os
import os.path
import tempfile
import sys
from ... import io
from . import common

def file_test(test, am, path, apath):
	count = 0
	wrote_data = []
	thedata = b'\xF1'*128

	fd = os.open(apath, os.O_APPEND|os.O_WRONLY|os.O_CREAT)
	wr = am.array.rallocate(('octets', 'acquire', 'output'), fd)
	wr.port.raised()

	writer = common.Events(wr)
	writer.setup_write(thedata)
	test/wr.resource == thedata

	i = 0
	with am.manage(writer):
		for x in am.delta():
			test/writer.terminated == False
			if writer.exhaustions:
				writer.clear()
				i += 1
				if i > 32:
					writer.terminate()
					break
				writer.setup_write(thedata)
				am.force()
	os.chmod(apath, 0o700)

	# validate expectations
	expected = (i * thedata)
	with open(path, 'rb') as f:
		actual = f.read()
	test/actual == expected
	data_size = len(thedata) * i

	# read it back
	fd = os.open(apath, os.O_RDONLY)
	rd = am.array.rallocate(('octets', 'acquire', 'input'), fd)
	rd.port.raised() # check exception

	out = []
	reader = common.Events(rd)
	reader.setup_read(37)
	xfer_len = 0
	with am.manage(reader):
		for x in am.delta():

			if reader.events:
				xfer = reader.data
				out.append(xfer)
				xfer_len += len(xfer)

				if reader.exhaustions:
					reader.clear()
					reader.setup_read(93)
					am.force()
			if reader.terminated or xfer_len >= data_size:
				out.append(reader.data)
				reader.raised()
				break
	out.append(reader.data)

	test/(bytearray(0).join(out)) == expected

	somedata = b'0' * 256

	fd = os.open(apath, os.O_WRONLY)
	wr = am.array.rallocate(('octets', 'acquire', 'output'), fd)
	wr.port.raised()
	writer = common.Events(wr)
	writer.setup_write(somedata)
	test/wr.resource == somedata
	i = 0
	with am.manage(writer):
		for x in am.delta():
			if writer.exhaustions:
				writer.clear()
				i += 1
				if i > 32:
					writer.terminate()
					break
				writer.setup_write(somedata)
				am.force()

	expected = (i * somedata)
	with open(path, 'rb') as f:
		actual = f.read()
	test/actual == expected

	# read it back
	data_size = len(expected)
	xfer_len = 0
	fd = os.open(apath, os.O_RDONLY)
	rd = am.array.rallocate(('octets', 'acquire', 'input'), fd)
	out = []
	reader = common.Events(rd)
	reader.setup_read(17)
	with am.manage(reader):
		for x in am.delta():
			if reader.events:
				xfer = reader.data
				out.append(xfer)
				xfer_len += len(xfer)

				if reader.exhaustions:
					reader.clear()
					reader.setup_read(73)
					am.force()
			if reader.terminated or xfer_len >= data_size:
				out.append(reader.data)
				reader.raised()
				break
	out.append(reader.data)
	test/(bytearray(0).join(out)) == expected

def test_file(test):
	am = common.ArrayActionManager()
	with am.thread(), tempfile.TemporaryDirectory() as d:
		path = os.path.join(d, "wfile")
		file_test(test, am, path, path)

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
