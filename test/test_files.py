"""
# Check the implementation of filesystem routes.
"""
import sys
import os.path
from .. import files as lib
from ...time import sysclock

def test_root(test):
	"""
	# Check for &.files.root presence and sanity.
	"""
	test/hasattr(lib, 'root') == True
	test.isinstance(lib.root, lib.Path)
	test/str(lib.root) == "/"

	for i in range(8):
		test/(lib.root ** i) == lib.root

def test_Path(test):
	dir = os.path.dirname(os.path.realpath(__file__))
	r = lib.Path.from_absolute(os.path.realpath(__file__))
	test/r.fullpath == os.path.realpath(__file__)

	rd = r.container
	test/rd.fullpath == dir

	test/(rd/'foo').fullpath == os.path.join(dir, 'foo')
	test/lib.Path.from_absolute('/foo/bar.tar.gz').extension == 'gz'

def test_Path_filename(test):
	p = lib.Path.from_absolute('/no/such/path')

	f = p/'data.tar.xz'
	test/p.filename == 'path'
	test/f.filename == 'data.tar.xz'
	test/f.extension == 'xz'

def test_Path_from_home(test):
	hd = lib.Path.home()
	test/str(hd) == os.environ['HOME']

def test_Path_repr(test):
	end = lib.Path.from_absolute('/test')
	test/repr(end).__contains__('/test') == True

	nx = lib.Path.from_absolute_parts('/usr/lib', 'python3.5m/site-packages', 'somemod.py')
	rstr = repr(nx)
	test/rstr.__contains__('/somemod.py') == True
	test/rstr.__contains__('/usr/') == True

def test_Path_string_cache(test):
	r = lib.Path.from_absolute('/')

	p1 = lib.Path.from_absolute('/test/string/path')
	test/lib.path_string_cache(p1) == 'test/string/path'

	p2 = r@"test//string//path"
	test/lib.path_string_cache(p2) == 'test/string/path'
	test/lib.path_string_cache(p2 ** 1) == 'test/string'
	test/lib.path_string_cache(p2 ** 2) == 'test'
	test/lib.path_string_cache(p2 ** 3) == ''

def test_Path_from_partitioned_string(test):
	p = lib.Path.from_partitioned_string("/root//prefix/stem//local/target")
	parts = p.partitions()
	test/parts == [
		('root',),
		('prefix', 'stem',),
		('local', 'target',),
	]

def test_Path_bytespath(test):
	p = lib.Path.from_absolute('/test/path')
	test/p.bytespath == b'/test/path'

def test_Path_which(test):
	test/lib.Path.which('cat') / lib.Path
	test/lib.Path.which('nosuchbin__x__') == None

def test_Path_temporary(test):
	path = None
	with lib.Path.fs_tmpdir() as t:
		path = t.fullpath
		test/os.path.exists(path) == True
		test/t.is_directory() == True
		test/t.get_last_modified() != None

	# temporary context over, should not exist.
	test/os.path.exists(path) == False
	test/OSError ^ t.get_last_modified

def test_Path_old_tree(test):
	"""
	# Test &lib.Path.tree and &lib.Path.subnodes
	"""

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	f = t / 'file'
	f.fs_init()
	test/f.subnodes() == ([],[]) # OSError
	test/f.tree() == ([], []) # OSError

	d = t / 'dir'
	s = d / 'subdir'
	l = s / 'file-in-subdir'
	l.fs_init()

	expect = ([d, s], [f, l])
	queried = t.tree()
	for x in (expect, queried):
		x[0].sort()
		x[1].sort()

	test/queried == expect

def test_Path_list(test):
	"""
	# - &lib.Path.fs_list
	"""

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	test/t.fs_list() == ([],[])
	expect = ([t/str(i) for i in range(24, 32)], [t/str(i) for i in range(16)])

	for i in range(16):
		(t/str(i)).fs_init()

	dl = t.fs_list()
	for l in dl:
		l.sort(key=(lambda x: int(x.identifier)))
	test/dl[1] == expect[1]
	test/dl[0] == []

	for i in range(24, 32):
		(t/str(i)).fs_mkdir()

	dl = t.fs_list()
	for l in dl:
		l.sort(key=(lambda x: int(x.identifier)))

	test/dl == expect

