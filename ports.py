"""
# Process state storage for residual file descriptors.

# &.ports manages the set of file descriptors that are intended to be kept across process images.
# This module is normally used exclusively by daemon processes that wish to update their
# process image without having to discard listening sockets.
"""
import os
import typing
from . import kernel

_state = {}

def close(identifier:str):
	"""
	# Close the ports associated with the &identifier and remove them from storage.
	"""
	for x in _state.pop(identifier, ()):
		os.close(x)

def restore(identifier:str, ports:kernel.Ports) -> None:
	"""
	# Add &ports to the set using the &identifier presuming that the ports
	# are properly configured. Used by daemon processes after performing
	# (syscall)&exec to refresh the process image.
	"""
	_state[identifier] = kernel.Ports(ports)

def install(identifier, ports:kernel.Ports) -> None:
	"""
	# Add the &ports to the set making sure that they are properly configured
	# to persist across process images.

	# If a sequence of ports is already installed with the &identifier, it
	# will be extended.
	"""
	p = kernel.Ports(ports)
	kernel.preserve(p)

	if identifier in _state:
		_state[identifier] += p
	else:
		_state[identifier] = p

def allocate() -> typing.Mapping[str, kernel.Ports]:
	"""
	# Return the full set of installed ports for use within a worker process
	# and remove the installed ports from process the global data.
	"""
	global _state

	fdset = _state
	_state = {}
	for ports in fdset.values():
		kernel.released(ports)

	return fdset
