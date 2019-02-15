"""
# This file *should* contain system.io *specific* tests.
# Primarily, invasive tests that rely on implementation specific functionality.

# Arguably, there's quite a bit of redundancy in this file.
# However, cases analyzed here that appear similar often have one-off cases
# that make it rather annoying to generalize.
"""
from ... import io

def test_port(test):
	f = io.Port(
		call = "kevent", error_code = 10, id = -1,
	)
	test/f.id == -1
	test/f.error_code == 10
	test/f.error_name == "ECHILD"
	test/f.call == "kevent"

	f = io.Port(
		call = "read", error_code = 100, id = 100,
	)
	test/f.id == 100
	test/f.error_code == 100
	test/f.call == "read"

	f = io.Port(
		call = "x", error_code = 1000, id = 1000,
	)
	test/f.id == 1000
	test/f.error_code == 1000
	test/f.call == 'INVALID'
	with test/TypeError as exc:
		io.Port(id = "nonanumber")
	f = io.Port(
		call = "read", error_code = 10, id = 1000,
	)
	with test/OSError as exc:
		f.raised()
	test.isinstance(f.exception(), OSError)

	repr(f)
	f.leak()
	f.shatter()

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
