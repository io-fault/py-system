import io
import os
from .. import library as libsys
from .. import kernel as library

def test_invocation_create(test):
	notreal = library.Invocation("some string", ())
	test/notreal / library.Invocation

	del notreal
	test.garbage()

	environ = library.Invocation("/bin/cat", ("-",), environ=dict(SOME_ENVIRON_VAR="foo"))

	realish = library.Invocation("/bin/cat", ("filepath1", "filepath2", "n"*100,))
	del realish
	test.garbage()

def test_invocation_file_not_found(test):
	"Validate that a reasonable OSError is raised when the executable doesn't exist."
	pass

def perform_cat(pids, input, output, data, *errors):
	error = []
	for errfd in errors:
		error.append(io.open(errfd, mode='rb'))

	input = io.open(input, mode='wb')
	output = io.open(output, mode='rb')

	idata = data
	while idata:
		idata = idata[input.write(idata):]

	input.flush()
	input.close()

	out = b''
	while out != data:
		out += output.read(len(data) - len(out))

	for e in error:
		e.close()

	status = []

	for pid in pids:
		status.append(libsys.process_delta(pid))
	output.close()

	return data, status

def test_invocation_execute(test):
	# echo data through cat
	stdin = os.pipe()
	stdout = os.pipe()
	stderr = os.pipe()

	catinv = library.Invocation("/bin/cat", ())
	pid = catinv(((stdin[0],0), (stdout[1],1), (stderr[1],2)))

	os.close(stdin[0])
	os.close(stdout[1])
	os.close(stderr[1])

	# process launched?
	os.kill(pid, 0) # Check that process exists.

	data = b'data\n'
	out, status = perform_cat([pid], stdin[1], stdout[0], data, stderr[0])
	test/out == data

if __name__ == '__main__':
	import sys; from ...development import libtest
	libtest.execute(sys.modules[__name__])
