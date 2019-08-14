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

def test_endpoint_ip4(test):
	ep = module.Endpoint.from_ip4(('127.0.0.1', 100))
	test/ep.port == 100
	test/ep.address == '127.0.0.1'

	ep = module.Endpoint.from_ip4(('0.0.0.0', -1))
	test/ep.port == 0xFFFF
	test/ep.address == '0.0.0.0'

def test_endpoint_ip4_invalid(test):
	from_ip4 = module.Endpoint.from_ip4

	with test/SystemError as exc:
		from_ip4(123)
	with test/TypeError as exc:
		from_ip4(())
	with test/TypeError as exc:
		from_ip4((1,))
	with test/TypeError as exc:
		from_ip4((321, 'type-error'))
	with test/OSError as exc:
		from_ip4(('name.com', 123))

def test_endpoint_ip4_pton_error(test):
	test.skip('EOVERRIDE' not in dir(module))
	with test/OSError as exc:
		try:
			module.EOVERRIDE['ip4_from_object'] = lambda x: (1,)
			ep = module.Endpoint.from_ip4(('127.0.0.1', 123))
		finally:
			module.EOVERRIDE.clear()
	test/exc().errno == 1

def test_endpoint_ip6_invalid_address(test):
	from_ip6 = module.Endpoint.from_ip6

	with test/SystemError as exc:
		from_ip6(123)
	with test/TypeError as exc:
		from_ip6(())
	with test/TypeError as exc:
		from_ip6((1,))
	with test/TypeError as exc:
		from_ip6((321, 'type-error'))
	with test/OSError as exc:
		from_ip6(('name.com', 123))

def test_endpoint_ip6_pton_error(test):
	test.skip('EOVERRIDE' not in dir(module))

	with test/OSError as exc:
		try:
			module.EOVERRIDE['ip6_from_object'] = lambda x: (1,)
			module.Endpoint.from_ip6(('::1', 123))
		finally:
			module.EOVERRIDE.clear()

	test/exc().errno == 1

def test_endpoint_ip6(test):
	ep = module.Endpoint('ip6', ('::1', 100))
	test/ep.port == 100
	test/ep.address == '::1'

	ep = module.Endpoint('ip6', ('0::0', -1))
	test/ep.port == 0xFFFF
	test/ep.address == '::'

if __name__ == '__main__':
	import sys; from ....test import library as libtest
	libtest.execute(sys.modules['__main__'])
