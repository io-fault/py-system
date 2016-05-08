"""
Command composition and argument parsing for system invocations.

! WARNING:
	&.libexecute's interfaces are unstable and will change.
	The current implementation is seeking to set a direction; when finalized
	this warning will be removed.

&.libexecute provides a set of classes and data structures for command composition
and argument parsing. For Python scripts and executable modules, this is a way
to expose the command interface in a structured manner that allows dependents such as
tests to compose invocations with greater certainty.

[ Command Composition ]

...

[ Argument Parsing ]

! WARNING:
	Currently, parsing is not supported by &.libexecute, and its implementation
	may change interfaces herein.

[ Probing ]

Probing is a common use-case for &.libexecute as the provided classes accommodate for defining
adaptations. However, &.libexecute does *not* provide any functionality specific to probing; rather,
probing should be managed by proposing &Feature instances to an
&Command and performing a test validating the functionality. If the check fails,
the Feature should be removed from the Command instance.

[ Properties ]

/environment
	The system environment variable's name that refers to the directory
	containing a set of serialized transformation &Matrix instances.

/Directory
	&Type instance used to provide directory parameters.
/File
	&Type instance used to provide file parameters.
/Socket
	&Type instance used to provide file system socket parameters.
/Executable
	&Type instance used to provide executable file parameters.
/Integer
	&Type instance used to refer to arbitrary integers.
/String
	&Type instance used to refer to arbitrary strings.
"""
import collections.abc
import contextlib
import typing
import functools
import operator
import itertools
import copy

from ..computation import librange
from ..computation import library as libc
from ..routes import library as libroutes
from ..xml import library as libxml

def _resolve_python_reference(string):
	# resolve the callable that manages the application of the parameters.
	# this is the function that sequences the adapted arguments.
	ir, atts = libroutes.Import.from_attributes(string)
	y = ir.module()
	for x in atts:
		y = getattr(y, x)
	return y

class Arguments(tuple):
	"""
	A partitioned sequence of arguments(A tuple of lists of arguments).

	Used to allow direct access to the prefix and suffix of a command during
	construction to manage common interface patterns regarding subcommand selection and
	trailing variable lists.

	The leading sequence is titled as &command, the middle as &options, and the final as
	&tail.

	&Arguments is defined to handle an arbitrary number of sequences, but at least three
	are required to fit the common pattern.

	&Arguments is essentially a limited &collections.OrderedDict with the ability
	to perform update instructions to select the slot.
	"""

	def update(self, slot, method, *args, methodcaller=operator.methodcaller, int=int):
		"""
		Perform the &method on the selected &slot with the given &args.

		Integer &slot parameters will be interpreted as index numbers; otherwise,
		it will be interpreted as an attribute.
		"""

		if isinstance(slot, int):
			s = self[slot]
		else:
			s = getattr(self, slot)

		return methodcaller(method, *args)(s)

	@classmethod
	def from_partitions(Class, count:int=3, Sequence=list):
		"""
		Create a &Sequence instance from a count of partitions; defaults to three.
		"""
		if count < 3:
			count = 3

		return Class([Sequence() for x in range(count)])

	def absolute(self, chain=itertools.chain.from_iterable):
		"""
		Return an iterator the absolute (flattened) sequence of arguments.
		"""
		return chain(self)

	@property
	def command(self):
		"""
		The leading sequence of the arguments.
		"""
		return self[0]

	@property
	def options(self):
		"""
		The middle sequence of the arguments.
		"""
		return self[1]

	@property
	def tail(self):
		"""
		The final sequence of arguments.
		"""
		return self[-1]

