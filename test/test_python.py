"""
# Check the implementation of import routes.
"""
import sys
from .. import python as lib

def test_Import(test):
	# ..bit strange when dealing with nested project packages
	fullpath = __package__.split('.')
	localpath = fullpath[fullpath.index('system'):]
	local = '.'.join(fullpath[:fullpath.index('system')+1])

	# package.test
	r = lib.Import.from_fullname(__package__)
	# context reduction
	test/(~r is r) == True

	# crawl stack; closest package module is chosen
	test/lib.Import.from_context() == r

	test/r.fullname == '.'.join((__package__, ))
	test/r.module() == sys.modules[__package__]
	test/r.container.module() == sys.modules['.'.join(__package__.split('.')[:-1])]

	# stack
	r = lib.Import.from_fullname(__package__)
	modules = r.stack()
	test/len(modules) >= len(fullpath)

	test/modules[0] == r.module() # most "significant" module first
	for x in modules:
		test/x == r.module()
		r = r.container

	# floor
	r = lib.Import.from_fullname(__package__)
	test/r.floor() == lib.Import.from_fullname(local)
	test/r.floor() == r.floor().floor()

	# module that does not exist
	r = lib.Import.from_fullname('+++++nosuch_module')
	test/r.module() == None
	test/r.root == r

	# real resolution
	r = lib.Import.from_fullname(__package__ + '.' + '+++++nosuch_module')
	test/r.module() == None
	test/r.real() == lib.Import.from_fullname(__package__)

def test_Import_anchor(test):
	i = lib.Import.from_fullname(__package__)
	i = i.anchor()
	test/i.context == i.floor()

def test_Import_from_attributes(test):
	mod, attr = lib.Import.from_attributes(__package__)
	test/mod == lib.Import.from_fullname(__package__)
	test/attr == ()

def test_Import_tree(test):
	pkg = lib.Import.from_fullname(__package__)
	project = pkg.floor()
	pkgs, mods = map(set, project.tree())

	test/((project/'test') in pkgs) == True
	test/((project/'library') in mods) == True

def test_Import_get_last_modified(test):
	# This is essentally the implementation; the method is mere convenience.
	pkg = lib.Import.from_fullname(__package__)
	test/pkg.file().get_last_modified() == pkg.get_last_modified()

def test_Import_subnodes(test):
	"""
	# Test the filtering and functionality of &library.Import.subnodes.
	"""
	test.skip("not implemented")
