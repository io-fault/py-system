"""
# Execute a module as a substitution.
"""
import importlib
from .. import process

def add_control_factor(root, param):
	root['control-factors'].append(param)

def add_product_path(root, param):
	root['product-paths'].append(param)

def connect_product_environ(root, param):
	import os
	pd_path = os.environ['F_PRODUCT']
	root['product-paths'].insert(0, pd_path)
	assert param == '' # -d does not take inline parameters

def add_path(root, param):
	root['paths'].append(param)

def define_parameter(root, param):
	k, v = param.split('=')
	root['parameters'][k] = v

def remove_parameter(root, param):
	root['parameters'][param] = None

handlers = {
	'-X': None,
	'-l': add_control_factor,
	'-L': add_product_path,
	'-d': connect_product_environ,
	'-P': add_path,
	'-D': define_parameter,
	'-U': remove_parameter,
}

def parse(arguments):
	"""
	# Extract command line parameter options.
	"""
	config = {
		'control-factors': [],
		'product-paths': [],
		'paths': [],
		'parameters': {},
	}

	i = 0
	for i, x in zip(range(len(arguments)), arguments):
		flag = x[:2]
		if flag not in handlers:
			break
		op = handlers[flag]

		inline_p = x[2:]
		op(config, inline_p)
	else:
		i += 1 # Trigger index error rather than import last option.

	return i, config

def apply(config, target, symbol):
	import sys
	try:
		sys._xoptions.update(config.get('parameters', {}).items())
	except:
		pass

	from .. import factors
	from .. import files
	sys.path.extend(config.get('paths', ()))

	for product_path in config.get('product-paths', ()):
		factors.finder.connect(files.Path.from_absolute(product_path))

	execution = importlib.import_module(target)

	for control in map(importlib.import_module, config.get('control-factors', ())):
		control.activate(target, symbol)

	return execution

def script(inv):
	import types
	import sys
	ctxmod = types.ModuleType('__main__')
	ctxmod.__builtins__ = __builtins__
	path = inv.argv[0]
	sys.argv = inv.argv

	with open(path, 'r') as f:
		src = f.read()
		co = compile(src, path, 'exec')

	exec(co, ctxmod.__dict__, ctxmod.__dict__)
	return inv.exit(0)

def _alloc(init):
	import types
	ctxmod = types.ModuleType('__main__')
	ctxmod.__builtins__ = __builtins__

	for i, expr in zip(range(len(init)), init):
		co = compile(expr, '<string:%d>'%(i,), 'single')
		eval(co, ctxmod.__dict__, ctxmod.__dict__)

	return ctxmod

def string(inv):
	_alloc(inv.argv)
	return inv.exit(0)

def module(inv):
	# Should be consistent with -m
	import sys
	import runpy
	sys.argv = inv.argv

	runpy.run_module(inv.argv[0], run_name='__main__', alter_sys=True)
	return inv.exit(0)

def console(inv):
	import sys
	import signal
	import code

	# Arguments played inline
	ctxmod = _alloc(inv.argv)

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
	return inv.exit(0)

def main(inv:process.Invocation) -> process.Exit:
	count, config = parse(inv.argv)

	module_path = inv.argv[count] # No module specified?
	if module_path.startswith('.'):
		symbol = module_path[1:]
		module_path = __name__
	else:
		symbol = config.get('symbol', 'main')

	module = apply(config, module_path, symbol)

	del inv.argv[0:count+1]
	inv.parameters['system'].setdefault('environment', {})

	process.Fork.substitute(getattr(module, symbol), inv)
	raise process.Panic("substitution failed to raised control exception")

if __name__ == '__main__':
	process.control(main, process.Invocation.system())
