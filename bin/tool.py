"""
# Tool execution factor for explicitly linked command sets.
"""
import sys
import importlib
from .. import process

_select_methods = []

def extend(*select):
	"""
	# Add lookup functions for extending the set of executable tools.

	# When a &select callable is invoked with a tool name, a module path should be
	# returned identifying the executable or a &LookupError should be raised designating
	# that no match could be made.
	"""
	_select_methods.extend(select)

def query(name):
	for select in _select_methods:
		try:
			return select(name)
		except LookupError:
			continue

	raise LookupError("tool identifier not known to configured set: " + name)

def main(inv:process.Invocation) -> process.Exit:
	name = inv.argv[0]
	factor, symbol, *protocol = query(name)
	f = importlib.import_module(factor)

	inv.parameters['tool'] = name
	del inv.argv[:1]
	process.Fork.substitute(getattr(f, symbol), inv)
