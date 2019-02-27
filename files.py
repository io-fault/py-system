"""
# Filesystem interfaces and data structures.
"""
import os
import os.path
import sys
import shutil
import tempfile
import contextlib
import collections
import stat
import typing
import itertools
import functools
import operator

from ..time import library as libtime # Import needs to be delayed somehow.
from ..routes import core

class Path(core.Route):
	"""
	# &core.Route subclass for local filesystem paths.
	"""
	_path_separator = os.path.sep

	__slots__ = ('context', 'points',)

	@classmethod
	def from_path(Class, path:str, getcwd=os.getcwd) -> "Path":
		"""
		# Construct a &Path instance from the given absolute or relative path
		# provided for &string; if a relative path is specified, it will
		# be relative to the current working directory as identified by
		# &os.getcwd.

		# This is usually the most appropriate way to instantiate a &Path route
		# from user input. The exception being cases where the current working
		# directory is *not* the relevant context.
		"""

		if path and path[0] == '/':
			return Class.from_absolute(path)
		else:
			return Class.from_relative(Class.from_absolute(getcwd()), path)

	@classmethod
	def from_relative(Class, context:"Path", path:str, chain=itertools.chain) -> "Path":
		"""
		# Return a new Route pointing to the file referenced by &path;
		# where path is a path relative to the &context &Path instance.

		# This function does *not* refer to the current working directory
		# returned by &os.getcwd; if this is desired, &from_path is the
		# appropriate constructor to use.
		"""
		s = Class._path_separator

		points = Class._relative_resolution(chain(
			context.absolute,
			path.strip(s).split(s)
		))
		return Class(None, tuple(points))

	@classmethod
	def from_absolute(Class, path:str, tuple=tuple) -> "Path":
		return Class(None, tuple(x for x in path.split(Class._path_separator) if x))

	@classmethod
	def from_absolute_parts(Class, start:str, *paths:str) -> "Path":
		ps = Class._path_separator

		ini = start.split(ps)
		if ini and not ini[0]:
			ini = ini[1:]

		current = Class(None, tuple(ini))
		for p in paths:
			current = Class(current, tuple(p.split(ps)))

		return current

	@classmethod
	def from_cwd(Class, *points:str, getcwd=os.getcwd) -> "Path":
		"""
		# Return a new Route to the current working directory.

		# The returned Route's `context` is the current working directory path,
		# and the &points as the sequence of following identifiers.
		"""

		return Class(Class.from_absolute(getcwd()), points)

	@classmethod
	def home(Class) -> "Path":
		"""
		# Return a new Route to the home directory defined by the environment.

		# The returned Route's &context is the HOME path.
		"""

		return Class(Class.from_absolute(os.environ['HOME']), ())

	@classmethod
	@contextlib.contextmanager
	def temporary(Class, TemporaryDirectory=tempfile.mkdtemp) -> "Path":
		"""
		# Create a temporary directory at the route using a context manager.
		# This is a wrapper around &tempfile.TemporaryDirectory that returns a &Path.

		# The use of specific temporary files is avoided as they have inconsistent
		# behavior on certain platforms.

		# A &Path route to the temporary directory is returned on entrance,
		# so the (keyword)`as` target *must* be specified in order to refer
		# files inside the directory.
		"""

		d = TemporaryDirectory()
		try:
			r = Class.from_absolute(d)
			yield Class(r, ())
		finally:
			r.void()

	@classmethod
	def which(Class, exe, dirname=os.path.dirname) -> "Path":
		"""
		# Return a new Route to the executable found by which.
		"""

		rp = shutil.which(exe)
		if rp is None:
			return None

		dn = dirname(rp)

		return Class(Class.from_absolute(dn), (rp[len(dn)+1:],))

	def __repr__(self):
		parts = [self.points]
		cur = self

		while cur.context is not None:
			cur = cur.context
			parts.append(cur.points)

		string = ','.join([repr('/'.join(y)) for y in reversed(parts[:-1])])
		start = repr(self._path_separator.join(parts[-1]))[1:-1]

		return "{0}.{1}.from_absolute_parts('/{2}',{3})".format(__name__, self.__class__.__name__, start, string)

	def __str__(self):
		return self.fullpath

	@property
	def fullpath(self, sep=os.path.sep) -> str:
		"""
		# Returns the full filesystem path designated by the route.
		"""

		if self.context is not None:
			# let the outermost context handle the root /, if any
			prefix = self.context.fullpath
			if not self.points:
				return prefix
		else:
			if not self.points:
				return sep
			# Covers the case for a leading '/'
			prefix = ''

		rpath = sep.join(self.points)

		return sep.join((prefix, rpath))

	@property
	def bytespath(self, encoding=sys.getfilesystemencoding()) -> bytes:
		"""
		# Returns the full filesystem path designated by the route as a &bytes object
		# returned by encoding the &fullpath in &sys.getfilesystemencoding with
		# `'surrogateescape'` as the error mode.
		"""

		return self.fullpath.encode(encoding, "surrogateescape")

	def suffix(self, appended_suffix) -> "Path":
		"""
		# Modify the name of the file adding the given suffix.

		# Returns a new &Path Route.
		"""

		*prefix, basename = self.points
		prefix.append(basename + appended_suffix)

		return self.__class__(self.context, tuple(prefix))

	def prefix(self, s) -> "Path":
		"""
		# Modify the name of the file adding the given prefix.

		# Returns a new &Path Route.
		"""

		*prefix, basename = self.points
		prefix.append(s + basename)

		return self.__class__(self.context, tuple(prefix))

	@property
	def extension(self):
		"""
		# Return the dot-extension of the file path.

		# Given a route to a file with a '.' in the final point, return the remainder of
		# the string after the '.'. Dot-extensions are often a useful indicator for the
		# consistency of the file's content.
		"""

		i = self.identifier
		p = i.rfind('.')
		if p == -1:
			return None

		return i[p+1:]

	_type_map = {
		stat.S_IFIFO: 'pipe',
		stat.S_IFLNK: 'link',
		stat.S_IFREG: 'file',
		stat.S_IFDIR: 'directory',
		stat.S_IFSOCK: 'socket',
		stat.S_IFBLK: 'device',
		stat.S_IFCHR: 'device',
	}

	def type(self, ifmt=stat.S_IFMT, stat=os.stat, type_map=_type_map) -> str:
		"""
		# The kind of node the route points to. Transforms the result of an &os.stat
		# call into a string describing the (python/attribute)`st_mode` field.

		# [ Returns ]
		# - `'pipe'`
		# - `'link'`
		# - `'file'`
		# - `'directory'`
		# - `'socket'`
		# - `'device'`
		# - &None

		# Where &None means the route does not exist or is not accessible.
		"""

		try:
			s = stat(self.fullpath)
		except FileNotFoundError:
			return None

		return type_map[ifmt(s.st_mode)]

	def executable(self, get_stat=os.stat, mask=stat.S_IXUSR|stat.S_IXGRP|stat.S_IXOTH) -> bool:
		"""
		# Whether the file at the route is considered to be an executable.
		"""

		mode = get_stat(self.fullpath).st_mode
		return (mode & mask) != 0

	def is_container(self, isdir=os.path.isdir) -> bool:
		"""
		# Whether or not the &Selection is a directory.
		"""

		return isdir(self.fullpath)
	is_directory = is_container

	def is_regular_file(self):
		"""
		# Whether or not the &Selection is a regular file.
		# Uses &os.stat to query the local file system to discover the type.
		"""

		return self.type() == 'file'

	def is_link(self, islink=os.path.islink):
		"""
		# Whether the Route refers to a symbolic link.
		# Returns &False in the case of a nonexistent file.
		"""

		try:
			return islink(self.fullpath)
		except FileNotFoundError:
			return False

	def follow_links(self, readlink=os.readlink) -> typing.Iterator['Path']:
		"""
		# Iterate through the links in a chain until a non-symbolic link file is reached.

		# ! NOTE:
			# The final Path yielded may not actually exist.
		"""
		Class = self.__class__
		r = self

		while r.is_link():
			yield r

			target = readlink(str(r))

			if target[:1] == '/':
				r = Class.from_absolute(target)
			else:
				r = Class.from_relative(r.container, target)

		yield r

	def subnodes(self, listdir=os.listdir, isdir=os.path.isdir, join=os.path.join):
		"""
		# Return a pair of lists, the first being a list of Routes to
		# directories in this Route and the second being a list of Routes to non-directories
		# in this Route.

		# If the Route does not point to a directory, a pair of empty lists will be returned.
		"""

		path = self.fullpath

		try:
			l = listdir(path)
		except OSError:
			# Error indifferent.
			# User must make explicit checks to interrogate permission/existence.
			return ([], [])

		directories = []
		files = []
		for x in l:
			sub = self.__class__(self, (x,))
			if isdir(join(path, x)):
				directories.append(sub)
			else:
				files.append(sub)

		return directories, files

	def subdirectories(self):
		"""
		# Query the file system and return a sequences of routes to directories
		# contained by &self. If &self is not a directory or contains no directories,
		# an empty list will be returned.
		"""
		return self.subnodes()[0]

	def files(self):
		"""
		# Query the file system returning non-directory nodes contained by the directory &self.
		"""
		return self.subnodes()[1]

	def tree(self, Queue=collections.deque):
		"""
		# Return a directory's full tree as a pair of lists of &Path
		# instances referring to the contained directories and files.
		"""
		dirs, files = self.subnodes()
		cseq = Queue(dirs)

		while cseq:
			dir = cseq.popleft()
			sd, sf = dir.subnodes()

			# extend output
			dirs.extend(sd)
			files.extend(sf)

			# process subdirectories
			cseq.extend(sd)

		return dirs, files

	def since(self, since:libtime.Timestamp,
			traversed=None,
		) -> typing.Iterable[typing.Tuple[libtime.Timestamp, "Path"]]:
		"""
		# Identify the set of files that have been modified
		# since the given point in time.

		# The resulting &typing.Iterable does not include directories.

		# [ Parameters ]

		# /since
			# The point in time after which files and directories will be identified
			# as being modified and returned inside the result set.
		"""

		# Traversed holds real absolute paths.
		if not traversed:
			traversed = set()
			traversed.add(os.path.realpath(str(self)))
		else:
			rpath = os.path.realpath(str(self))
			if rpath in traversed:
				return
			else:
				traversed.add(rpath)

		dirs, files = self.subnodes()

		for x in files:
			mt = x.get_last_modified()
			if mt.follows(since):
				yield (mt, x)

		for x in dirs:
			yield from x.since(since, traversed=traversed)

	def real(self, exists=os.path.exists) -> "Path":
		"""
		# Return the part of the Path route that actually exists on the File system.
		"""

		x = self.__class__.from_absolute(self.fullpath)
		while x.points:
			if exists(x.fullpath):
				return x
			x = x.container

	def exists(self, exists=os.path.exists) -> bool:
		"""
		# Query the filesystem and return whether or not the file exists.

		# A Route to a symbolic link *will* return &False if the target does not exist.
		"""

		return exists(self.fullpath)

	def size(self, stat=os.stat) -> int:
		"""
		# Return the size of the file as depicted by &os.stat.

		# The &os.stat function is used to get the information.
		# &None is returned if an &OSError is raised by the call.
		"""

		return stat(self.fullpath, follow_symlinks=True).st_size

	def get_last_modified(self, stat=os.stat, unix=libtime.unix) -> libtime.Timestamp:
		"""
		# Return the modification time of the file.
		"""

		return unix(stat(self.fullpath).st_mtime)

	def set_last_modified(self, time:libtime.Timestamp, utime=os.utime):
		"""
		# Set the modification time of the file identified by the &Route.
		"""

		return utime(self.__str__(), (-1, time.select('unix')/1000))

	def get_text_content(self, encoding:str='utf-8') -> str:
		"""
		# Retrieve the entire contents of the file as a &str.
		"""
		with open(str(self), encoding=encoding) as f:
			return f.read()

	def set_text_content(self, string:str, encoding:str='utf-8') -> None:
		"""
		# Modify the regular file identified by &self to contain the given &string.
		"""
		with open(str(self), 'w', encoding=encoding) as f:
			f.write(string)

	def meta(self, stat=os.stat, unix=libtime.unix):
		"""
		# Return file specific meta data.

		# ! WARNING:
			# Preliminary API.
		"""

		st = stat(self.fullpath)

		return (unix(st.st_ctime), unix(st.st_mtime), st.st_size)

	def void(self, rmtree=shutil.rmtree, remove=os.remove):
		"""
		# Remove the entire tree or file that this &Route points to.
		# No file will survive. Unless it's not owned by the user.

		# If the Route refers to a symbolic link, only the link file will be removed.
		"""
		fp = self.fullpath

		if self.is_link():
			remove(fp)
		elif self.exists():
			if self.is_directory():
				rmtree(fp)
			else:
				remove(fp)

	def replace(self, replacement:"Path", copytree=shutil.copytree, copyfile=shutil.copy):
		"""
		# Drop the existing file or directory, &self, and replace it with the
		# file or directory at the given route, &replacement.

		# [ Parameters ]

		# /replacement
			# The route to the file or directory that will be used to replace
			# the one at &self.
		"""

		self.void()
		src = replacement.fullpath
		dst = self.fullpath

		if replacement.is_directory():
			copytree(src, dst, symlinks=True, copy_function=copyfile)
		else:
			copyfile(src, dst)

	def link(self, to:"Path", relative=True, link=os.symlink, exists=os.path.lexists):
		"""
		# Create a *symbolic* link at &self pointing to &to, the target file.

		# [ Parameters ]
		# /to/
			# The target of the symbolic link.
		# /relative/
			# Whether or not to resolve the link as a relative path.
		"""

		if relative:
			parents, points = self >> to
			target = '../' * parents
			target += '/'.join(points)
		else:
			target = str(to)

		dst = str(self)
		if exists(dst):
			os.remove(dst)

		link(target, dst)

	def init(self, type, mkdir=os.mkdir, exists=os.path.lexists):
		"""
		# Create the filesystem node described by the type parameter at this route.
		# Any directories leading up to the node will be automatically created if
		# they do not exist.

		# If a node of any type already exists at this route, nothing happens.

		# &type is one of: `'file'`, `'directory'`, `'pipe'`.
		"""

		fp = self.fullpath
		if exists(fp):
			return

		routes = []
		x = self.container
		while not exists(x.fullpath):
			routes.append(x)
			x = x.container

		# create directories
		routes.reverse()
		for x in routes:
			mkdir(x.fullpath)

		if type == "file":
			# touch the file.
			with open(fp, 'x'): # Save ACL errors, concurrent op created file
				pass
		elif type == "directory":
			mkdir(fp)
		elif type == "pipe":
			os.mkfifo(fp)

	@contextlib.contextmanager
	def open(self, *args, **kw):
		"""
		# Open the file pointed to by the route.

		# If the file doesn't exist, create it; if the directories
		# leading up to the file don't exist, create the directories too.
		"""

		f = None

		try:
			self.init('file')
			f = open(self.fullpath, *args, **kw)
			with f:
				yield f
		finally:
			pass

	def load(self, mode='rb') -> bytes:
		"""
		# Open the file, and return the entire contents as a &bytes instance.
		# If the file does not exist, an *empty bytes instance* is returned.

		# Unlike &store, this will not initialize the file.
		"""
		if not self.exists():
			return b''

		with self.open(mode) as f:
			return f.read()

	def store(self, data:bytes, mode='wb'):
		"""
		# Given a &bytes instance, &data, store the contents at the location referenced
		# by the &Route. If the file does not exist, *it will be created* along with
		# the leading directories.
		"""
		with self.open(mode) as f:
			return f.write(data)

	@contextlib.contextmanager
	def cwd(self, chdir=os.chdir, getcwd=os.getcwd) -> 'Path':
		"""
		# Context manager using the &Path route as the current working directory within the
		# context. On exit, restore the current working directory to that the operating system
		# reported:

			#!/pl/python
				root = fault.system.files.Path.from_absolute('/')
				with root.cwd() as oldcwd:
					assert str(root) == os.getcwd()

				# Restored.
				assert str(oldcwd) == os.getcwd()

		# The old current working directory is yielded as a &Path instance.
		"""

		cwd = getcwd()

		try:
			chdir(self.fullpath)
			yield self.from_absolute(cwd)
		finally:
			chdir(cwd)

class Endpoint(tuple):
	"""
	# Filesystem endpoint interface.

	# Maintains an (address, port) structure for selecting filesystem endpoints.
	# Primarily intended for use with AF_LOCAL sockets, but no constraints are enforced so regular
	# files can be selected.

	# [ Related ]
	# - &..internet.host.Endpoint
	"""
	__slots__ = ()

	@property
	def address(self) -> Path:
		return self[0]

	@property
	def port(self) -> str:
		return self[1]

	def __str__(self):
		return str(self[0]/self[1])

	@classmethod
	def from_route(Class, route:Path) -> 'Endpoint':
		return tuple.__new__(Class, (route.container, route.identifier))

	@classmethod
	def from_absolute_path(Class, path:str) -> 'Endpoint':
		route = Path.from_absolute(path)
		return tuple.__new__(Class, (route.container, route.identifier))

	def target(self) -> 'Endpoint':
		"""
		# Return a new &Endpoint referring to the final target.
		# &self if there are no links or the endpoint's Path does not exist.
		"""
		l = self[0]/self[1]
		if not l.is_link():
			return self

		for x in l.follow_links():
			l = x

		return self.from_route(l)
