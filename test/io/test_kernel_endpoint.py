from ... import io

def test_endpoint_new(test):
	with test/TypeError as exc:
		io.Endpoint()
	with test/TypeError as exc:
		io.Endpoint("ip4")
	with test/TypeError as exc:
		io.Endpoint(("ip4", ()))
	with test/TypeError as exc:
		io.Endpoint(("ip4", ('127.0.0.1', )))
	with test/ValueError as exc:
		io.Endpoint("no such domain", None)

	e = io.Endpoint("ip4", ('127.0.0.1', 0))
	test/str(e) == "[127.0.0.1]:0"
	e = io.Endpoint("ip6", ('::1', 0))
	test/str(e) == "[::1]:0"
	e = io.Endpoint("local", '/path')
	test/str(e) == "/path"
	e = io.Endpoint("local", ('/dir', 'sockname'))
	test/str(e) == "/dir/sockname"
	e = io.Endpoint("file", '/path')
	test/str(e) == "/path"
	e = io.Endpoint("local", b'/path')
	test/str(e) == "/path"
	e = io.Endpoint("file", b'/path')
	test/str(e) == "/path"

def test_endpoint_inconsistent_pf(test):
	with test/TypeError:
		e = io.Endpoint("ip4", ('127.0.0.1', 0))
		e = io.Endpoint("ip6", e)

	with test/TypeError:
		e = io.Endpoint("ip6", ('::1', 0))
		e = io.Endpoint("ip4", e)

	with test/TypeError:
		e = io.Endpoint("ip6", ('::1', 0))
		e = io.Endpoint("local", e)

	with test/TypeError:
		e = io.Endpoint("ip6", ('::1', 0))
		e = io.Endpoint("file", e)

def test_endpoint_consistent_pf(test):
	e = io.Endpoint("ip4", ('127.0.0.1', 0))
	test/e == io.Endpoint("ip4", e)

	e = io.Endpoint("ip6", ('::1', 0))
	test/e == io.Endpoint("ip6", e)

	e = io.Endpoint("local", '/foobar')
	test/e == io.Endpoint("local", e)

	e = io.Endpoint("file", '/foobar')
	test/e == io.Endpoint("file", e)

def test_endpoint_ne(test):
	e = io.Endpoint("ip4", ('127.0.0.1', 0))
	test/e != io.Endpoint("ip4", ('127.0.0.2', 0))

	e = io.Endpoint("ip6", ('::1', 0))
	test/e != io.Endpoint("ip6", ('::1', 1))

	e = io.Endpoint("local", '/foobar')
	test/e != io.Endpoint("local", '/fooba')

	e = io.Endpoint("file", '/foobar')
	test/e != io.Endpoint("file", '/fooba')

def test_endpoint_nicmp(test):
	'not implemented result'
	e = io.Endpoint("ip4", ('127.0.0.1', 0))
	test/(e == 1) == False

def test_endpoint_only_equals(test):
	e = io.Endpoint("ip4", ('127.0.0.1', 0))
	with test/TypeError:
		e > e
	with test/TypeError:
		e < e
	with test/TypeError:
		e <= e
	with test/TypeError:
		e >= e

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