class Type(tuple):
	"""
	A constraint type used by &Command instances to
	validate, constrain, and construct &Feature parameters.

	&Type can also designate if the data is a collection of instances
	in order to identify the Type as sets or sequences of the &type.
	"""
	__slots__ = ()

	@property
	def type(self):
		"""
		The &Type's associated class. An instance of this class is not necessarily
		a valid instance of the &Type. The &...builtins.type expected by &valid.
		"""
		return self[0]

	def valid(self, instance):
		"""
		Whether the instance is valid according to the &Type instance.

		Iterates over the named constraints checking the given &instance with each of them.
		Any constraint returning &False cause the method to return &False, otherwise &True
		is returned.
		"""
		for name, constraint in self[2]:
			if constraint(instance) == False:
				return False
		return True

	def construct(self, obj):
		"""
		Construct an instance of the &Type according to its defined constructor.
		"""
		return self[1](obj)

	def constrain(self, *constraints):
		"""
		Create a subtype with an additional constraints.
		"""
		l = list(self[2])
		l.extend(contrainsts)
		return self.__class__((self[0], self[1], tuple(l)))

Path = Type((
	libroutes.File,
	libroutes.File.from_path,
	(),
))

Directory = Type((
	libroutes.File,
	libroutes.File.from_path,
	(('system directory does not exist', libroutes.File.is_container),),
))

File = Type((
	libroutes.File,
	libroutes.File.from_path,
	(('system file does not exist or is not a regular file', operator.methodcaller('is_regular_file')),),
))

Executable = Type((
	libroutes.File,
	libroutes.File.from_path,
	(('system file is not an executable', operator.methodcaller('executable')),),
))

Integer = Type((
	int,
	int,
	(lambda x: True),
))

String = Type((
	str,
	str,
	(lambda x: True),
))

class Concatenation(tuple):
	"""
	A tuple that concatenates the &str of the items in order to represent itself as a &str.

	Used by &Feature applications to preserve the original object
	while building the &Reference.
	"""
	__slots__ = ()

	def __str__(self):
		return ''.join(map(str, self))

	def __eq__(self, ob):
		return str(self) == ob

