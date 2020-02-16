"""
# Check the implementation of filesystem routes.
"""
import sys
import os.path
from .. import files as lib
from ...time import sysclock

def test_Path(test):
	dir = os.path.dirname(os.path.realpath(__file__))
	r = lib.Path.from_absolute(os.path.realpath(__file__))
	test/r.fullpath == os.path.realpath(__file__)

	rd = r.container
	test/rd.fullpath == dir

	test/(rd/'foo').fullpath == os.path.join(dir, 'foo')
	test/lib.Path.from_absolute('/foo/bar.tar.gz').extension == 'gz'

def test_Path_repr(test):
	end = lib.Path.from_absolute('/test')
	test/repr(end).__contains__('/test') == True
	nx = lib.Path.from_absolute_parts('/usr/lib', 'python3.5m/site-packages', 'somemod.py')
	test/repr(nx).__contains__('somemod.py') == True

def test_Path_bytespath(test):
	p = lib.Path.from_absolute('/test/path')
	test/p.bytespath == b'/test/path'

def test_Path_which(test):
	test/lib.Path.which('cat') / lib.Path
	test/lib.Path.which('nosuchbin__x__') == None

def test_Path_temporary(test):
	path = None
	with lib.Path.temporary() as t:
		path = t.fullpath
		test/os.path.exists(path) == True
		test/t.is_container() == True
		test/t.get_last_modified() != None

	# temporary context over, should not exist.
	test/os.path.exists(path) == False
	test/OSError ^ t.get_last_modified

def test_Path_tree(test):
	"""
	# Test &lib.Path.tree and &lib.Path.subnodes
	"""

	t = test.exits.enter_context(lib.Path.temporary())
	f = t / 'file'
	f.init('file')
	test/f.subnodes() == ([],[]) # OSError
	test/f.tree() == ([], []) # OSError

	d = t / 'dir'
	s = d / 'subdir'
	l = s / 'file-in-subdir'
	l.init('file')

	expect = ([d, s], [f, l])
	queried = t.tree()
	for x in (expect, queried):
		x[0].sort()
		x[1].sort()

	test/queried == expect

def test_Path_real(test):
	"""
	# Test &lib.Path.real
	"""
	t = test.exits.enter_context(lib.Path.temporary())
	f = t/'doesnotexist'
	test/f.is_container() == False
	test/f.real() != f

def test_Path_replace(test):
	# Regular file replacement.
	t = test.exits.enter_context(lib.Path.temporary())
	src = t / 'srcfile'
	with src.open('wb') as f:
		f.write(b'sources')

	dst = t / 'dstfile'
	with dst.open('wb') as f:
		f.write(b'dest')

	dst.replace(src)
	with dst.open('rb') as f:
		test/f.read() == b'sources'

	src = t / 'srcdir'
	srcfile = src / 's' / 's'
	srcfile.init('file')
	with srcfile.open('wb') as f:
		f.write(b'subdir_sources')

	dst.replace(src)
	test/(dst / 's').exists() == True
	test/(dst / 's' / 's').exists() == True
	dir = dst / 's'
	test/dir.type() == 'directory'
	file = dir / 's'
	with file.open('rb') as f:
		test/f.read() == b'subdir_sources'

def test_Path_cwd(test):
	t = test.exits.enter_context(lib.Path.temporary())
	test/os.getcwd() != str(os.path.realpath(str(t)))
	with t.cwd():
		test/os.getcwd() == str(os.path.realpath(str(t)))
		test/str(lib.Path.from_cwd()) == os.path.realpath(str(t))

def test_Path_init(test):
	"""
	# Test &lib.Path.init checking that the parent directories are
	# properly created regardless of the selected type.
	"""

	t = test.exits.enter_context(lib.Path.temporary())
	d = t / 'parent' / 'subdir'
	f = t / 'parent2' / 'subfile'
	p = t / 'parent2' / 'subpipe'

	f.init('file')
	test/f.exists() == True
	test/f.type() == 'file'

	d.init('directory')
	test/d.exists() == True
	test/d.type() == 'directory'

	p.init('pipe')
	test/p.type() == 'pipe'

def test_Path_extension(test):
	f = lib.Path.from_path('test')
	test/f.extension == None

def test_Path_from_home(test):
	hd = lib.Path.home()
	test/str(hd) == os.environ['HOME']

