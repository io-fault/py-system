import sys
from .. import xml as module
from ...xml import libfactor

exe = b"""<?xml version="1.0" encoding="ascii"?>
<frame xmlns="http://fault.io/xml/system/execute"
	type="command" abstract="Admin Information.">
 <environment>
  <setting name="ENV" value="VAL"/>
 </environment>
 <executable path="/bin/cat"/>
 <parameters>
  <field literal="-f"/>
  <field literal="some_file"/>
 </parameters>
</frame>
"""
exe_doc = module.Execute.load(exe)

def test_Execute(test):
	test/module.Execute.schema.exists() == True

def test_Execute_isinstance(test):
	test/module.Execute.isinstance(exe_doc) == True

def test_Execute_structure(test):
	struct = module.Execute.structure(exe_doc)
	test/struct['executable'] == "/bin/cat"
	test/struct['environment'] == {"ENV": "VAL"}
	test/struct['parameters'] == ["-f", "some_file"]
	test/struct['abstract'] == "Admin Information."
	test/struct['type'] == "command"
	test/struct['alteration'] == None

def test_Execute_consistency(test):
	ix = module.Execute.structure(exe_doc)
	i = module.Execute.serialize(ix)
	xml = b''.join(i)
	x = module.Execute.structure(libfactor.readstring(xml))
	test/x == ix

if __name__ == '__main__':
	import sys
	from ...development import libtest
	libtest.execute(sys.modules[__name__])
