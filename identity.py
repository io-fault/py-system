"""
# Collection of identifiers that represent the host system's implementation.

# Primarily, this provides information about the Root Execution Context in addition
# to information regarding the host Python importing the module.

# The identifiers are intended to be wholly symbolic; versions should only be employed
# in certain contexts where the distinctions are understood by the administrator
# providing (system/environ)`FCI_SYSTEM` and (system/environ)`FCI_ARCHITECTURE`
# overrides.
"""

import sys
_python_architecture = sys.implementation.name + ''.join(map(str, sys.version_info[:2]))
_python_architecture = _python_architecture.replace('-', '')
del sys

# Fault (Execution) Context Information
fci_system_envid = 'FCI_SYSTEM'
fci_architecture_envid = 'FCI_ARCHITECTURE'

def _uname(flag, path="/usr/bin/uname", encoding='utf-8'):
	"""
	# Execute the (system/executable)`uname` returning its output for the given &flag.
	"""
	from . import execution as libexec

	inv = libexec.KInvocation(path, [path, flag])
	pid, exitcode, out = libexec.dereference(inv)

	return out.strip().decode(encoding)

def _cache(uname_system='-s', uname_machine='-m'):
	global _system, _machine, _re_pair

	# Environment overrides.
	import os
	sys = os.environ.get(fci_system_envid, None)
	arc = os.environ.get(fci_architecture_envid, None)
	from . import kernel # _uname depends on it as well

	if sys is None:
		sys = getattr(kernel, 'fv_system', None) or _uname(uname_system).lower()
	if arc is None:
		arc = getattr(kernel, 'fv_architecture', None) or _uname(uname_machine).lower()

	_system = sys
	_machine = arc
	_re_pair = (sys, arc)

	return _re_pair

def root_execution_context():
	"""
	# Return the (operating system, architecture) pair identifying the Root Execution Context.
	"""
	try:
		return _re_pair
	except NameError:
		return _cache()

def python_execution_context():
	"""
	# Return the Python execution Context identification.
	# Used to select marshalled code objects(bytecode).
	"""
	return root_execution_context()[0], _python_architecture
