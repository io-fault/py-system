"""
# Execute an application module selected by argument zero.
"""
import importlib
from .. import process

def main(inv:process.Invocation) -> process.Exit:
	module_path = inv.parameters['system']['name']
	sub = importlib.import_module(module_path)
	process.Fork.substitute(sub.main, inv)
	raise process.Panic("substitution failed to raised control exception")

if __name__ == '__main__':
	process.control(main, process.Invocation.system())
