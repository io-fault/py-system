"""
# Execute the process in the environment described by the &.schemas.execute instance.
"""
import sys
import os
import typing

from ..xml import Execute
from ..library import Reference
from ...xml.libfactor import readfile

def extract(refpath:str, substitutions:typing.Sequence[typing.Tuple[str,str]]):
	spawn = readfile(refpath)
	spawn = spawn.getroot()
	struct = Spawn.structure(spawn)

	env = list(struct['environment'])
	keys = set(os.environ)
	for k, v in struct.get('defaults', {}).items():
		if k not in keys:
			env.append((k, v))

	exe = struct['executable']
	params = [struct.get('program_name') or exe] + struct['parameters']

	return (env, exe, *Reference.strings(params))

if __name__ == '__main__':
	env, exe, params = extract(sys.argv[1], sys.argv[2:]) # Does not return (execl)
	os.environ.update(env)
	os.execl(exe, *params)
	assert False
