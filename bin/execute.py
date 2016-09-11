"""
Execute the process in the environment described by the &.schemas.spawn instance.
"""
import sys
import os
import typing

from ..libxml import Spawn
from ..library import Reference
from ...xml.libfactor import readfile

def main(refpath:str, substitutions:typing.Sequence[typing.Tuple[str,str]]):
	spawn = readfile(refpath)
	spawn = spawn.getroot()
	struct = Spawn.structure(spawn)

	os.environ.update(struct['environment'])
	keys = set(os.environ)
	for k, v in struct.get('defaults', {}).items():
		if k not in keys:
			os.environ[k] = v

	exe = struct['executable']
	params = [struct.get('program_name') or exe] + struct['parameters']
	os.execl(exe, *Reference.strings(params))

	assert False # lexec should replace image.

if __name__ == '__main__':
	main(sys.argv[1], sys.argv[2:]) # Does not return (execl)
