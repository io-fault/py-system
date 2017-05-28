from .. import libfactor as module
from ...routes import library as libroutes

def test_composite(test):
	"""
	# Validate &library.composite
	"""

	test/module.composite(libroutes.Import.from_fullname(__name__)) == False
	test/module.composite(libroutes.Import.from_fullname(__package__)) == False

	prefix = module.__package__ + '.'
	ir = libroutes.Import.from_fullname(prefix + 'extensions.kernel')
	test/module.composite(ir) == True

if __name__ == '__main__':
	from .. import libtest; import sys
	libtest.execute(sys.modules[__name__])