def test_Path_size(test):
	r = lib.Path.from_path(__file__)

	d = r.container / 'test-size-info'

	f1 = d / 'one'
	f2 = d / 'two'
	f3 = d / 'three'
	f4 = d / 'four'
	test/f1.size() == 1
	test/f2.size() == 2
	test/f3.size() == 3
	test/f4.size() == 4

def test_Path_get_last_modified(test):
	"""
	# System check.
	"""
	import time

	d = test.exits.enter_context(lib.Path.temporary())
	r = d / 'last_modified_testfile'
	with r.open('w') as f:
		f.write('data\n')

	test/r.exists() == True

	mtime1 = r.get_last_modified()
	time.sleep(1.1)
	# sleep one whole second in case the filesystem's
	# precision is at the one second mark.

	with r.open('a') as f:
		f.write('appended\n')

	mtime2 = r.get_last_modified()

	test/mtime2 > mtime1
	test/mtime1.measure(mtime2) >= lib.timetypes.Measure.of(second=1)

def test_Path_set_last_modified(test):
	"""
	# System check.
	"""

	d = test.exits.enter_context(lib.Path.temporary())
	r = d / 'last_modified_testfile'
	with r.open('w') as f:
		f.write('data\n')

	test/r.exists() == True
	original_time = r.get_last_modified()

	ttime = sysclock.now().update('minute', -10, 'hour')

	r.set_last_modified(ttime)
	new_time = r.get_last_modified()

	test/new_time != original_time
	test/new_time.truncate('second') == ttime.truncate('second')

def test_Path_get_text_content(test):
	"""
	# Validate &lib.Path.get_text_content.
	"""

	d = test.exits.enter_context(lib.Path.temporary())
	r = d / 'tf'
	with open(str(r), 'w', encoding='utf-8') as f:
		f.write("data\n")
	test/r.exists() == True # sanity
	test/r.get_text_content() == "data\n"

def test_Path_set_text_content(test):
	"""
	# Validate &lib.Path.set_text_content.
	"""

	d = test.exits.enter_context(lib.Path.temporary())
	r = d / 'tf'
	test/r.set_text_content("data\n")
	with open(str(r), encoding='utf-8') as f:
		test/f.read() == "data\n"

def test_Path_since(test):
	"""
	# &lib.Path.since
	"""

	root = test.exits.enter_context(lib.Path.temporary())
	f1 = root / 'file1'
	f2 = root / 'file2'
	f3 = root / 'file3'

	files = [f1, f2, f3]
	for x in files:
		x.init('file')

	times = [x.get_last_modified() for x in files]
	times.sort(reverse=True)
	y = times[0]

	test/list(root.since(sysclock.now())) == []

	m = root.since(sysclock.now().rollback(minute=1))
	test/set(x[1] for x in m) == set(files)

def test_Path_construct(test):
	"""
	# Test the various classmethods that construct file instances.
	"""

	test/str(lib.Path.from_absolute('/')) == '/'

	context = lib.Path.from_absolute('/no/such/directory')

	test/str(lib.Path.from_relative(context, 'file')) == '/no/such/directory/file'
	test/str(lib.Path.from_relative(context, './file')) == '/no/such/directory/file'

	test/str(lib.Path.from_relative(context, '../file')) == '/no/such/file'
	test/str(lib.Path.from_relative(context, '../../file')) == '/no/file'

	# Same directory
	test/str(lib.Path.from_relative(context, '../.././././file')) == '/no/file'

	# parent references that find the limit.
	test/str(lib.Path.from_relative(context, '../../..')) == '/'
	test/str(lib.Path.from_relative(context, '../../../..')) == '/'

	# Smoke test .from_path; two branches that use prior tested methods.
	test/str(lib.Path.from_path('./file')) == os.getcwd() + '/file'
	test/str(lib.Path.from_path('file')) == os.getcwd() + '/file'
	test/str(lib.Path.from_path('/file')) == '/file'

def test_Path_basename_manipulations(test):
	t = test.exits.enter_context(lib.Path.temporary())
	f = t/'doesnotexist'
	f_archive = f.suffix('.tar.gz')
	test/f_archive.fullpath.endswith('.tar.gz') == True
	f_test_archive = f.prefix('test_')
	test/f_test_archive.identifier.startswith('test_') == True

