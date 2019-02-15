from .. import kernel

def test_endpoint_new(test):
	with test/TypeError as exc:
		kernel.Endpoint()
	with test/TypeError as exc:
		kernel.Endpoint("ip4")
	with test/TypeError as exc:
		kernel.Endpoint(("ip4", ()))
	with test/TypeError as exc:
		kernel.Endpoint(("ip4", ('127.0.0.1', )))
	with test/ValueError as exc:
		kernel.Endpoint("no such domain", None)

	e = kernel.Endpoint("ip4", ('127.0.0.1', 0))
	test/str(e) == "[127.0.0.1]:0"
	e = kernel.Endpoint("ip6", ('::1', 0))
	test/str(e) == "[::1]:0"
	e = kernel.Endpoint("local", '/path')
	test/str(e) == "/path"
	e = kernel.Endpoint("local", ('/dir', 'sockname'))
	test/str(e) == "/dir/sockname"
	e = kernel.Endpoint("file", '/path')
	test/str(e) == "/path"
	e = kernel.Endpoint("local", b'/path')
	test/str(e) == "/path"
	e = kernel.Endpoint("file", b'/path')
	test/str(e) == "/path"

def test_endpoint_inconsistent_pf(test):
	with test/TypeError:
		e = kernel.Endpoint("ip4", ('127.0.0.1', 0))
		e = kernel.Endpoint("ip6", e)

	with test/TypeError:
		e = kernel.Endpoint("ip6", ('::1', 0))
		e = kernel.Endpoint("ip4", e)

	with test/TypeError:
		e = kernel.Endpoint("ip6", ('::1', 0))
		e = kernel.Endpoint("local", e)

	with test/TypeError:
		e = kernel.Endpoint("ip6", ('::1', 0))
		e = kernel.Endpoint("file", e)

def test_endpoint_consistent_pf(test):
	e = kernel.Endpoint("ip4", ('127.0.0.1', 0))
	test/e == kernel.Endpoint("ip4", e)

	e = kernel.Endpoint("ip6", ('::1', 0))
	test/e == kernel.Endpoint("ip6", e)

	e = kernel.Endpoint("local", '/foobar')
	test/e == kernel.Endpoint("local", e)

	e = kernel.Endpoint("file", '/foobar')
	test/e == kernel.Endpoint("file", e)

def test_endpoint_ne(test):
	e = kernel.Endpoint("ip4", ('127.0.0.1', 0))
	test/e != kernel.Endpoint("ip4", ('127.0.0.2', 0))

	e = kernel.Endpoint("ip6", ('::1', 0))
	test/e != kernel.Endpoint("ip6", ('::1', 1))

	e = kernel.Endpoint("local", '/foobar')
	test/e != kernel.Endpoint("local", '/fooba')

	e = kernel.Endpoint("file", '/foobar')
	test/e != kernel.Endpoint("file", '/fooba')

def test_endpoint_nicmp(test):
	'not implemented result'
	e = kernel.Endpoint("ip4", ('127.0.0.1', 0))
	test/(e == 1) == False

def test_endpoint_only_equals(test):
	e = kernel.Endpoint("ip4", ('127.0.0.1', 0))
	with test/TypeError:
		e > e
	with test/TypeError:
		e < e
	with test/TypeError:
		e <= e
	with test/TypeError:
		e >= e

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules['__main__'])
