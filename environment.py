"""
# Python Interface implementation of software environments.

# Environment implementations may vary; this module provides an interface
# to common protocols for resolving project locations.
"""

def test(route):
	"""
	# Check whether the given route is the root of an environment.
	"""
	envdir = route / '.environment'
	if not envdir.is_directory():
		return False

	pi = envdir / 'project-index.txt'
	if not pi.is_regular_file():
		return False

	g = envdir / 'groups.txt'
	if not g.is_regular_file():
		return False

	return True

def discover(route):
	"""
	# Identify the environment that holds the given &route.
	"""

	current = route
	while test(current) is False:
		current = current.container
		if (current.context, current.point) == (None, ()):
			# This return implies root is not allowed to be an environment. 
			return None

	return current