class Sequencing(object):
	"""
	Collection of command argument composition functions.

	Named "Sequencing" as that is the general operation taking place when a command is composed.
	Constructing data structures into a particular format often being called "serialization",
	the term "sequencing" is used to reveal the difference in that a series of tokens are
	being constructed instead of a particular series of bytes.
	"""
	__slots__ = ()

	@staticmethod
	def option(matrix, command, parameter, signal=None):
		"""
		A single option passed with a parameter passed to the command.
		The &signal keyword will be the actual option argument that precedes the &parameter.
		If the &parameter is &None, the option will not be added.
		"""
		if parameter is not None:
			return None, None, None, ('options', 'extend', (signal, parameter))
		else:
			return None, None, None, ('options', 'extend', ())

	# Common applications (of parameters).
	@staticmethod
	def options(matrix, command, parameters, **Mapping):
		"""
		A set of option flags that will be represented locally as a dictionary whose keys
		are formal parameters names defined by the keyword parameters passed to &options,
		and whose values are the actual flags that will be passed to the command.

		#!/pl/python
			fapply = functools.partial(options, enable_option='-X', enable_second_option='-Y')
		"""

		return None, None, None, ('options', 'extend', [
				Mapping[k] for k, v in parameters.items()
				if v == True and Mapping[k] is not None
			]
		)

	@staticmethod
	def selection(matrix, command, parameters, **Mapping):
		"""
		A Feature sequencing that offers a set of choices selected by a string.
		The choice will be appended to the options portion of the command.

		If the key inside the mapping is an empty string, no argument will be produced if
		selected.

		#!/pl/python
			fapply = functools.partial(selection, executable="", library="-shared")
		"""
		param = Mapping[parameters]
		return None, None, None, ('options', 'extend', [param] if param else [])

	@staticmethod
	def assignments(matrix, command, parameters, **Mapping):
		"""
		Direct mapping of assignments that are specified with the same argument.
		Does not allow specification of I/O system objects. (input/output files used or generated)

		Used to sequence arguments that are assignments whose key is the option. The
		&Mapping defines the abstraction for the option itself; the key is the exposed name,
		and the value is the actual option that will be passed to the command.
		"""

		arguments = [
			Concatenation((Mapping[k], v))
			for k, v in parameters.items()
		]

		return None, None, None, ('options', 'extend', arguments)

	@staticmethod
	def precede(matrix, command, parameters,
			slot='options', operation='extend',
			signal=None, inputs=None, outputs=None
		):
		"""
		Precede each string with the &signal.

		[ Parameters ]

		/parameters
			The sequence of stringable objects that make up the arguments.
		/signal
			The string to prefix on each string given in the &parameters sequence.
		/inputs
			The slot used to store the exact &parameters as input (dependencies) to the command.
			&None disables noting the parameters as input.
		/outputs
			The slot used to store the exact &parameters as output of the command.
			&None disables noting the parameters as output.
		"""
		inputs_dict = {}
		outputs_dict = {}

		if inputs is not None:
			inputs_dict[inputs] = parameters
		if outputs is not None:
			outputs_dict[outputs] = parameters

		args = [
			Concatenation((signal, x)) for x in parameters
		]
		args = list(libc.interlace([signal]*len(parameters), parameters))

		return inputs_dict, outputs_dict, None, (slot, operation, args)

	@staticmethod
	def prefix(matrix, command, parameters,
			slot='options', operation='extend',
			signal=None, inputs=None, outputs=None
		):
		"""
		Prefix the &signal to all the given parameters.
		Defaults to extending the `options` of the &Reference.

		[ Parameters ]

		/parameters
			The sequence of stringable objects that make up the arguments.
		/signal
			The string to prefix on each string given in the &parameters sequence.
		/inputs
			The slot used to store the exact &parameters as input (dependencies) to the command.
			&None disables noting the parameters as input.
		/outputs
			The slot used to store the exact &parameters as output of the command.
			&None disables noting the parameters as output.
		"""
		inputs_dict = {}
		outputs_dict = {}

		if inputs is not None:
			inputs_dict[inputs] = parameters
		if outputs is not None:
			outputs_dict[outputs] = parameters

		args = [
			Concatenation((signal, x)) for x in parameters
		]

		return inputs_dict, outputs_dict, None, (slot, operation, args)

	@staticmethod
	def input(matrix, command, inputs, name=None):
		"""
		A Feature application that emits the parameter field &Name
		as the inputs and arguments.

		&input presumes that the set of fields that will be identified as "command input"
		merely needs to be appended to the tail of the argument sequence.
		"""
		return {name:inputs}, None, None, ('tail', 'extend', inputs)

	@staticmethod
	def output(matrix, command, output, signal=None, name=None):
		"""
		A Feature join that specifies an output file with using a &signal flag.

		The &signal string, if not &None, will precede the feature's parameter
		extended on the tail.

		[ Parameters ]
		/signal
			The argument that will precede the &output parameter supplied
			by the &Feature.apply.
			Usually supplied by a &functools.partial application.
		/name
			The key used to refer to the &output in &Reference.outputs.
			Usually supplied by a &functools.partial application.
		"""
		return None, {name:output}, None, ('tail', 'extend', (signal, output,))

	@staticmethod
	def subcommand(matrix, command, parameter, signal=None):
		"""
		Feature application used to select a subcommand.
		"""
		return None, None, None, ('command', 'extend', (signal, parameter))

	@staticmethod
	def void(matrix, command, parameter):
		"""
		A Feature join that discards the parameter entirely.

		Used when the parameter is known to not be supported by the command, but
		may be unconditionally provided.
		"""

		return (None, None, None, None)

	@staticmethod
	def choice(martix, command, parameter, choices=None):
		"""
		A Feature sequencer that select a sequences of options from a set of choices.
		"""
		return None, None, None ('options', 'extend', choices[parameter])

