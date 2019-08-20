import io
import os

from .. import files
from .. import kernel as module

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

	# Workaround for macos.
	# Process (cat) exits don't appear to be occurring properly
	# on macos. (Thu Jun 22 09:33:35 MST 2017)
	# Specifically, the cat implementation doesn't appear to be getting
	# closed file descriptors.
	# This may indicate an issue with the fault.system or its usage.
	for pid in pids:
		os.kill(pid, 9)

	for pid in pids:
		r = os.waitpid(pid, 0)
		status.append(r)
	output.close()

	return data, status

def test_Invocation(test):
	"""
	# Sanity and operations.
	"""
	notreal = module.Invocation("some string", ())
	test.isinstance(notreal, module.Invocation)

	del notreal
	test.garbage()

	environ = module.Invocation("/bin/cat", ("cat", "-",), environ=dict(SOME_ENVIRON_VAR="foo"))

	realish = module.Invocation("/bin/cat", ("cat", "filepath1", "filepath2", "n"*100,))
	del realish
	test.garbage()

def test_Invocation_file_not_found(test):
	"""
	# Validate that a reasonable OSError is raised when the executable doesn't exist.
	"""
	tr = test.exits.enter_context(files.Path.temporary())
	r = tr / 'no-such.exe'
	i = module.Invocation(str(r), ())

	with open(os.devnull) as f:
		invoke = lambda: i(((f.fileno(), 0), (f.fileno(), 1), (f.fileno(), 2)))
		test/FileNotFoundError ^ invoke

def test_Invocation_execute(test):
	# echo data through cat
	stdin = os.pipe()
	stdout = os.pipe()
	stderr = os.pipe()

	catinv = module.Invocation("/bin/cat", (), environ={})
	pid = catinv(((stdin[0],0), (stdout[1],1), (stderr[1],2)))

	os.close(stdin[0])
	os.close(stdout[1])
	os.close(stderr[1])

	# process launched?
	os.kill(pid, 0) # Check that process exists.

	data = b'data\n'
	out, status = perform_cat([pid], stdin[1], stdout[0], data, stderr[0])
	test/out == data

def test_Ports_new(test):
	kp = module.Ports(list(range(10)))
	test/list(kp) == list(range(10))

	# Internal list materialization
	kp = module.Ports((range(10)))
	test/list(kp) == list(range(10))

def test_Ports_sequence(test):
	kp = module.Ports.allocate(16)
	test/len(kp) == 16

	# Iterator
	test/list(kp) == [-1 for x in range(16)]

	test/kp[0] == -1
	test/kp[-1] == -1

	# set/get item.
	for x in range(16):
		kp[x] = 10
		test/kp[x] == 10

if __name__ == '__main__':
	import sys; from ...test import library as libtest
	libtest.execute(sys.modules[__name__])
