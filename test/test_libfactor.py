from .. import libfactor as library

def test_roles_management(test):
	# defaults to factor
	test/library.role('void') == 'optimal'
	test/library.role('void.module') == 'optimal'

	# override default for everything in void.
	library.select('void.', 'debug')
	test/library.role('void') == 'debug'
	test/library.role('void.module') == 'debug'

	# designate exact
	library.select('void.alternate', 'test')
	test/library.role('void.alternate') == 'test'

if __name__ == '__main__':
	from .. import libtest; import sys
	libtest.execute(sys.modules[__name__])
