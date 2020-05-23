"""
# Limited tests as the information retrieved is expected to vary.
"""
from .. import query as module

def test_paths(test):
	"""
	# - &module.paths
	"""
	i = module.paths()
	l = list(i)

def test_executables_cat(test):
	"""
	# - &module.executables
	"""
	i = module.executables('cat')
	l = list(i)
	test/len(l) >= 1 # No cat?

def test_executables_rm(test):
	"""
	# - &module.executables
	"""
	i = module.executables('rm')
	l = list(i)
	test/len(l) >= 1 # No rm?

def test_username(test):
	"""
	# - &module.username
	"""
	user = module.username()
	test/user != None

def test_usertitle(test):
	"""
	# - &module.usertitle
	"""
	title = module.usertitle()
	test/title != None

def test_shell(test):
	"""
	# - &module.shell
	"""
	sh = module.shell()
	test/sh != None

if __name__ == '__main__':
	import sys; from ...test import core as libtest
	libtest.execute(sys.modules[__name__])
