"""
# Process state storage for residual file descriptors.

# &.ports manages the set of file descriptors that are intended to be kept across process images.
# This module is normally used exclusively by daemon processes that wish to update their
# process image without having to discard listening sockets.
"""

_state = {}

def restore(identifier:str, ports:typing.Sequence[int]):
	"""
	# Add &ports to the set using the &identifier presuming that the ports
	# are properly configured. Used by daemon processes after performing
	# (syscall)&exec to refresh the process image.
	"""
	_state[identifier] = list(ports)

def install(identifier, ports:typing.Sequence[int]):
	"""
	# Add the &ports to the set making sure that they are properly configured
	# to persist across process images.

	# If a sequence of ports is already installed with the &identifier, it
	# will be extended.
	"""
	from . import kernel

	p = list(ports)
	kernel.preserve(p)

	if identifier in _state:
		_state[identifier].extend(p)
	else:
		_state[identifier] = p

def allocate():
	"""
	# Return the full set of installed ports for use within a worker process
	# and remove the installed ports from process the global data.
	"""
	global _state
	fdset = _state
	_state = {}
	return fdset
