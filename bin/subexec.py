"""
# Execute a module as a substitution.
"""
import importlib
from .. import process

def add_module(root, param):
	root['modules'].append(param)

def add_path(root, param):
	root['paths'].append(param)

def define_parameter(root, param):
	k, v = param.split('=')
	root['parameters'][k] = v

def remove_parameter(root, param):
	root['parameters'][param] = None

handlers = {
	'-X': None,
	'-l': add_module,
	'-L': add_path,
	'-D': define_parameter,
	'-U': remove_parameter,
}

def parse(arguments):
	"""
	# Extract command line parameter options.
	"""
	config = {
		'modules': [],
		'paths': [],
		'parameters': {},
	}

	i = 0
	for i, x in zip(range(len(arguments)), arguments):
		flag = x[:2]
		if flag not in handlers:
			break
		op = handlers[flag]
		op(config, x[2:])
	else:
		i += 1 # Trigger index error rather than import last option.

	return i, config

def apply(config):
	import sys
	try:
		sys._xoptions.update(config.get('parameters', {}).items())
	except:
		pass

	sys.path.extend(config.get('paths', ()))
	for x in map(importlib.import_module, config.get('modules', ())):
		if hasattr(x, 'activate'):
			x.activate()

def script(args):
	import types
	import sys
	ctxmod = types.ModuleType('__main__')
	ctxmod.__builtins__ = __builtins__
	path = args[0]
	sys.argv = args

	with open(path, 'r') as f:
		src = f.read()
		co = compile(src, path, 'exec')

	return exec(co, ctxmod.__dict__, ctxmod.__dict__)

def string(args):
	import types
	ctxmod = types.ModuleType('__main__')
	ctxmod.__builtins__ = __builtins__

	for i, expr in zip(range(len(args)), args):
		co = compile(expr, '<string:%d>'%(i,), 'single')
		eval(co, ctxmod.__dict__, ctxmod.__dict__)

	return ctxmod

def module(args):
	# Should be consistent with -m
	import sys
	import runpy
	sys.argv = args
	runpy.run_module(args[0], run_name='__main__', alter_sys=True)

def console(args):
	import sys
	import signal
	import code

	# Arguments played inline
	ctxmod = string(args)

	try:
		import site
		import readline
		import rlcompleter
		readline.set_completer(rlcompleter.Completer(ctxmod.__dict__).complete)
		site.enablerlcompleter()
		sys.__interactivehook__()
	except:
		sys.__excepthook__(*sys.exc_info())

	# Use KeyboardInterrupt for a less surprising experience.
	signal.signal(signal.SIGINT, signal.default_int_handler)

	banner = '[%s via %s]' %(sys.executable, sys.argv[0])
	ic = code.InteractiveConsole(ctxmod.__dict__)
	ic.interact(banner, exitmsg="EOF")

def main(inv:process.Invocation) -> process.Exit:
	count, config = parse(inv.args)
	apply(config)

	module_path = inv.args[count] # No module specified?
	del inv.args[0:count+1]
	inv.parameters['system'].setdefault('environment', {})

	if module_path.startswith('.'):
		if module_path == '.script':
			script(inv.args)
		elif module_path == '.string':
			string(inv.args)
		elif module_path == '.module':
			module(inv.args)
		elif module_path == '.console':
			console(inv.args)
		else:
			raise ModuleNotFoundError(module_path) # .* modules for builtin handlers.

		# Arguably a questionable default, but likely consistent with bin/python.
		raise SystemExit(0)
	else:
		sub = importlib.import_module(module_path)
		process.Fork.substitute(sub.main, inv)

		raise process.Panic("substitution failed to raised control exception")

if __name__ == '__main__':
	process.control(main, process.Invocation.system())

