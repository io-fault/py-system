"""
# High-level system inquiries for user information and environment derived paths.

# All information is retrieved from the system when invoked.
"""
import os
import operator

from ..context import tools
from . import files

# Keep the parsed forms cached.
_path = tools.cachedcalls(16)(tools.compose(operator.methodcaller('delimit'), files.Path.from_absolute))
_dirs = tools.cachedcalls(2)(operator.methodcaller('split', os.pathsep))

def paths(environment:str='PATH') -> files.Path:
	"""
	# Select paths present in the (system/environ)`PATH` variable.
	"""
	return map(_path, _dirs(os.environ[environment]))

def executables(exename:str, environment:str='PATH') -> files.Path:
	"""
	# Select executable paths from the environment, (system/environ)`PATH`.

	# Iterate over the directories listed in `PATH` and yield paths that exist
	# when the given &exename is extended on the directory.

	# The produced &files.Path instances may refer to files of any type.
	# Paths referring to broken links will not be included.
	"""

	# Isolated to avoid lookup/splitting in cases where the override is not exhausted.
	for x in paths(environment):
		path = x / exename
		typ = path.fs_type()
		if typ != 'void':
			yield path

def home(environment:str='HOME') -> files.Path:
	"""
	# Retrieve the user's home directory from the environment, (system/environ)`HOME`.

	# If the environment variable is not set, the (id)`pw_dir` field will be retrieved
	# using &pwd.
	"""
	try:
		return _path(os.environ[environment])
	except (KeyError, TypeError):
		try:
			import pwd
			return _path(pwd.getpwnam(username()).pw_dir)
		except:
			return None

def username(environment:str='USER') -> str:
	"""
	# Retrieve the user's name.

	# On nix systems, this is the (system/environ)`USER`.
	"""
	return str(os.environ[environment])

def usertitle() -> str:
	"""
	# Retrieve the user's title; the long name associated with the user.

	# The (id)`pw_gecos` field is retrieved using &pwd without modification.
	# If the system has no such concept or it cannot be resolved, &None is returned.
	"""
	try:
		import pwd
		return pwd.getpwnam(username()).pw_gecos
	except:
		return None

def shell() -> files.Path:
	"""
	# Retrieve the path to the user's login shell.

	# If the system has no such concept or it cannot be resolved, &None is returned.
	"""
	try:
		import pwd
		return pwd.getpwnam(username()).pw_shell
	except:
		return None

def hostname() -> str:
	"""
	# Retrieve the hostname using the POSIX (system/manual)`gethostname(2)` call.
	"""
	from . import kernel
	return kernel.hostname().decode('idna')
