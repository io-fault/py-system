from .. import execution as module
from .test_kernel import perform_cat

def test_PInvocation(test):
	data = b'data sent through a cat pipeline\n'
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
		"\t:cat\n" + \
		"\t:/file\n"

	test/module.parse_sx_plan(sample) == ([('PATH', "/reset")], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_multiple_env(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"PATH=/reset\n" + \
		"OPTION=data\n" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n"

	test/module.parse_sx_plan(sample) == ([('PATH', "/reset"), ('OPTION', "data")], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_no_env(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_env_unset(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"VAR\n" \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n"

	test/module.parse_sx_plan(sample) == ([('VAR', None)], "/bin/cat", ["cat", "/file"])

def test_parse_sx_plan_newlines(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n" + \
		"\t\\1\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file\n"])

def test_parse_sx_plan_newlines_suffix(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n" + \
		"\t\\1 suffix\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file\nsuffix"])

def test_parse_sx_plan_zero_newlines(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n" + \
		"\t\\0 suffix\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/filesuffix"])

def test_parse_sx_plan_plural_newlines(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n" + \
		"\t\\3 suffix\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file\n\n\nsuffix"])

def test_parse_sx_plan_no_op(test):
	"""
	# - &module.parse_sx_plan
	"""
	sample = "" + \
		"/bin/cat\n" + \
		"\t:cat\n" + \
		"\t:/file\n" + \
		"\t\\0\n" + \
		"\t\\0\n"

	test/module.parse_sx_plan(sample) == ([], "/bin/cat", ["cat", "/file"])

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