def test_Path_index(test):
	"""
	# - &lib.Path.fs_index
	# - &lib.Path.fs_list
	"""

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	f = t / 'file'
	f.fs_init()
	test/f.fs_list() == ([],[]) # OSError
	test/f.fs_index() == {}

	d = t / 'dir'
	s = d / 'subdir'
	l = s / 'file-in-subdir'
	l.fs_init()

	expect = {
		t: [f],
		d: [],
		s: [l],
	}
	queried = t.fs_index()

	test/queried == expect

def test_Path_snapshot(test):
	"""
	# - &lib.Path.fs_snapshot
	"""
	sk = (lambda x: x[2]['identifier'])

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	f = (t / 'file').fs_init()
	report = t.fs_snapshot()
	expect = [
		('data', [], {'status': os.stat(f.fullpath), 'identifier': f.identifier})
	]

	test/report == expect # single file entry

	d = (t / 'dirname').fs_mkdir()
	expect.append(
		('directory', [], {'status': os.stat(d.fullpath), 'identifier': d.identifier})
	)
	expect.sort(key=sk)
	report = t.fs_snapshot()
	report.sort(key=sk)
	test/report == expect # file and directory

def test_Path_real(test):
	"""
	# - &lib.Path.fs_real
	"""
	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	f = t/'doesnotexist'

	r = f.fs_real()
	test/f.fs_real() != f
	test/r == t

	d = (t/'subdir').fs_mkdir()
	test/d.fs_real() == d

	i = d@"i1/i2/i3/i4/i5/i6/i7/i8/i9"
	test/i.fs_real() == d

def test_Path_replace(test):
	# Regular file replacement.
	t = test.exits.enter_context(lib.Path.fs_tmpdir())
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
	srcfile.fs_init()
	with srcfile.open('wb') as f:
		f.write(b'subdir_sources')

	dst.replace(src)
	test/(dst / 's').exists() == True
	test/(dst / 's' / 's').exists() == True
	dir = dst / 's'
	test/dir.fs_type() == 'directory'
	file = dir / 's'
	with file.open('rb') as f:
		test/f.read() == b'subdir_sources'

def test_Path_chdir(test):
	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	test/os.getcwd() != str(os.path.realpath(str(t)))

	with t.fs_chdir():
		test/os.getcwd() == str(os.path.realpath(str(t)))
		test/str(lib.Path.from_cwd()) == os.path.realpath(str(t))

def test_Path_fs_init(test):
	"""
	# Test &lib.Path.fs_init checking that the parent directories are
	# properly created regardless of the selected type.
	"""

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	f = t / 'parent-2' / 'filename'

	f2 = f.fs_init(b'content')
	test/f2 == f
	test/f.exists() == True
	test/f.fs_type() == 'data'
	test/f.identifier == 'filename'

	test/f.fs_load() == b'content'
	test/f.fs_init(b'content-2').fs_load() == b'content-2'
	test/f.fs_init().fs_load() == b'content-2'

def test_Path_fs_mkdir(test):
	"""
	# Test &lib.Path.fs_mkdir checking that the parent directories are
	# properly created regardless of the selected type.
	"""

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	d = t / 'parent-2' / 'directory'

	d2 = d.fs_mkdir()
	test/d2 == d
	test/d.exists() == True
	test/d.fs_type() == 'directory'
	test/d.identifier == 'directory'

	s = (d/'subdir').fs_mkdir()
	test/s.exists() == True
	test/s.fs_type() == 'directory'
	test/s.identifier == 'subdir'

