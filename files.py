"""
# Filesystem interfaces and data structures.
"""
import os
import os.path
import sys
import contextlib
import collections
import stat
import typing
import itertools
import functools

# Moving to cached class properties.
import shutil
import tempfile

from ..context.tools import cachedcalls
from .. import routes

class Status(tuple):
	"""
	# File status interface providing symbolic names for the data packed in
	# the system's status record, &system.

	# [ Engineering ]
	# Expiremental. Helps isolate delayed imports.
	# Likely undesired noise if a stat-cache is employed by &Path.
	"""
	__slots__ = ()

	_fs_type_map = {
		stat.S_IFIFO: 'pipe',
		stat.S_IFLNK: 'link',
		stat.S_IFREG: 'data',
		stat.S_IFDIR: 'directory',
		stat.S_IFSOCK: 'socket',
		stat.S_IFBLK: 'device',
		stat.S_IFCHR: 'device',
	}

	_fs_subtype_map = {
		stat.S_IFBLK: 'block',
		stat.S_IFCHR: 'character',
	}

	@property
	def _interpret_time(self):
		from ..time.types import from_unix_timestamp
		self.__class__._interpret_time = staticmethod(from_unix_timestamp)
		return from_unix_timestamp

	@property
	def _read_user(self):
		from pwd import getpwuid
		self.__class__._read_user = staticmethod(getpwuid)
		return getpwuid

	@property
	def _read_group(self):
		from grp import getgrgid
		self.__class__._read_group = staticmethod(getgrgid)
		return getgrgid

	@classmethod
	def from_route(Class, route):
		return Class((os.stat(route), route.identifier))

	@property
	def system(self):
		"""
		# The status record produced by the system (&os.stat).
		"""
		return self[0]

	@property
	def filename(self):
		"""
		# The name of the file.
		"""
		return self[1]

	def __add__(self, operand):
		# Protect from unexpected addition.
		# tuple() + Status(...) is still possible.
		return NotImplemented

	@property
	def size(self):
		"""
		# Number of bytes contained by the file.
		"""
		return self.system.st_size

	@property
	def type(self, ifmt=stat.S_IFMT):
		"""
		# /`'directory'`/
			# A file containing other files.
		# /`'data'`/
			# A regular file.
		# /`'pipe'`/
			# A named pipe; also known as a FIFO.
		# /`'socket'`/
			# A unix domain socket.
		# /`'device'`/
			# A character or block device file.
		# /`'void'`/
			# A broken link.
		# /`'link'`/
			# Status record of a link to a file.
		"""
		return self._fs_type_map.get(ifmt(self.system.st_mode), 'unknown')

	@property
	def subtype(self, ifmt=stat.S_IFMT):
		"""
		# For POSIX-type systems, designates the kind of `'device'`: `'block'` or `'character'`.
		# Returns &None for types other than `'device'`.
		"""
		return self._fs_subtype_map.get(ifmt(self.system.st_mode))

	@property
	def created(self):
		"""
		# Time of creation; UTC. Not available on all systems.
		"""
		return self._interpret_time(self.system.st_birthtime)

	@property
	def last_modified(self):
		"""
		# Time of last modification; UTC.
		"""
		return self._interpret_time(self.system.st_mtime)

	@property
	def last_accessed(self):
		"""
		# Time of last access; UTC.
		"""
		return self._interpret_time(self.system.st_atime)

	@property
	def meta_last_modified(self):
		"""
		# Time of last status change; UTC.
		"""
		return self._interpret_time(self.system.st_ctime)

	@property
	def owner(self):
		return self._read_user(self.system.st_uid)

	@property
	def group(self):
		return self._read_group(self.system.st_gid)

	@property
	def setuid(self):
		return (self.system.st_mode & S_ISUID)

	@property
	def setgid(self):
		return (self.system.st_mode & stat.S_ISGID)

	@property
	def sticky(self):
		return (self.system.st_mode & stat.S_ISVTX)

	@property
	def executable(self, mask=stat.S_IXUSR|stat.S_IXGRP|stat.S_IXOTH) -> bool:
		"""
		# Whether the data file is considered executable by anyone.
		"""
		return (self.system.st_mode & mask) != 0 and self.type == 'data'

	@property
	def searchable(self, mask=stat.S_IXUSR|stat.S_IXGRP|stat.S_IXOTH) -> bool:
		"""
		# Whether the directory file is considered searchable by anyone.
		"""
		return (self.system.st_mode & mask) != 0 and self.type == 'directory'