class Command(object):
	"""
	An abstract Command defining the set of available features.

	Commands are unaware of their use as the feature set is what ultimately defines
	the effect.

	[Properties]

	/purpose
		The purpose of the Command. Normally this is used to identify related commands.

	/route
		The exact route to the executable that will perform the command with the system.

	/features
		The mapping of feature identifiers to &Feature instances.

	/defaults
		The dictionary that should be used to initialize command parameters from.
		&allocate.

	/qualifications
		A dictionary of settings better describing the Command.
		For transformation commands, this can be used to store
		the primary input type and the primary output type.
	"""

	def __init__(self, identifier, purpose, route, defaults, **qualifications):
		self.identifier = identifier
		self.purpose = purpose
		self.route = route
		self.qualifications = qualifications
		self.defaults = defaults or None
		self.features = {}

	def allocate(self):
		"""
		Allocate a dictionary for constructing parameters. If the command has
		not defaults, returns a new &dict instance.

		Essentially performs a deep copy of &defaults.
		"""

		if self.defaults is not None:
			return copy.deepcopy(self.defaults)
		else:
			return dict()

	def define(self, feature, identifier=None):
		"""
		Add a &Feature instance to the Transformation exposing a capability.
		"""
		self.features[identifier or feature.identifier] = feature

	def invalidate(self, identifier):
		"""
		Remove the identified feature from the transformation, &self.

		Normally used during system probing when a defined feature is found to be unavailable.
		"""
		del self.features[identifier]

	@contextlib.contextmanager
	def test(self, feature):
		try:
			self.define(feature.identifier, feature)
			yield None
		except:
			self.invalidate(feature.identifier)
		finally:
			pass

	def merge(self, transformation):
		"""
		Merge the features in &transformation into &self.
		"""
		self.features.update(transformation.features)

	@classmethod
	def construct(Class, identifier, purpose, route:libroutes.File, **features):
		assert route.executable()

		rob = Class(identifier, purpose, route, {})
		rob.features.update(features)
		return rob

	def serialize(self, serialization, configprefix='cfg:'):
		"""
		Return a generator producing an XML element describing the Command.
		"""

		xml = serialization
		quals = list(self.qualifications.items())
		quals.sort()
		pargs = [
			('identifier', self.identifier),
			('purpose', self.purpose),
			('executable', str(self.route)),
		]
		pargs.extend([(configprefix+k, v) for k, v in quals])

		feats = list(self.features.items())
		feats.sort()

		data = xml.switch('data:')

		return xml.prefixed(
			'command', itertools.chain(
				itertools.chain.from_iterable([
					f.serialize(xml, k, self.defaults.get(k))
					for k, f in feats
				]),
				xml.prefixed('defaults',
					libxml.Data.serialize(data, self.defaults),
				)
			),
			*pargs,
		)

class Feature(tuple):
	"""
	A capability of a &Command. Options manager.

	A &Feature is an object that can be assigned to a &Command instance
	in order to expose command parameters.

	Essentially, &Feature provides adaptation logic, documentation and constraints
	for system command parameters.

	Features are not strict about what and how options are managed; rather they're quite
	arbitrary in order to help accommodate outliers of normal argument parsing.
	"""

	@property
	def identifier(self):
		"""
		The identification of the &Feature used by &System in order to activate
		its functionality.
		"""
		return self[0]

	@property
	def parameter(self):
		"""
		The parameter accepted by the join operation. If configured as a dictionary
		or tuple, the collection defines the &Type of their corresponding keys, otherwise
		a &Type instance describing the one parameter.
		"""
		return self[1]

	@property
	def apply(self):
		"""
		A callable taking keyword parameters that will adapt the parameters
		for use by the command. This may set environment variables or insert
		position arguments for the command.
		"""
		return self[2]

	@property
	def reference(self):
		"""
		Reference information to the application of the feature, &apply.
		Used by &serialize to extract the parts of the partial object.
		"""
		apply = self.apply
		if isinstance(apply, functools.partial):
			f = apply.func
			args = apply.args or None
			kw = apply.keywords or None
		else:
			f = apply
			args = None
			kw = None

		return (f.__module__, f.__qualname__, kw)

	@property
	def protocol(self):
		"""
		Often &None, but optionally a hashable identifying the parameter protocol.
		"""
		return self[3]

	def xml_defaults(self, serialization, data):
		"""
		Serialize the given default parameters.

		This method takes the defaults associated with the Feature and yields out
		XML nodes (&serialization constructed nodes).
		"""
		xml = serialization

		mod, name, params = self.reference
		if name == 'options':
			yield from xml.escape(' '.join([k for k, v in data.items() if v == True]))
		else:
			if isinstance(data, list):
				for x in data:
					yield from xml.element('item', xml.escape(str(x)))
			elif isinstance(data, dict):
				for k, v in data.items():
					yield from xml.element('item', xml.escape(str(v)), key=str(k))
			else:
				yield from xml.escape(str(data))

	def serialize(self, serialization, slot, defaults, configprefix='cfg:'):
		xml = serialization
		mod, name, params = self.reference
		params = list(params.items() if params else ())
		params.sort()

		pargs = [
			('identifier', self.identifier),
			('protocol', self.protocol),
			('application', mod+'.'+name),
		]
		pargs.extend([
			(configprefix+k, str(v)) for k, v in params
		])

		return xml.prefixed('feature', None, *pargs)

	@classmethod
	def construct(Class,
			identifier:str, apply:typing.Callable, parameters:dict,
			protocol:collections.abc.Hashable=None
		):
		"""
		Construct the &Feature from the typed parameters.
		"""
		return Class((identifier, parameters, apply, protocol))