def test_Path_type_void(test):
	"""
	# - &lib.Path.fs_type
	"""

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	l = t / 'deadlink.txt'
	d = t / 'directory'
	f = t / 'data-file.txt'
	d.fs_mkdir()
	f.fs_init(b'nothing')
	l.fs_link_relative(d/'no-such-target')

	v1 = d / 'subdirectory'
	test/v1.fs_type() == 'void'

	v2 = f / 'erroneous-subfile'
	test/v2.fs_type() == 'void'

	v3 = l
	test/v3.fs_type() == 'void'

def test_Path_extension(test):
	f = lib.Path.from_path('test')
	test/f.extension == None

def test_Path_size(test):
	r = lib.Path.from_path(__file__)

	d = r.container / 'test-size-info'

	f1 = d / 'one'
	f2 = d / 'two'
	f3 = d / 'three'
	f4 = d / 'four'
	test/f1.fs_size() == 1
	test/f2.fs_size() == 2
	test/f3.fs_size() == 3
	test/f4.fs_size() == 4

def test_Path_get_last_modified(test):
	"""
	# System check.
	"""
	import time

	d = test.exits.enter_context(lib.Path.fs_tmpdir())
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

	d = test.exits.enter_context(lib.Path.fs_tmpdir())
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

	d = test.exits.enter_context(lib.Path.fs_tmpdir())
	r = d / 'tf'
	with open(str(r), 'w', encoding='utf-8') as f:
		f.write("data\n")
	test/r.exists() == True # sanity
	test/r.get_text_content() == "data\n"

def test_Path_set_text_content(test):
	"""
	# Validate &lib.Path.set_text_content.
	"""

	d = test.exits.enter_context(lib.Path.fs_tmpdir())
	r = d / 'tf'
	test/r.set_text_content("data\n")
	with open(str(r), encoding='utf-8') as f:
		test/f.read() == "data\n"

def test_Path_since(test):
	"""
	# &lib.Path.since
	"""

	root = test.exits.enter_context(lib.Path.fs_tmpdir())
	f1 = root / 'file1'
	f2 = root / 'file2'
	f3 = root / 'file3'

	files = [f1, f2, f3]
	for x in files:
		x.fs_init()

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
	"""
	# - &lib.Path.prefix_filename
	# - &lib.Path.suffix_filename
	"""
	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	f = t/'doesnotexist'
	f_archive = f.suffix_filename('.tar.gz')
	test/f_archive.fullpath.endswith('.tar.gz') == True
	f_test_archive = f.prefix_filename('test_')
	test/f_test_archive.identifier.startswith('test_') == True

def test_Path_join(test):
	"""
	# - &lib.Path.join
	"""
	test/lib.root.join() == '/'
	test/lib.root.join('file') == '/file'

	f = lib.Path.from_absolute('/var/empty')

	test/f.join('datafile') == "/var/empty/datafile"
	test/f.join('subdir', 'datafile') == "/var/empty/subdir/datafile"
	test/f.join('subdir/datafile') == "/var/empty/subdir/datafile"

	test/f.join('super', 'subdir/datafile') == "/var/empty/super/subdir/datafile"

def test_Path_properties(test):
	# executable
	sysexe = lib.Path.from_absolute(sys.executable)
	test/sysexe.executable() == True
	test/sysexe.fs_type() == 'data'

	module = lib.Path.from_absolute(__file__)
	test/module.executable() == False
	test/module.fs_type() == 'data'

	moddir = module.container
	test/moddir.fs_type() == 'directory'

def test_Path_void(test):
	"""
	# File.fs_void operation.
	"""

	d = test.exits.enter_context(lib.Path.fs_tmpdir())
	sd = d / 'subdir'
	sf = sd / 'subfile'
	sf.fs_init()

	with sf.open('wb') as x:
		x.write(b'data')

	test/sd.exists() == True
	test/sf.exists() == True
	sf.fs_void()
	test/sf.exists() == False
	sf.fs_init()
	test/sf.exists() == True

	sd.fs_void()
	test/sd.exists() == False
	test/sf.exists() == False

