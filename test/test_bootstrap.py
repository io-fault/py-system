"""
# Check bootstrapping operations.
"""
import types
from .. import files
from .. import bootstrap

def test_import_constructions(test):
	"""
	# - &bootstrap.package_import
	# - &bootstrap.module_import
	"""
	# Constraint and sanity checks.
	root = 'F'
	path = '/'
	bytecode = (lambda d, module: 'bytecode:' + d + '/' + module + '.pb')

	package_record = bootstrap.package_import(bytecode, path, root, 'project')
	test/package_record == (
		'F.project',
		'/F/project',
		'/F/project/__init__.py',
		'bytecode:/F/project/__init__.pb',
	)

	module_record = bootstrap.module_import(bytecode, path, root, 'project.factor')
	test/module_record == (
		'F.project.factor',
		'F.project',
		'/F/project/factor.py',
		'bytecode:/F/project/factor.pb',
	)

def test_load_code(test):
	"""
	# - &bootstrap.load_code
	"""
	tmp = test.exits.enter_context(files.Path.fs_tmpdir())
	src = tmp/'test-source.py'
	bc = tmp/'test-source.pb'
	src.fs_store(b'print("load-code-test")')

	srcloaded = bootstrap.load_code('test-name', str(bc), str(src))
	test.isinstance(srcloaded, types.CodeType)
	import marshal
	with open(str(bc), 'wb') as f:
		marshal.dump(srcloaded, f)

	bcloaded = bootstrap.load_code('test-name', str(bc), str(src))
	test.isinstance(srcloaded, types.CodeType)

	# Check with source file missing.
	src.fs_void()
	test/src.fs_type() == 'void'
	bcloaded = bootstrap.load_code('test-name', str(bc), str(src))
	test.isinstance(srcloaded, types.CodeType)

def test_integration(test):
	"""
	# - &bootstrap.install_root
	# - &bootstrap.install_packages
	# - &bootstrap.install_modules
	# - &bootstrap.install
	# - &bootstrap.finish
	# - &bootstrap.integrate

	# Test presumes the system project exists under a python/fault context.
	"""
	tmpdir = test.exits.enter_context(files.Path.fs_tmpdir())
	p1 = (tmpdir/'product-1').fs_mkdir()
	p2 = (tmpdir/'product-2').fs_mkdir()

	rename = (lambda x: x.capitalize())
	ctx, sysproject, *path = __name__.split('.')
	from .. import __file__ as pkgfile
	pkgfile = files.Path.from_absolute(pkgfile)
	faultpath = (pkgfile ** 3)

	from .. import identity
	sys, pyimp = identity.python_execution_context()
	sys, host = identity.root_execution_context()

	M = {}
	bfactors = bootstrap.integrate(
		str(faultpath), ctx, 'optimal', '__f-int__',
		sys, pyimp, host, 'optimal',
		str(p1), str(p2)
		modules=M
	)
	test/len(M) == (len(bootstrap.pkglist) + len(bootstrap.modlist) + 1)
