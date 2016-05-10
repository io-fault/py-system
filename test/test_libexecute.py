"""
Validate mutability of System and Matrix and their serializability.
"""
import functools
from .. import libexecute as library
from ...routes import library as libroutes

def test_Arguments(test):
	"""
	&library.Arguments
	"""
	p = library.Arguments.from_partitions()
	test/list(p.absolute()) == []

	p.tail.append('last')
	p.command.append('first')

	test/list(p.absolute()) == ['first', 'last']

	p.options.append('middle')
	test/list(p.absolute()) == ['first', 'middle', 'last']

	p.update('tail', 'append', 'another last')
	test/list(p.absolute()) == ['first', 'middle', 'last', 'another last']

def test_Type(test):
	"""
	Use &library.Directory and &library.File to validate functionality.
	"""

	with libroutes.File.temporary() as t:
		test/library.Directory.valid(t) == True
		test/library.File.valid(t) == False

		f = t / 'file'
		f.init('file')
		test/library.Directory.valid(f) == False
		test/library.File.valid(f) == True

		d = t / 'dir'
		d.init('directory')

def test_Concatenation(test):
	"""
	&library.Concatenation
	"""
	c = library.Concatenation((123, ' ', 321))
	test/str(c) == '123 321'
	test/c == '123 321'

def test_Sequencing_prefix(test):
	"""
	&library.Sequencing.prefix feature application.

	Prefix is likely a common feature application; -L and -I being common use
	cases.
	"""
	id, od, env, op = library.Sequencing.prefix(None, None, ['test-1', 'test-2'], signal='-L')
	test/op == ('options', 'extend', ['-Ltest-1', '-Ltest-2'])
	test/id == {}
	test/od == {}
	test/env == None

	id, od, env, op = library.Sequencing.prefix(None, None, ['test-3'], signal='-I', inputs='input-type')
	test/op == ('options', 'extend', ['-Itest-3'])
	test/id['input-type'] == ['test-3']

	id, od, env, op = library.Sequencing.prefix(None, None, ['test-4'], signal='-O', outputs='output-type')
	test/op == ('options', 'extend', ['-Otest-4'])
	test/od['output-type'] == ['test-4']
	test/id == {}

def test_Sequencing_options(test):
	"""
	&library.Sequencing.options
	"""

	# The disabled_option validates that None options are thrown away.
	# Used to support options that are absent in a particular implementation.
	flags = dict(flag_one = '-f', flow_two = '-F', disabled_option = None)
	params = {'flag_one':True, 'disabled_option':True}
	id, od, env, op = library.Sequencing.options(None, None, params, **flags)

	test/op == ('options', 'extend', ['-f'])
	test/(id, od, env) == (None, None, None)

def test_Sequencing_assignments(test):
	"""
	&library.Sequencing.assignments
	"""

	accept = dict(test='--test=')
	provide = dict(test=library.libroutes.File.from_path('test'))
	id, od, env, op = library.Sequencing.assignments(None, None, provide, **accept)
	test/(id, od, env) == (None, None, None)
	test/op == ('options', 'extend', ['--test='+str(provide['test'])])

def test_Sequencing_input(test):
	"""
	&library.Sequencing.inputs
	"""
	id, od, env, op = library.Sequencing.input(None, None, ['i1', 'i2'], name='input_set')
	test/(od, env) == (None, None)
	test/id['input_set'] == ['i1', 'i2']
	test/op == ('tail', 'extend', ['i1', 'i2'])

def test_Sequencing_output(test):
	"""
	&library.Output
	"""
	id, od, env, op = library.Sequencing.output(None, None, 'o1', signal='-o', name='render_set')
	test/(id, env) == (None, None)
	test/od['render_set'] == 'o1'
	test/op == ('tail', 'extend', ('-o', 'o1'))

def test_Matrix(test):
	"""
	&library.Matrix
	"""
	matrix = library.Matrix('id', ())

def test_Command(test):
	"""
	&library.Command

	While the class is a base class, it does have some methods for
	managing the feature set.
	"""

	cat_route = library.libroutes.File.which('cat')
	matrix = library.Matrix('id', ())

	si = library.Command('exec', cat_route, None, {})

def test_Feature(test):
	"""
	&library.Feature tests.

	Feature is primarily used as a data structure, so test the sanity of properties.
	"""

	from functools import partial
	from ...xml import library as libxml
	Type = library.Feature
	xml = libxml.Serialization()

	feature = Type.construct('feature-id', library.Sequencing.options, bool, 'v1')
	test/feature.identifier == 'feature-id'
	test/feature.parameter == bool
	test/feature.apply == library.Sequencing.options
	test/feature.reference == (library.__name__, 'Sequencing.options', None)

	feature_not_partial = b''.join(feature.serialize(xml, None, None))

	feature = Type.construct('feature-2', partial(library.Sequencing.options, signal='-I'), bool, 'v1')
	test/feature.identifier == 'feature-2'
	test/feature.parameter == bool
	test/feature.apply.func == library.Sequencing.options
	test/feature.reference == (library.__name__, 'Sequencing.options', {'signal':'-I'})

	feature_with_partial = b''.join(feature.serialize(xml, None, None))

def test_Reference(test):
	"""
	&library.Reference
	"""

	m = library.Matrix('id', ())
	ir = library.libroutes.File.which('ls')
	mi = library.Command('list-path', ir, None, {})
	m.commands['list-path'] = mi
	f = library.Feature.construct('feature-id',
		functools.partial(library.Sequencing.options, enable_option='-X'),
		dict,
		'protocol-version-1',
	)
	mi.define(f)

	ref = library.Reference(m, mi)
	ref.update('feature-id', {'enable_option': True})
	#ref.update('search-feature', ['dir1', 'dir2'])
	cenv, command, args = ref.render()
	test/cenv == {}
	test/str(command) == args[0]
	test/[str(command), '-X'] == args

if __name__ == '__main__':
	from ...development import libtest; import sys
	libtest.execute(sys.modules[__name__])
