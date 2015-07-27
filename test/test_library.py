import os
from .. import library
from .test_kernel import perform_cat

def test_pinvocation(test):
	data = b'data sent through a cat pipeline\n'
	for count in range(0, 16):
		s = library.PInvocation.from_commands(
			*([('/bin/cat',)] * count)
		)
		pl = s()
		out, status = perform_cat(pl.process_identifiers, pl.input, pl.output, data, *pl.standard_errors.values())
		test/out == data
		test/len(status) == count

	os.wait()

if __name__ == '__main__':
	import sys; from ...development import libtest
	libtest.execute(sys.modules[__name__])