@cachedcalls(32)
def path_string_cache(path):
	if path.context is not None:
		prefix = path_string_cache(path.context)
		segment = '/'.join(path.points)
		return '/'.join(x for x in (prefix, segment) if x)
	else:
		return '/'.join(path.points)

class Path(routes.Selector):
	"""
	# &routes.Selector subclass for local filesystem paths.

	# Methods starting with `fs_` perform filesystem operations.
	"""
	_path_separator = os.path.sep

	__slots__ = ('context', 'points',)

	class Void(Exception):
		"""
		# Exception thrown by methods that require files at the Path to exist.

		# Instances are using the bad path and a context argument.
		# The context argument is user data describing where the path came from.

		# [ Engineering ]
		# Expiremental. Currently, only raised by &fs_select.
		"""

		rtype = 'system-file'

		def __init__(self, badpath, context=None):
			self.path = badpath
			self.context = context

		def __str__(self):
			if self.context is not None:
				return "[%s: %s] path did not exist" %(self.context, str(self.path))
			else:
				return "[%s] path did not exist" %(str(self.path),)

		def fragments(self):
			"""
			# Construct the real portion and segment pair.

			# Calculates the part of the path that actually exists on the filesytem,
			# and the remaining invalid part.
			"""
			r = self.path.fs_real()
			return (r, self.path.segment(r))

	@classmethod
	def fs_select(Class, *path:str, pwd=None, context=None):
		"""
		# Construct a &Path from user input, the given string &path segments.

		# If the first segment starts with a `'/'`, the path will be interpreted as absolute.
		# Otherwise, the path will be interpreted as relative to the current working directory.

		# If the path does not exists, a &Void exception will be raised.

		# [ Engineering ]
		# Expiremental. Constructor intended for user input; likely replacement for from_path.
		"""
		start = path[0] if path else ''

		if start[0:1] == '/':
			fsr = Class.from_absolute(start)
		else:
			if pwd is None:
				try:
					pwd = Class.from_absolute(os.environ['PWD'])
				except KeyError:
					pwd = Class.from_absolute(os.getcwd())
			fsr = pwd @ start

		for x in path[1:]:
			fsr @= x

		if fsr.fs_type() == 'void':
			raise Class.Void(fsr, context)

		return fsr

	@classmethod
	def from_path(Class, path:str, getcwd=os.getcwd):
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
	def from_relative(Class, context, path:str, chain=itertools.chain):
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
	def from_absolute(Class, path:str, tuple=tuple):
		return Class(None, tuple(x for x in path.split(Class._path_separator) if x))

	@classmethod
	def from_absolute_parts(Class, start:str, *paths:str):
		ps = Class._path_separator

		ini = start.split(ps)
		if ini and not ini[0]:
			ini = ini[1:]

		current = Class(None, tuple(ini))
		for p in paths:
			current = Class(current, tuple(p.split(ps)))

		return current

	@staticmethod
	def _partition_string(path:str) -> typing.Iterable[typing.Sequence[str]]:
		return (x.strip('/').split('/') for x in path.split("//"))

	@classmethod
	def from_partitioned_string(Class, path:str):
		"""
		# Construct an absolute path while interpreting consecutive separators
		# as distinct partitions.
		"""
		return Class.from_partitions(Class._partition_string(path))

	def __matmul__(self, path:str):
		parts = self._partition_string(path)
		if path[:1] == "/":
			return self.from_partitions(parts)
		else:
			return self // routes.Segment.from_partitions(parts)

	@classmethod
	def from_cwd(Class, *points:str, getcwd=os.getcwd):
		"""
		# Return a new Route to the current working directory.

		# The returned Route's `context` is the current working directory path,
		# and the &points as the sequence of following identifiers.
		"""

		return Class(Class.from_absolute(getcwd()), points)

	@classmethod
	def home(Class):
		"""
		# Return a new Route to the home directory defined by the environment.

		# The returned Route's &context is the HOME path.
		"""

		return Class(Class.from_absolute(os.environ['HOME']), ())

	@classmethod
	@contextlib.contextmanager
	def fs_tmpdir(Class, TemporaryDirectory=tempfile.mkdtemp):
		"""
		# Create a temporary directory at a new route using a context manager.

		# A &Path to the temporary directory is returned on entrance,
		# and that same path is destroyed on exit.

		# [ Engineering ]
		# The use of specific temporary files is avoided as they have inconsistent
		# behavior on some platforms.
		"""

		d = TemporaryDirectory()
		try:
			r = Class.from_absolute(d).delimit()
			yield r
		finally:
			assert str(r) == d
			try:
				r.fs_void()
			except NameError:
				os.rmdir(d)
	temporary = fs_tmpdir

	@classmethod
	def which(Class, exe, dirname=os.path.dirname):
		"""
		# Return a new Route to the executable found by which.

		# [ Engineering ]
		# Relocating to &.execution.
		"""

		rp = shutil.which(exe)
		if rp is None:
			return None

		dn = dirname(rp)

		return Class(Class.from_absolute(dn), (rp[len(dn)+1:],))

	def __repr__(self):
		parts = ["/".join(p) for p in self.partitions() if p]
		if not parts:
			return "(file@'/')"
		parts[0] = "/" + parts[0]
		return "(file@%r)" %("//".join(parts),)

	def __str__(self):
		return self.fullpath

	def __fspath__(self) -> str:
		return self.fullpath

	@property
	def fullpath(self) -> str:
		"""
		# Returns the full filesystem path designated by the route.
		"""

		l = ['']
		if self.context is not None:
			l.append(path_string_cache(self.context))
		l.extend(self.points)

		return '/'.join(l) or '/'

	@property
	def bytespath(self, encoding=sys.getfilesystemencoding()) -> bytes:
		"""
		# Returns the full filesystem path designated by the route as a &bytes object
		# returned by encoding the &fullpath in &sys.getfilesystemencoding with
		# `'surrogateescape'` as the error mode.
		"""

		return self.fullpath.encode(encoding, "surrogateescape")

	def join(self, *parts:str) -> str:
		"""
		# Construct a string path using &self as the prefix and appending the path
		# fragments from &parts.

		# Segment instances should be given with an asterisk applied to the argument.
		"""

		if self.context is not None:
			ctxstr = self.context.fullpath
		else:
			ctxstr = ''

		subpath = self.points + parts
		if not subpath:
			return ctxstr or '/'

		return '/'.join((ctxstr, '/'.join(subpath)))

	@property
	def filename(self):
		"""
		# Filesystem specific alias for &identifier.
		"""
		return self.identifier

	@property
	def extension(self):
		"""
		# Return the last dot-extension of the filename.
		# &None if the filename has no `.` characters at all.
		"""

		i = self.identifier
		p = i.rfind('.')
		if p == -1:
			return None

		return i[p+1:]

	def suffix_filename(self, appended_suffix):
		"""
		# Modify the name of the file adding the given suffix.

		# Returns a new &Path Route.
		"""

		return self * (self.identifier + appended_suffix)
	suffix = suffix_filename

	def prefix_filename(self, prefix_string):
		"""
		# Modify the name of the file adding the given prefix.

		# Returns a new &Path Route.
		"""

		return self * (prefix_string + self.identifier)
	prefix = prefix_filename

	def fs_status(self, stat=os.stat) -> Status:
		"""
		# Construct a &Status instance using a system status record.
		"""
		return Status((stat(self.fullpath), self.identifier))

	def fs_type(self, ifmt=stat.S_IFMT, stat=os.stat, type_map=Status._fs_type_map) -> str:
		"""
		# The type of file the route points to. Transforms the result of an &os.stat
		# call into a string describing the (python/attribute)`st_mode` field.

		# [ Returns ]
		# - `'directory'`
		# - `'data'`
		# - `'pipe'`
		# - `'socket'`
		# - `'device'`
		# - `'void'`

		# If no file is present at the route or a broken link is present, `'void'` will be returned.
		"""

		try:
			s = stat(self.fullpath)
		except (FileNotFoundError, NotADirectoryError):
			return 'void'

		return type_map.get(ifmt(s.st_mode), 'unknown')

	def fs_test(self, type:str=None) -> bool:
		"""
		# Perform a set of tests against a fresh status record.

		# Returns &True when all the tests pass.
		# &False if one fails or the file does not exist.
		"""
		t = self.fs_type()
		if type is None:
			if t == 'void':
				return False
			else:
				pass
				# Default to continue when no type is present.
		elif type != t:
			return False

		return True

	def fs_executable(self, get_stat=os.stat, mask=stat.S_IXUSR|stat.S_IXGRP|stat.S_IXOTH) -> bool:
		"""
		# Whether the file at the route is considered to be an executable.
		"""

		mode = get_stat(self.fullpath).st_mode
		return (mode & mask) != 0

	def fs_follow_links(self, readlink=os.readlink, islink=os.path.islink) -> typing.Iterator[routes.Selector]:
		"""
		# Iterate through the links in a chain until a non-symbolic link file is reached.

		# ! NOTE:
			# The final Path yielded may not actually exist.
		"""
		Class = self.__class__
		r = self

		while islink(str(r)):
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
			sub = self/x
			if isdir(join(path, x)):
				directories.append(sub)
			else:
				files.append(sub)

		return directories, files

	def fs_iterfiles(self, type=None, scandir=os.scandir):
		"""
		# Generate &Path instances identifying the files held by the directory, &self.
		# By default, all file types are included, but if the &type parameter is given,
		# only files of that type are returned.

		# If &self is not a directory or cannot be searched, an empty iterator is returned.
		"""
		try:
			dl = scandir(self.fullpath)
		except OSError:
			# Error indifferent.
			# User must make explicit checks to interrogate permission/existence.
			return

		with dl as scan:
			if type is None:
				# No type constraint.
				for de in scan:
					yield self/de.name
			elif type == 'directory':
				# Avoids the stat call in the last branch.
				for de in scan:
					if de.is_dir():
						yield self/de.name
			else:
				# stat call needed (fs_type) to filter here.
				for de in scan:
					r = self/de.name
					if type == r.fs_type():
						yield r

	def subdirectories(self):
		return list(self.fs_iterfiles('directory'))
	def files(self):
		return list(self.fs_iterfiles('data'))

	def tree(self, Queue=collections.deque):
		"""
		# Return a directory's full tree as a pair of lists of &Path
		# instances referring to the contained directories and files.
		"""
		dirs, files = self.fs_list()
		cseq = Queue(dirs)

		while cseq:
			dir = cseq.popleft()
			sd, sf = dir.fs_list()

			# extend output
			dirs.extend(sd)
			files.extend(sf)

			# process subdirectories
			cseq.extend(sd)

		return dirs, files

	def fs_list(self, type='data', scandir=os.scandir):
		"""
		# Retrieve the list of files contained by the directory referred to by &self.
		# Returns a pair, the sequence of directories and the sequence of data files.

		# Sockets, pipes, devices, and other non-data files are not retained in the list.
		"""

		try:
			dl = scandir(self.fullpath)
		except OSError:
			# Error indifferent.
			# User must make explicit checks to interrogate permission/existence.
			return ([], [])

		dirs = []
		files = []

		with dl as scan:
			for de in scan:
				sub = self/de.name
				if de.is_dir():
					dirs.append(sub)
				else:
					typ = sub.fs_type()
					if sub.fs_type() == type:
						files.append(sub)

		return (dirs, files)

	def fs_index(self, type='data', Queue=collections.deque):
		"""
		# Generate pairs of directories associated with their files.

		# Sockets, pipes, devices, broken links, and other non-data files are not retained in the lists.
		"""

		dirs, files = self.delimit().fs_list(type=type)
		if not dirs and not files:
			return

		yield self, files
		cseq = Queue(dirs)

		while cseq:
			subdir = cseq.popleft()
			sd, sf = subdir.fs_list(type=type)

			yield subdir, sf

			# continue with subdirectories
			cseq.extend(sd)

	def fs_snapshot(self,
			process=(lambda x, y: y[0] == 'exception'),
			depth=8, limit=2048,
			ifmt=stat.S_IFMT, Queue=collections.deque, scandir=os.scandir,
			lstat=os.lstat,
		):
		"""
		# Construct an element tree of files from the directory referenced by &self.
		# Exceptions raised by the &os.stat and &os.scandir are stored in the tree
		# as `'exception'` elements.

		# [ Parameters ]
		# /process/
			# Boolean callable allowing attribute transformation and determining
			# whether or not an element should be included in the
			# resulting element tree.
			# Defaults to a lambda excluding `'exception'` types.
		# /depth/
			# The maximum filesystem depth to descend from &self.
			# If &None, no depth constraint is enforced.
			# Defaults to `8`.
		# /limit/
			# The maximum number of elements to accumulate.
			# If &None, no limit constraint is enforced.
			# Defaults to `2048`.
		"""

		if depth == 0 or limit == 0:
			# Allows presumption >= 1 or None.
			return []

		ftype = Status._fs_type_map.get

		cdepth = 0
		ncount = 0
		nelements = 0

		elements = []
		cseq = Queue()
		getnext = cseq.popleft

		cseq.append((self.delimit(), elements, self.fullpath))

		count = len(cseq)
		while cseq:
			subdir, dirlist, fp = getnext()
			count -= 1

			add = dirlist.append
			try:
				scan = scandir(fp)
			except OSError:
				add(('exception', [], {'status': None, 'error': err}))
				continue

			with scan as scan:
				for de in scan:
					file = subdir/de.name

					try:
						st = de.stat()
						typ = ftype(ifmt(st.st_mode), 'unknown')
						attrs = {'status': st, 'identifier': de.name}
					except FileNotFoundError:
						try:
							st = lstat(subdir.join(de.name))
						except FileNotFoundError:
							# Probably concurrent delete in this case.
							continue

						typ = 'void'
						attrs = {'status': st, 'identifier': de.name}
					except Exception as err:
						typ = 'exception'
						attrs = {'status': st, 'identifier': de.name, 'error': err}

					record = (typ, [], attrs)

					if process(file, record):
						continue
					add(record)

					nelements += 1
					if limit is not None and nelements >= limit:
						return elements

					if de.is_dir():
						cseq.append((file, record[1], file.fullpath))
						ncount += 1 # avoid len() call on deque

			if count <= 0 and ncount:
				cdepth += 1
				if depth is not None and cdepth >= depth:
					return elements
				count = ncount
				ncount = 0

		return elements

	def fs_since(self, since:int,
			traversed=None,
		) -> typing.Iterable[typing.Tuple[int, routes.Selector]]:
		"""
		# Identify the set of files that have been modified
		# since the given point in time.

		# The resulting &typing.Iterable does not include directories.

		# [ Parameters ]

		# /since/
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

		dirs, files = self.fs_list()

		for x in files:
			mt = x.fs_status().last_modified
			if mt.follows(since):
				yield (mt, x)

		for x in dirs:
			yield from x.fs_since(since, traversed=traversed)

	def fs_real(self, exists=os.path.exists):
		"""
		# Return the part of the Path that actually exists on the filesystem.
		"""

		for x in ~self:
			if exists(x.fullpath):
				return x

		return root

	def exists(self, exists=os.path.exists) -> bool:
		"""
		# Query the filesystem and return whether or not the file exists.

		# A Route to a symbolic link *will* return &False if the target does not exist.
		"""

		return exists(self.fullpath)

	def fs_size(self, stat=os.stat) -> int:
		"""
		# Return the size of the file as depicted by &os.stat.

		# The &os.stat function is used to get the information.
		# &None is returned if an &OSError is raised by the call.
		"""

		return stat(self.fullpath, follow_symlinks=True).st_size

	def get_last_modified(self) -> int:
		"""
		# Return the modification time of the file.
		"""

		return self.fs_status().last_modified

	def set_last_modified(self, time, utime=os.utime):
		"""
		# Set the modification time of the file identified by the &Route.
		"""

		return utime(self.__str__(), (-1, time.select('unix')/1000))

	def get_text_content(self, encoding:str='utf-8') -> str:
		"""
		# Retrieve the entire contents of the file as a &str.
		"""
		with self.fs_open('rt', encoding=encoding) as f:
			return f.read()

	def set_text_content(self, string:str, encoding:str='utf-8') -> None:
		"""
		# Modify the regular file identified by &self to contain the given &string.
		"""
		with self.fs_open('w', encoding=encoding) as f:
			f.write(string)

	def meta(self):
		"""
		# Return file specific meta data.

		# ! WARNING:
			# Preliminary API.
		"""

		st = self.fs_status()
		return (st.created, st.last_modified, st.st_size)

	def fs_void(self, rmtree=shutil.rmtree, remove=os.remove):
		"""
		# Remove the file that is referenced by this path.

		# If the Route refers to a symbolic link, only the link file will be removed.
		# If the Route refers to a directory, the contents and the directory will be removed.
		"""
		fp = self.fullpath

		try:
			typ = self.fs_type(stat=os.lstat)
		except FileNotFoundError:
			# Work complete.
			return

		if typ == 'directory':
			return rmtree(fp)
		else:
			# typ is 'void' for broken links.
			return remove(fp)

	def fs_replace(self, replacement, copytree=shutil.copytree, copyfile=shutil.copy) -> None:
		"""
		# Drop the existing file or directory, &self, and replace it with the
		# file or directory at the given route, &replacement.

		# [ Parameters ]
		# /replacement/
			# The route to the file or directory that will be used to replace
			# the one at &self.
		"""

		src = replacement.fullpath
		dst = self.fullpath
		self.fs_void()

		if replacement.fs_type() == 'directory':
			copytree(src, dst, symlinks=True, copy_function=copyfile)
		else:
			copyfile(src, dst)

	def fs_link_relative(self, path, link=os.symlink) -> None:
		"""
		# Create or update a *symbolic* link at &self pointing to &path, the target file.
		# The linked target path will be relative to &self' route.

		# Returns &self, the newly created link.

		# [ Parameters ]
		# /path/
			# The route identifying the target path of the symbolic link.
		"""

		relcount, segment = self.correlate(path)
		target = '../' * (relcount - 1)
		target += '/'.join(segment)

		try:
			link(target, self.fullpath)
		except FileExistsError:
			self.fs_void()
			if self.fs_type() != 'void':
				raise

			link(target, self.fullpath)

		return self

	def fs_link_absolute(self, path, link=os.symlink) -> None:
		"""
		# Create or update a *symbolic* link at &self pointing to &path, the target file.
		# The linked target path will be absolute.

		# Returns &self, the newly created link.

		# [ Parameters ]
		# /path/
			# The route identifying the target path of the symbolic link.
		"""

		target = path.fullpath

		try:
			link(target, self.fullpath)
		except FileExistsError:
			self.fs_void()
			if self.fs_type() != 'void':
				raise

			link(target, self.fullpath)

		return self

	def fs_init(self, data:typing.Optional[bytes]=None, mkdir=os.mkdir, exists=os.path.exists):
		"""
		# Create and initialize a data file at the route using the given &data.

		# If &data is &None, no write operation will occur for pre-existing files.
		# If &data is not &None, the bytes will be written regardless.

		# Returns the route instance.
		# Leading directories will be created as needed.
		"""

		fp = self.fullpath
		if exists(fp):
			if data is not None:
				self.fs_store(data) #* Re-initialize data file.
			return self

		routes = []
		for p in ~self.container:
			if p.fs_type() != 'void':
				break
			routes.append(p)

		# Create leading directories.
		for x in reversed(routes):
			mkdir(x.fullpath)

		with self.fs_open('xb') as f: #* Save ACL errors, concurrent op created file
			f.write(data or b'')

		return self

	def fs_mkdir(self, mkdir=os.mkdir, exists=os.path.exists):
		"""
		# Create and initialize a directory file at the route.

		# Returns the route instance.
		# Leading directories will be created as needed.
		"""

		fp = self.fullpath
		if exists(fp):
			return self

		routes = []
		for p in ~self.container:
			if p.fs_type() != 'void':
				break
			routes.append(p)

		# Create leading directories.
		for x in reversed(routes):
			mkdir(x.fullpath)

		mkdir(fp)
		return self

	@contextlib.contextmanager
	def fs_open(self, *args, **kw):
		"""
		# Open the file pointed to by the route.

		# If the file doesn't exist, create it; if the directories
		# leading up to the file don't exist, create the directories too.
		"""

		f = open(self.fullpath, *args, **kw)
		try:
			f.__enter__()
			yield f
		except BaseException as err:
			if not f.__exit__(err.__class__, err, err.__traceback__):
				raise
		else:
			f.__exit__(None, None, None)

	def fs_load(self, mode='rb') -> bytes:
		"""
		# Open the file, and return the entire contents as a &bytes instance.
		# If the file does not exist, an *empty bytes instance* is returned.

		# Unlike &store, this will not initialize the file.
		"""
		try:
			with self.fs_open(mode) as f:
				return f.read()
		except FileNotFoundError:
			return b''

	def fs_store(self, data:bytes, mode='wb'):
		"""
		# Given a &bytes instance, &data, store the contents at the location referenced
		# by the &Route. If the file does not exist, *it will be created* along with
		# the leading directories.
		"""
		with self.fs_open(mode) as f:
			return f.write(data)

	@contextlib.contextmanager
	def fs_chdir(self, chdir=os.chdir, getcwd=os.getcwd):
		"""
		# Context manager setting the &Path as the current working directory during the
		# context. On exit, restore the current working directory and PWD environment.

			#!/pl/python
				root = files.Path.from_absolute('/')
				with root.fs_chdir() as oldpwd:
					assert str(root) == os.getcwd()

				# # Restored at exit.
				assert str(oldcwd) == os.getcwd()

		# The old current working directory is yielded as a &Path instance.
		"""

		# RLock may be appropriate here.

		cwd = getcwd()
		oldpwd = os.environ.get('PWD')

		try:
			if oldpwd is not None:
				os.environ['OLDPWD'] = oldpwd

			os.environ['PWD'] = self.fullpath
			chdir(self.fullpath)
			yield self.from_absolute(oldpwd or cwd)
		finally:
			chdir(cwd)
			if oldpwd is not None:
				os.environ['PWD'] = oldpwd
				os.environ['OLDPWD'] = self.fullpath

root = Path(None, ())

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

		for x in l.fs_follow_links():
			l = x

		return self.from_route(l)
