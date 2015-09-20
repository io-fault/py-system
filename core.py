import operator
import collections

class Sever(BaseException):
	"""
	Exception used to signal thread kills.
	"""
	__kill__ = True

class ContainerException(Exception):
	"""
	Common containment Exception.
	"""

class Containment(ContainerException):
	"""
	Raised when an open call attempts to access a contained exception.

	&.__cause__ contains the original, contained, exception.
	"""

class ContainerError(ContainerException):
	'Base class for **errors** involving containers'
class VoidError(ContainerError):
	'Raised when access to an absent resource occurs.'
class NoExceptionError(VoidError):
	'Raised when a Return is contained, but the Exception was accessed.'
class NoReturnError(VoidError):
	'Raised when an Exception is contained, but the Return was accessed.'

##
# Python Callable Result Containers
##

class Container(tuple):
	"""
	An object that contains either the returned object or the raised exception.
	"""
	#: Whether or not the Contained object was raised.
	__slots__ = ()
	failed = None
	contained = property(operator.itemgetter(0), doc = 'The contained object.')

	_fields = ('contained',)
	@property
	def __dict__(self, OrderedDict = collections.OrderedDict, _fields = _fields):
		return OrderedDict(zip(_fields, self))

	def self(self):
		"""
		Return the container itself; useful for cases where a reference is needed.
		"""
		return self

	def shed(self):
		"""
		Given an object contained within a container, return the innermost
		*Container*.
		"""
		c = self
		while isinstance(c.contained, Container):
			c = c.contained
		return c

	def open(self):
		"""
		Open the container with the effect that the original Contained callable
		would have. If the contained object was raised as an exception, raise the
		exception. If the contained object the object returned by the callable,
		return the contained object.
		"""
		raise VoidError(self)
	__call__ = open

	def inject(self, generator):
		"""
		Given an object supporting the generator interface, `throw` or `send` the
		contained object based on the Container's
		:py:attr:`Container.failed` attribute.
		"""
		raise VoidError()

	def exception(self):
		"""
		Open the container returning the exception raised by the Contained
		callable.

		An exception, :py:class:`.VoidError`, is raised iff the
		Contained object is not an exception-result.
		"""
		raise VoidError()

	def returned(self):
		"""
		Open the container returning the object returned by the Contained
		callable.

		An exception, :py:class:`.VoidError`, is raised iff the
		Contained object is not a return-result.
		"""
		raise VoidError()

	def endpoint(self, callback):
		"""
		Give the container to the callback.

		This method exists explicitly to provide interface consistency with Deliveries.
		Rather than conditionally checking what was returned by a given function that
		may be deferring subsequent processing, a container can be returned and immediately
		passed to the next call.
		"""
		return callback(self)

class ContainedReturn(Container):
	"""
	The Container type a return-result.

	See :py:class:`Container` for details.
	"""
	__slots__ = ()
	failed = False

	def open(self):
		return self[0]
	returned = open
	__call__ = open

	def exception(self):
		raise NoExceptionError(self)

	def inject(self, generator):
		return generator.send(self[0])

class ContainedRaise(Container):
	"""
	The Container type for an exception-result.

	See :py:class:`Container` for details.
	"""
	__slots__ = ()
	failed = True

	def _prepare(self):
		contained_exception, traceback, why = self
		contained_exception.__traceback__ = traceback
		contained_exception.__cause__, contained_exception.__context__ = why
		contained = Containment()
		contained.container = self
		contained.__cause__ = contained_exception
		return (contained, contained_exception)

	def open(self):
		containment, contained_exception = self._prepare()
		raise containment from contained_exception # opened a contained raise
	__call__ = open

	def returned(self):
		raise NoReturnError(self.contained)

	def exception(self):
		return self[0]

	def inject(self, generator):
		contained, contained_exception = self._prepare()
		self.__class__ = Container
		generator.throw(Contained, contained, None)

def contain(callable, *args,
	ContainedReturn = ContainedReturn,
	ContainedRaise = ContainedRaise,
	BaseException = BaseException,
	getattr = getattr
):
	"""
	contain(callable, *args)

	:param callable: The object to call with the given arguments.
	:type callable: :py:class:`collections.Callable`
	:param args: The positional arguments to pass on to `callable`.
	:returns: The Contained result.

	Construct and return a Container suitable for the fate of the given
	callable executed with the given arguments.

	The given callable is only provided with positional parameters. In cases
	where keywords need to be given, :py:class:`functools.partial` should be used prior to
	calling `contain`.
	"""
	try:
		return ContainedReturn((callable(*args),None,None)) ### Exception was Contained ###
	except BaseException as exc:
		# *All* exceptions are trapped here, save kills.
		if getattr(exc, '__kill__', None) is True:
			raise
		return ContainedRaise((exc,exc.__traceback__,(exc.__cause__,exc.__context__)))

def compose(*args,
		str=str, len=len, dict=dict, zip=zip, exec=exec,
		_model = ("def COMPOSITION(x,", "):\n\treturn ")
	):
	'return a composition of the given callable arguments'

	# not necessary, but let's avoid creating superfluous
	# function objects for compose() and compose(f)
	nargs = len(args)

	if not nargs:
		# no transformation is a reflection
		return (lambda x: x)
	elif nargs == 1:
		# not composition necessary
		return args[0]

	names = ['f' + str(i) for i in range(nargs)]
	locals = dict(zip(names, args))
	code = _model[0] + ','.join(['='.join((y,y)) for y in names]) + _model[1]
	code = code + '('.join(names) + '(x)' + (')' * (nargs-1))
	locals['__name__'] = "<function-composition>"
	exec(code, locals, locals)
	composition = locals["COMPOSITION"]
	composition.transforms = args
	return composition