def test_Path_link(test):
	"""
	# Test &lib.Path.symbolic_link.
	"""
	t = test.exits.enter_context(lib.Path.fs_tmpdir())

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
	target.fs_void()
	test/target.exists() == False
	test/sym.exists() == False
	test/sym.is_link() == True

	common = t / 'dir' / 'subdir'
	common.fs_mkdir()
	dst = common / 'from-1' / 'from-2' / 'file'
	src = common / 'to-1' / 'to-2' / 'to-3' / 'file'
	src.fs_init()

	with src.open('wb') as f:
		f.write(b'source data')

	dst.fs_init()
	dst.fs_void()
	dst.link(src)
	test/dst.is_link() == True
	with dst.open('rb') as f:
		test/f.read() == b'source data'

def link_checks(test, create_link):
	t = test.exits.enter_context(lib.Path.fs_tmpdir())

	target = t / 'file'
	target.fs_init(b'test file')

	# Relative Mode
	sym = t / 'symbolic'
	test/sym.exists() == False
	test/sym.is_link() == False

	create_link(sym, target)
	test/sym.is_link() == True

	test/sym.exists() == True
	with sym.open('rb') as f:
		test/f.read() == b'test file'
	target.fs_void()
	test/target.exists() == False
	test/sym.exists() == False
	test/sym.is_link() == True

	common = t / 'dir' / 'subdir'
	common.fs_mkdir()
	dst = common / 'from-1' / 'from-2' / 'file'
	src = common / 'to-1' / 'to-2' / 'to-3' / 'file'
	src.fs_init(b'source data')

	dst.fs_init()
	dst.fs_void()
	create_link(dst, src)
	test/dst.is_link() == True
	test/dst.fs_load() == b'source data'

def test_Path_fs_relative_links(test):
	"""
	# &lib.Path.fs_link_relative.
	"""
	link_checks(test, lib.Path.fs_link_relative)

def test_Path_fs_absolute_links(test):
	"""
	# &lib.Path.fs_link_absolute.
	"""
	link_checks(test, lib.Path.fs_link_absolute)

def test_Path_recursive_since(test):
	"""
	# &lib.Path.since with recursive directories.
	"""
	import itertools
	ago10mins = sysclock.now().rollback(minute=10)
	thirty = sysclock.now().rollback(minute=30)

	t = test.exits.enter_context(lib.Path.fs_tmpdir())
	d = t / 'dir' / 'subdir'
	f = d / 'file'
	f.fs_init()
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
	td = test.exits.enter_context(lib.Path.fs_tmpdir())

	t = (td/'target.s')
	t.fs_init()

	l1 = (td/'link1')
	l1.fs_link_relative(t)

	l2 = (td/'link2')
	l2.fs_link_relative(l1)

	l3 = (td/'link3')
	l3.fs_link_relative(l2)

	test/list(map(str, l3.follow_links())) == list(map(str, [l3, l2, l1, t]))
	test/list(map(str, l2.follow_links())) == list(map(str, [l2, l1, t]))
	test/list(map(str, l1.follow_links())) == list(map(str, [l1, t]))
	test/list(map(str, t.follow_links())) == list(map(str, [t]))

def test_Path_io(test):
	"""
	# - &lib.Path.fs_load
	# - &lib.Path.fs_store
	"""
	td = test.exits.enter_context(lib.Path.fs_tmpdir())

	f = td/'test-file'
	f.fs_init(b'')
	test/f.fs_type() == 'data'

	f.fs_store(b'bytes-data')
	test/f.fs_load() == b'bytes-data'
	with open(str(f)) as fp:
		test/fp.read() == "bytes-data"

	f.fs_store(b'overwritten')
	test/f.fs_load() == b'overwritten'
	with open(str(f)) as fp:
		test/fp.read() == "overwritten"

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
	td = test.exits.enter_context(lib.Path.fs_tmpdir())
	t = (td/'target.s')
	t.fs_init()
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