def test_Path_properties(test):
	# executable
	sysexe = lib.Path.from_absolute(sys.executable)
	test/sysexe.executable() == True
	test/sysexe.type() == "file"

	module = lib.Path.from_absolute(__file__)
	test/module.executable() == False
	test/module.type() == "file"

	moddir = module.container
	test/moddir.type() == "directory"

def test_Path_void(test):
	"""
	# File.void operation.
	"""

	d = test.exits.enter_context(lib.Path.temporary())
	sd = d / 'subdir'
	sf = sd / 'subfile'
	sf.init('file')

	with sf.open('wb') as x:
		x.write(b'data')

	test/sd.exists() == True
	test/sf.exists() == True
	sf.void()
	test/sf.exists() == False
	sf.init('file')
	test/sf.exists() == True

	sd.void()
	test/sd.exists() == False
	test/sf.exists() == False

def test_Path_link(test):
	"""
	# Test &lib.Path.symbolic_link.
	"""
	t = test.exits.enter_context(lib.Path.temporary())

	target = t / 'file'
	with target.open('wb') as f:
		f.write(b'test file')

	# Relative Mode
	sym = t / 'symbolic'
	test/sym.exists() == False
	test/sym.is_link() == False
	sym.link(target)
	test/sym.is_link() == True

	test/sym.exists() == True
	with sym.open('rb') as f:
		test/f.read() == b'test file'
	target.void()
	test/target.exists() == False
	test/sym.exists() == False
	test/sym.is_link() == True

	common = t / 'dir' / 'subdir'
	common.init('directory')
	dst = common / 'from-1' / 'from-2' / 'file'
	src = common / 'to-1' / 'to-2' / 'to-3' / 'file'
	src.init('file')

	with src.open('wb') as f:
		f.write(b'source data')

	dst.init('file')
	dst.void()
	dst.link(src)
	test/dst.is_link() == True
	with dst.open('rb') as f:
		test/f.read() == b'source data'

def test_Path_recursive_since(test):
	"""
	# &lib.Path.since with recursive directories.
	"""
	import itertools
	ago10mins = sysclock.now().rollback(minute=10)
	thirty = sysclock.now().rollback(minute=30)

	t = test.exits.enter_context(lib.Path.temporary())
	d = t / 'dir' / 'subdir'
	f = d / 'file'
	f.init('file')
	for r in itertools.chain(*t.tree()):
		r.set_last_modified(thirty)

	f.set_last_modified(ago10mins)

	# create recursion
	l = d / 'link'
	l.link(t / 'dir')
	test/list(t.since(ago10mins.rollback(minute=10)))[0][1] == f

def test_Path_follow_links(test):
	"""
	# - &lib.Path.follow_links
	"""
	td = test.exits.enter_context(lib.Path.temporary())

	t = (td/'target.s')
	t.init('file')

	l1 = (td/'link1')
	l1.link(t)

	l2 = (td/'link2')
	l2.link(l1)

	l3 = (td/'link3')
	l3.link(l2)

	test/list(map(str, l3.follow_links())) == list(map(str, [l3, l2, l1, t]))
	test/list(map(str, l2.follow_links())) == list(map(str, [l2, l1, t]))
	test/list(map(str, l1.follow_links())) == list(map(str, [l1, t]))
	test/list(map(str, t.follow_links())) == list(map(str, [t]))

def test_Endpoint_properties(test):
	ep = lib.Endpoint.from_absolute_path('/dev')
	str(ep) == '/dev'
	str(ep.address) == '/'
	str(ep.port) == 'dev'
	test.isinstance(ep.address, lib.Path)

	ep = lib.Endpoint.from_route(lib.Path.from_absolute('/dev'))
	str(ep) == '/dev'
	str(ep.address) == '/'
	str(ep.port) == 'dev'
	test.isinstance(ep.address, lib.Path)

def test_Endpoint_target(test):
	"""
	# - &lib.Endpoint.target
	"""
	td = test.exits.enter_context(lib.Path.temporary())
	t = (td/'target.s')
	t.init('file')
	l = (td/'link1')
	l.link(t)

	ep = lib.Endpoint.from_route(l)
	test/str(ep.target()) == str(t)

	l2 = (td/'link2')
	l2.link(l)
	ep = lib.Endpoint.from_route(l2)
	test/str(ep.target()) == str(t)

	l3 = (td/'link3')
	l3.link(l2)
	ep = lib.Endpoint.from_route(l3)
	test/str(ep.target()) == str(t)