class Matrix(object):
	"""
	A set of &Command instances associated with arbitrary identifiers.

	The &Matrix provides indexed access to &Command instances that construct
	command arguments for a &..system.library.KInvocation instance. Matrices
	exist to manage a view to a set of the system's executables with respect
	to a configured set of features intended to be exercised by users of the view.

	[ Properties ]

	/identifier
		A string identifying the Matrix. Used to allow users to recognize the purpose
		of a given matrix. Often, the qualified name of the function that created
		the original instance is preferrable in order to allowing the origin to be traced.

	/environment
		The environment variables used for all system commands.

	/paths
		Executable paths used by the environment. Ultimately defines the
		(system:environment)`PATH` to be used.

	/commands
		The mapping of commands available to the &Matrix. The key
		is an arbitrary identifier, and the values are the &Command
		instances.
	"""

	def __init__(self, identifier, paths, Sequence=list, Mapping=dict):
		"""
		Initialize the &paths, &environment, and &commands attributes
		to empty collection objects.
		"""
		self.identifier = identifier
		self.executable_paths = paths
		self.environment = Mapping()
		self.commands = Mapping()
		self.context = Mapping()

	@staticmethod
	def _extract_config(element, namespace):
		"""
		Construct a dictionary from all the #config namespaced attributes.
		"""

		cfg = '{' + namespace + '}'
		configs = [
			(k[len(cfg):], v) for k, v in element.attrib.items()
			if k.startswith(cfg)
		]

		return dict(configs)

	@classmethod
	def from_xml(Class, document, ns="https://fault.io/xml/system/matrix",
			datans="https://fault.io/xml/data"
		):
		"""
		Extract the &environment and &commands from the given XML document.
		"""

		nsmap = {'s': ns, 'cfg': ns + '#config', 'd': datans}
		envvars = []
		add = envvars.append

		for element in document.xpath('/s:matrix/s:environment/s:variable', namespaces=nsmap):
			key = str(element.attrib['identifier'])
			value = str(element.attrib['value'])
			add((key, value))

		exepaths = document.xpath('/s:matrix/s:executable.paths/text()', namespaces=nsmap)
		matrix = Class(document.xpath('/s:matrix/@identifier', namespaces=nsmap), exepaths)
		matrix.environment = dict(envvars)

		commands = []
		for element in document.xpath('/s:matrix/s:command', namespaces=nsmap):
			iid = element.attrib['identifier']
			exe = element.attrib['executable']
			quals = Class._extract_config(element, nsmap['cfg'])
			features = []

			for feature in element.xpath('s:feature', namespaces=nsmap):
				# location of join function
				fid = feature.attrib['identifier']
				join_address = feature.attrib['application']

				fconfig = Class._extract_config(feature, nsmap['cfg'])

				y = _resolve_python_reference(join_address)
				join_op = functools.partial(y, **fconfig)
				f = Feature.construct(fid, join_op, None)
				features.append(f)

			cmd = Command(
				str(iid),
				element.attrib['purpose'],
				libroutes.File.from_absolute(exe), quals)
			for x in features:
				cmd.define(x)
			cmd.defaults = libxml.Data.structure(
				element.xpath('s:defaults/d:dictionary', namespaces=nsmap)[0]
			)
			matrix.commands[cmd.identifier] = cmd

		ctx = document.xpath('/s:matrix/s:context/d:*', namespaces=nsmap)[0]
		matrix.context.update(libxml.Data.structure(ctx))
		return matrix

	def serialize(self, serialization, namespace='https://fault.io/xml/system/matrix'):
		"""
		Get the XML serialized form of the Matrix for storage.
		"""
		xml = serialization
		commands = list(self.commands.items())
		commands.sort()

		yield from xml.root(
			'matrix', itertools.chain.from_iterable((
				xml.prefixed('context', libxml.Data.serialize(xml.switch('data:'), self.context)),
				xml.prefixed('environment',
					[
						xml.prefixed('variable', None, identifier=k, value=v)
						for k, v in self.environment.items()
					]
				),
				xml.prefixed('executable.paths', itertools.chain.from_iterable(
					[
						xml.encode(str(x))
						for x in self.executable_paths
					]
				)),
				itertools.chain.from_iterable(
					cmd.serialize(xml) for k, cmd in commands
				),
			)),

			('version', '0'),
			('identifier', self.identifier),
			('xmlns:cfg', namespace+'#config'),
			('xmlns:data', 'https://fault.io/xml/data'),
			namespace='https://fault.io/xml/system/matrix',
		)

