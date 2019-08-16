import os
import errno
from ... import io
from ... import network
from . import common

def test_io(test):
	common.stream_listening_connection(test, 'ip4', ('127.0.0.1', 0))

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
