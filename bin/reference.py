"""
# Generate a reference to an execution.

# Creates a &.schemas.execute instance for subsequent system invocation.
# Initial parameters containing an `=` sign will be perceived as environment
# settings; a `?` placed directly before the `=` will cause it to be perceived
# as a default instead of a setting. The first parameter not containing an `=`
# will be identified as the executable, even if it's an emtpy string, and the
# ones that follow will be used as literal parameters to the execution of
# the program.

# The resulting XML will be written standard output.
"""
import sys
from ..library import Reference
from .. import libxml

def main(args):
	senv = {}
	denv = {}
	x = None

	i = iter(args)
	for x in i:
		if '=' in x:
			k, v = x.split('=')
			if k.endswith('?'):
				denv[k[:-1]] = v
			else:
				senv[k] = v
		else:
			break
	exe = x
	params = [
		Reference.environment(*(y.strip('$').split('?')[:2])) if y[0]+y[-1] == '$$' else y
		for y in i
	]

	struct = {
		'type': None,
		'environment': senv,
		'executable': exe,
		'parameters': params
	}
	if denv:
		struct['defaults'] = denv

	sys.stdout.buffer.writelines(libxml.Execute.serialize(struct))

if __name__ == '__main__':
	main(sys.argv[1:])
