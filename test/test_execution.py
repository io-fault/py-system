from .. import files
from .. import execution as module
from .test_kernel import perform_cat

def test_PInvocation(test):
	data = b"data sent through a cat pipeline\n"
	for count in range(0, 16):
		s = module.PInvocation.from_commands(
			*([('/bin/cat', 'cat')] * count)
		)
		pl = s()
		out, status = perform_cat(pl.process_identifiers, pl.input, pl.output, data, *pl.standard_errors.values())
		test/out == data
		test/len(status) == count

def test_parse_sx_plan_empty(test):
	"""
	# - &module.parse_sx_plan
	"""
	test/module.parse_sx_plan("") == ([], "", [])
	test/module.parse_sx_plan(" ") == ([], "", [])

def test_parse_sx_plan_env(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"PATH=/reset\n" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n"

	test/module.parse_sx_plan(sample) == ([('PATH', "/reset")], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_multiple_env(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"PATH=/reset\n" + \
		"OPTION=data\n" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n"

	test/module.parse_sx_plan(sample) == ([('PATH', "/reset"), ('OPTION', "data")], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_no_env(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_env_unset(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"VAR\n" \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n"

	test/module.parse_sx_plan(sample) == ([('VAR', None)], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_newlines(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n" + \
		"\t\\1\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file\n"])

def test_parse_sx_plan_newlines_suffix(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n" + \
		"\t\\1 suffix\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file\nsuffix"])

def test_parse_sx_plan_zero_newlines(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n" + \
		"\t\\0 suffix\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/filesuffix"])

def test_parse_sx_plan_plural_newlines(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n" + \
		"\t\\3 suffix\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file\n\n\nsuffix"])

def test_parse_sx_plan_no_op(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t|/file\n" + \
		"\t\\0\n" + \
		"\t\\0\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_unknown_qual(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t|cat\n" + \
		"\t?/file\n"

	test/ValueError ^ (lambda: module.parse_sx_plan(sample))

def test_serialize_sx_plan_escapes(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = (
		[('ENV', 'env-string')],
		'/bin/cat',
		[
			"-f", "FILE",
			"\nsuffix",
		]
	)

	sxp = ''.join(module.serialize_sx_plan(sample))
	test/sxp.split('\n') == [
		"ENV=env-string",
		"/bin/cat",
		"\t|-f",
		"\t|FILE",
		"\t|",
		"\t\\1 suffix",
		"",
	]

def test_serialize_sx_plan_none(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = (
		[('ENV', 'env-string'), ('ZERO', None)],
		'/bin/cat',
		[
			"-f", "FILE",
			"\nsuffix",
		]
	)

	sxp = ''.join(module.serialize_sx_plan(sample))
	test/sxp.split('\n') == [
		"ENV=env-string",
		"ZERO",
		"/bin/cat",
		"\t|-f",
		"\t|FILE",
		"\t|",
		"\t\\1 suffix",
		"",
	]

def test_sx_plan_space_separated_fields(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = (
		[('ENV', 'env-string'), ('ZERO', None)],
		'/bin/cat',
		[
			"-f", "FILE",
			"-n", "NUMBER",
			"captured spaces   ",
			"-ab", "-cde" + "\nsuffix",
		]
	)

	source = '\n'.join([
		"ENV=env-string",
		"ZERO",
		"/bin/cat",
		"\t: -f FILE -n NUMBER  ",
		"\t|captured spaces   ",
		"\t: -ab -cde",
		"\t\\1 suffix",
		"",
	])

	test/sample == module.parse_sx_plan(source)

def test_Platform_init(test):
	"""
	# - &module.Platform.__init__
	"""
	p = module.Platform('system-identifier')
	test/p.system == 'system-identifier'
	test/len(p.architectures) == 0
	test/len(p.synonyms) == 0
	test/len(p.plans) == 0

def test_Platform_features(test):
	"""
	# - &module.Platform.prepare
	# - &module.Platform.identify
	# - &module.Platform.priority
	# - &module.Platform.sections
	"""
	p = module.Platform('system-identifier')
	p.extend([('machine-class', ('alias',), ({}, 'system-command', []))])
	test/len(p.architectures) == 1

	test/p.identify('alias') == 'machine-class'
	expect = ({}, 'system-command', ['factor.path', 'arg1'])
	test/expect == p.prepare('machine-class', 'factor.path', ['arg1'])
	test/1 == p.priority('machine-class')

	p2 = module.Platform(p.system)
	test/p2.system == p.system
	p2.extend(p.sections())
	test/p2.architectures == p.architectures
	test/p2.synonyms == p.synonyms
	test/p2.plans == p.plans

def pfi(path, pairs):
	plans = (path/'plans').fs_mkdir()
	archs = (path/'architectures')

	anames = '\n'.join(x[0] for x in pairs)
	archs.fs_store(anames.encode('utf-8') + b'\n')

	for a, exe in pairs:
		sxp = module.serialize_sx_plan(([], exe, ['-F']))
		(plans/a).fs_store(''.join(sxp).encode('utf-8'))

def test_Platform_from_directory(test):
	"""
	# - &module.Platform.from_directory
	"""
	td = test.exits.enter_context(files.Path.fs_tmpdir())
	path = (td/'pf').fs_mkdir()

	empty = module.Platform.from_directory(path)

	(path/'system').fs_store(b'system-identifier\n')
	sysonly = module.Platform.from_directory(path)
	test/'system-identifier' == sysonly.system

	(path/'architectures').fs_store(b'machine-class host\n')

	sxp = module.serialize_sx_plan((
		[], '/bin/dispatch-system', ['-F']
	))
	((path/'plans').fs_mkdir()/'machine-class').fs_store(''.join(sxp).encode('utf-8'))

	single = module.Platform.from_directory(path)
	env, exe, args = single.prepare('machine-class', 'factor.path', ['argn'])
	test/['-F', 'factor.path', 'argn'] == args

def test_Platform_from_system(test):
	"""
	# - &module.Platform.from_system
	"""
	td = test.exits.enter_context(files.Path.fs_tmpdir())
	path = (td/'pf-1').fs_mkdir()
	(path/'system').fs_store(b'nothing\n')

	pfi(path, [
		('machine-class', '/bin/dispatch-system'),
	])

	single = module.Platform.from_system('nothing', [path])
	env, exe, args = single.prepare('machine-class', 'factor.path', ['argn'])
	test/['-F', 'factor.path', 'argn'] == args

	# Test filtering.
	empty = module.Platform.from_system('invalid', [path])
	test/0 == len(empty.architectures)

	path2 = (td/'pf-2').fs_mkdir()
	(path2/'system').fs_store(b'something\n')
	pfi(path2, [
		('machine-class-1', '/bin/dispatch-something-1'),
		('machine-class-2', '/bin/dispatch-something-2'),
	])

	spf = module.Platform.from_system('nothing', [path, path2])
	test/1 == len(spf.architectures)
	spf = module.Platform.from_system('something', [path, path2])
	test/2 == len(spf.architectures)

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
