"""
# Collection of identifiers that represent the host system's implementation.

# Primarily, this provides information about the Root Execution Context in addition
# to information regarding the host Python importing the module.

# The identifiers are intended to be wholly symbolic; versions should only be employed
# in certain contexts where the distinctions are understood by the administrator
# providing (system/environ)`FCI_SYSTEM` and (system/environ)`FCI_ARCHITECTURE`
# overrides.
"""

import typing

# The inclusion of ABI flags may be inappropriate as the bytecode should not vary;
# however, they are included to allow a developer to impose a variation for some
# expiremental purposes.
import sys
_python_architecture = sys.implementation.name + ''.join(map(str, sys.version_info[:2])) + sys.abiflags
_python_architecture = _python_architecture.replace('-', '')
del sys

# Fault (Execution) Context Information
fci_system_envid = 'FCI_SYSTEM'
fci_architecture_envid = 'FCI_ARCHITECTURE'

def _uname(flag, path="/usr/bin/uname", encoding='utf-8'):
	"""
	# Execute the (system/executable)`uname` returning its output for the given &flag.
	"""
	from . import library as exe

	inv = exe.KInvocation(path, [path, flag])
	pid, exitcode, out = exe.dereference(inv)

	return out.strip().decode(encoding)

def _cache(uname_system='-s', uname_machine='-m'):
	global _system, _machine, _re_pair

	# Environment overrides.
	import os
	sys = os.environ.get(fci_system_envid, None)
	arc = os.environ.get(fci_architecture_envid, None)

	if sys is None:
		sys = _uname(uname_system).lower()
	if arc is None:
		arc = _uname(uname_machine).lower()

	_system = sys
	_machine = arc
	_re_pair = (sys, arc)

	return _re_pair

def root_execution_context() -> typing.Tuple[str,str]:
	"""
	# Return the (operating system, architecture) pair identifying the Root Execution Context.
	"""
	try:
		return _re_pair
	except NameError:
		return _cache()

def python_execution_context() -> typing.Tuple[str,str]:
	"""
	# Return the Python execution Context identification.
	# Used to select marshalled code objects(bytecode).
	"""
	s, m = root_execution_context()
	return s, _python_architecture

del typing