class Reference(object):
	"""
	A reference to the execution of a single, particular system command.
	A &Reference is the state structure used by the invocation of a &System command.
	The &Reference holds semi-structured data that can be converted to strings suitable
	for execution.

	The primary parameters to a &..system.library.KInvocation with trace information
	about the origins of the parameters and environment variables.

	&References are associated with the transformation &Matrix and the &System command
	being constructed for execution.

	[ Properties ]

	/matrix
		The environment that the command will be ran within.

	/environment
		The environment variables that will be set when the command is executed.
		Additional variables may be defined by the &matrix, but the command local
		will always override.

	/(&Arguments)`arguments`
		The sequence of str-capable parameters that will be given to the command.
		The arguments are a sequence of tuples. Each tuple being a series of concatenations
		that will be performed in order to construct the actual callable.

	/trace
		A sequence of pairs containing a &librange.RangeSet referencing the
		arguments produced by a &Feature. The feature and its parameters are the other item.

	/outputs
		The set of output resources that will be produced by the execution of the command.

	/inputs
		The set of input resources that will be used by the command. Identified by
		the &Feature associated with the designation of the arguments or environment
		variables that cause the resource to be identified as inputs.
	"""

	def update(self, feature_id:collections.abc.Hashable, parameters):
		"""
		Update the command reference with the effects of the parameters given to the
		selected feature.

		[ Parameters ]

		/feature_id
			The feature of the command

		/parameters
			The set of parameters to give to the feature selected using &feature_id.
		"""
		feature = self.command.features[feature_id]
		inputs, outputs, env, command_parameters = feature.apply(self.matrix, None, parameters)
		origin = (feature, parameters)

		for x in (command_parameters,):
			#self.trace[('argument', tuple(x[2:]))] = origin
			if x is not None:
				self.arguments.update(*x)

		if env:
			for x in env:
				self.trace[('environment', x)] = origin
			self.environment.extend(env)

		if inputs:
			self.inputs[self] = inputs
		if outputs:
			self.outputs[self] = outputs

	def render(self):
		"""
		Return the environment, executable, and parameters to execute the referenced command.
		"""
		cr = self.command.route
		args = [str(cr)]
		args.extend(map(str, (x for x in self.arguments.absolute() if x)))

		return (
			{k:str(v) for k, v in self.environment},
			self.command.route, args
		)

	def __init__(self, matrix:Matrix, command:Command):
		"""
		Create an empty reference to a system command associated with a &Matrix.

		Usually called by &Matrix.interact.
		"""
		self.matrix = matrix
		self.command = command

		self.environment = []
		self.arguments = Arguments.from_partitions()

		# title -> route or set of routes.
		self.outputs = {}
		self.inputs = {}

		self.trace = {}

