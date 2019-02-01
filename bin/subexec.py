"""
# Execute a module as a substitution.
"""
import importlib
from .. import process

def main(inv:process.Invocation) -> process.Exit:
	module_path = inv.args[0]
	del inv.args[0:1]
	sub = importlib.import_module(module_path)
	process.Fork.substitute(sub.main, inv)
	raise process.Panic("substitution failed to raised control exception")

if __name__ == '__main__':
	process.control(main, process.Invocation.system())

