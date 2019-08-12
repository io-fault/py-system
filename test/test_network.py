from .. import network as module

def test_endpoint_new(test):
	with test/TypeError as exc:
		module.Endpoint()
	with test/TypeError as exc:
		module.Endpoint("ip4")
	with test/TypeError as exc:
		module.Endpoint(("ip4", ()))
	with test/TypeError as exc:
		module.Endpoint(("ip4", ('127.0.0.1', )))
	with test/ValueError as exc:
		module.Endpoint("no such domain", None)

	e = module.Endpoint("ip4", ('127.0.0.1', 0))
	test/str(e) == "[127.0.0.1]:0"
	e = module.Endpoint("ip6", ('::1', 0))
	test/str(e) == "[::1]:0"
	e = module.Endpoint("local", '/path')
	test/str(e) == "/path"
	e = module.Endpoint("local", ('/dir', 'sockname'))
	test/str(e) == "/dir/sockname"
	e = module.Endpoint("local", b'/path')
	test/str(e) == "/path"

def test_endpoint_inconsistent_pf(test):
	with test/TypeError:
		e = module.Endpoint("ip4", ('127.0.0.1', 0))
		e = module.Endpoint("ip6", e)

	with test/TypeError:
		e = module.Endpoint("ip6", ('::1', 0))
		e = module.Endpoint("ip4", e)

	with test/TypeError:
		e = module.Endpoint("ip6", ('::1', 0))
		e = module.Endpoint("local", e)

def test_endpoint_consistent_pf(test):
	e = module.Endpoint("ip4", ('127.0.0.1', 0))
	test/e == module.Endpoint("ip4", e)

	e = module.Endpoint("ip6", ('::1', 0))
	test/e == module.Endpoint("ip6", e)

	e = module.Endpoint("local", '/local')
	test/e == module.Endpoint("local", e)

def test_endpoint_ne(test):
	e = module.Endpoint("ip4", ('127.0.0.1', 0))
	test/e != module.Endpoint("ip4", ('127.0.0.2', 0))

	e = module.Endpoint("ip6", ('::1', 0))
	test/e != module.Endpoint("ip6", ('::1', 1))

	e = module.Endpoint("local", '/local')
	test/e != module.Endpoint("local", '/loca')

def test_endpoint_nicmp(test):
	e = module.Endpoint("ip4", ('127.0.0.1', 0))
	test/(e == 1) == False

def test_endpoint_only_equals(test):
	e = module.Endpoint("ip4", ('127.0.0.1', 0))
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
