#include <errno.h>
#include <spawn.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>
#include <frameobject.h>

extern char **environ;

/*
	// For fork callbacks
*/
static PyObj process_module = NULL;
static int exit_signal = -1;
static pid_t exit_for_pid = -1;

#define IOPTION_SET_PGROUP 1

struct Invocation {
	PyObject_HEAD

	char *invocation_path;
	char **invocation_argv;
	char **invocation_environ;

	posix_spawnattr_t invocation_spawnattr;
	char invocation_spawnattr_init;
	char invocation_options;
};
typedef struct Invocation *Invocation;

#define SPAWN_ATTRIBUTES() \
	SA(POSIX_SPAWN_SETPGROUP, set_process_group, posix_spawnattr_setpgroup) \
	SA(POSIX_SPAWN_SETSIGMASK, set_signal_mask, posix_spawnattr_setsigmask) \
	SA(POSIX_SPAWN_SETSIGDEF, set_signal_defaults)

#define APPLE_SPAWN_EXTENSIONS() \
	SA(POSIX_SPAWN_SETEXEC, replace_process_image) \
	SA(POSIX_SPAWN_START_SUSPENDED, start_suspended)

/**
	// SA(POSIX_SPAWN_CLOEXEC_DEFAULT, close_exec_default)
	// CLOEXEC_DEFAULT is an apple extension that is unconditionally used; users
	// are encourage to explicitly map (dup2) file descriptors using Invocation's call
*/
#define POSIX_SPAWN_ATTRIBUTES() \
	SA(POSIX_SPAWN_SETSCHEDULER, set_schedular_priority) \
	SA(POSIX_SPAWN_SETSCHEDPARAM, set_schedular_parameter)

static PyObj
invocation_spawn(PyObj self, PyObj args, PyObj kw)
{
	int r;
	pid_t child = 0;
	pid_t pgrp = -1;
	short flags = 0;
	static char *kwlist[] = {"fdmap", "inherit", "process_group", NULL,};

	PyObj fdmap = NULL;
	PyObj inherits = NULL;

	posix_spawn_file_actions_t fa;

	Invocation inv = (Invocation) self;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|OOi", kwlist, &fdmap, &inherits, &pgrp))
		return(NULL);

	/*
		// Inherit pgroup setting from Invocation instance if not overridden.
	*/
	if (pgrp < 0 && inv->invocation_options & IOPTION_SET_PGROUP)
	{
		/*
			// Some invocations are essentially identified
			// as independent daemons this way
		*/
		pgrp = 0;
	}

	if (posix_spawn_file_actions_init(&fa) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	/*
		// Modify attributes per-invocation.
		// Attributes like process group need to be per-invocation.
	*/
	if (posix_spawnattr_getflags(&(inv->invocation_spawnattr), &flags))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	if (pgrp >= 0)
	{
		flags |= POSIX_SPAWN_SETPGROUP;
		posix_spawnattr_setpgroup(&(inv->invocation_spawnattr), pgrp);
	}
	else
	{
		flags &= ~POSIX_SPAWN_SETPGROUP;
		posix_spawnattr_setpgroup(&(inv->invocation_spawnattr), 0);
	}

	if (posix_spawnattr_setflags(&(inv->invocation_spawnattr), flags))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	if (fdmap != NULL)
	{
		int fd, newfd, r;

		PyLoop_ForEachTuple(fdmap, "ii", &fd, &newfd)
		{
			r = posix_spawn_file_actions_adddup2(&fa, fd, newfd);

			if (r != 0)
			{
				PyErr_SetFromErrno(PyExc_OSError);
				break;
			}
		}
		PyLoop_CatchError(fdmap)
		{
			posix_spawn_file_actions_destroy(&fa);
			return(NULL);
		}
		PyLoop_End(fdmap)
	}

	#if __DARWIN__
		/*
			// Might remove this due to portability issues.
		*/
		if (inherits != NULL)
		{
			PyObj fdo;
			int fd, r;

			PyLoop_ForEach(inherits, &fdo)
			{
				fd = PyLong_AsLong(fdo);
				if (fd == -1 && PyErr_Occurred())
					break;

				r = posix_spawn_file_actions_addinherit_np(&fa, fd);

				if (r != 0)
				{
					PyErr_SetFromErrno(PyExc_OSError);
					break;
				}
			}
			PyLoop_CatchError(inherits)
			{
				posix_spawn_file_actions_destroy(&fa);
				return(NULL);
			}
			PyLoop_End(fdmap)
		}
	#else
		if (inherits != NULL)
		{
			PyErr_SetString(PyExc_TypeError, "inherits only supported on Darwin");
			return(NULL);
		}
	#endif

	r = posix_spawn(&child, (const char *) inv->invocation_path, &fa,
		&(inv->invocation_spawnattr),
		inv->invocation_argv,
		inv->invocation_environ == NULL ? environ : inv->invocation_environ);

	if (posix_spawn_file_actions_destroy(&fa) != 0)
	{
		/*
			// A warning would be appropriate.
		*/
		errno = 0;
	}

	if (r != 0)
	{
		errno = r;
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	return(PyLong_FromLong((long) child));
}

static PyObj
invocation_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"path", "arguments", "environ", "set_process_group", NULL,};
	PyObj rob;
	Invocation inv;

	pid_t child = 0;
	short flags = 0;
	int set_pgroup = 0;

	char *path;
	Py_ssize_t pathlen = 0;

	PyObj env = NULL, cargs;

	if (!PyArg_ParseTupleAndKeywords(
		args, kw, "s#|OOp", kwlist,
		&path, &pathlen, &cargs, &env, &set_pgroup)
	)
		return(NULL);

	if (env != NULL && !PyDict_Check(env))
	{
		PyErr_SetString(PyExc_TypeError, "environ keyword requires a builtins.dict instance");
		return(NULL);
	}

	rob = subtype->tp_alloc(subtype, 0);
	inv = (Invocation) rob;
	if (inv == NULL)
		return(NULL);

	inv->invocation_environ = NULL;
	inv->invocation_argv = NULL;
	inv->invocation_options = 0;

	if (set_pgroup)
		inv->invocation_options |= IOPTION_SET_PGROUP;

	inv->invocation_path = malloc(pathlen+1);
	if (inv->invocation_path == NULL)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}

	memcpy(inv->invocation_path, path, pathlen+1);

	if (posix_spawnattr_init(&(inv->invocation_spawnattr)) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}
	inv->invocation_spawnattr_init = 1;

	#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
		flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
	#endif

	if (posix_spawnattr_setflags(&(inv->invocation_spawnattr), flags) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}

	/*
		// Environment
	*/
	if (env == NULL || env == Py_None)
	{
		inv->invocation_environ = NULL;
	}
	else
	{
		unsigned long k = 0;
		Py_ssize_t keysize, valuesize, dl;
		char *key, *value;
		char **envp;

		dl = PyDict_Size(env);
		envp = inv->invocation_environ = malloc(sizeof(char *) * (dl + 1));

		if (envp == NULL)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			Py_DECREF(rob);
			return(NULL);
		}
		else
		{
			PyLoop_ForEachDictItem(env, "s#s#", &key, &keysize, &value, &valuesize)
			{
				register int size = keysize+valuesize+2;
				envp[k] = malloc(size);
				snprintf(envp[k], size, "%s=%s", key, value);
				k += 1;
			}
			PyLoop_CatchError(env)
			{
				/*
				 * Python Exceptions will only occur at start of loop,
				 * so the 'k' index will be the last.
				 */
				envp[k] = NULL;

				Py_DECREF(rob);
				return(NULL);
			}
			PyLoop_End(env)

			envp[k] = NULL;
		}
	}

	/*
		// Command Arguments
	*/
	if (cargs != NULL)
	{
		unsigned long k = 0;
		char *value = NULL;
		char **argv;
		Py_ssize_t valuesize = 0, al = PySequence_Length(cargs);

		argv = inv->invocation_argv = malloc(sizeof(void *) * (al + 1));
		if (argv == NULL)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			Py_DECREF(rob);
			return(NULL);
		}
		else
		{
			PyObj obj = NULL;
			argv[al] = NULL;

			PyLoop_ForEach(cargs, &obj)
			{
				if (PyBytes_Check(obj))
				{
					valuesize = PyBytes_GET_SIZE(obj);
					value = PyBytes_AS_STRING(obj);
				}
				else if (PyUnicode_Check(obj))
				{
					value = PyUnicode_AsUTF8AndSize(obj, &valuesize);
					if (value == NULL)
						break;
				}
				else
				{
					PyErr_SetString(PyExc_ValueError, "execfile command arguments must be bytes or str");
					break;
				}

				argv[k] = malloc(valuesize+1);
				strncpy(argv[k], value, valuesize);
				argv[k][valuesize] = '\0';
				k += 1;
				argv[k] = NULL;
			}
			PyLoop_CatchError(cargs)
			{
				Py_DECREF(rob);
				return(NULL);
			}
			PyLoop_End(cargs)

		}
	}

	return(rob);
}

#define free_null_terminated_array(free_op, NTL) \
	if (NTL != NULL) \
	{ \
		char **ntlp = (char **) NTL; \
		unsigned int i = 0; \
		for (i = 0; ntlp[i] != NULL; ++i) \
			free_op(ntlp[i]); \
		free_op(ntlp); \
	}

static void
invocation_dealloc(PyObj self)
{
	Invocation inv = (Invocation) self;

	/*
		// cleanup code. errors here are ignored.
	*/
	if (inv->invocation_path != NULL)
		free(inv->invocation_path);

	free_null_terminated_array(free, inv->invocation_argv);
	inv->invocation_argv = NULL;

	if (inv->invocation_environ != NULL)
	{
		free_null_terminated_array(free, inv->invocation_environ);
		inv->invocation_environ = NULL;
	}

	if (inv->invocation_spawnattr_init)
	{
		if (posix_spawnattr_destroy(&(inv->invocation_spawnattr)) != 0)
		{
			/*
			 * A warning would be appropriate.
			PyErr_SetFromErrno(PyExc_OSError);
			 */
		}
	}
}

static PyMethodDef
invocation_methods[] = {
	{"spawn", invocation_spawn, METH_VARARGS|METH_KEYWORDS, "submit the request to spawn the invocation"},
	{NULL,},
};

PyDoc_STRVAR(invocation_doc,
	"System command invocation interface.\n\n"
	"Invocation works by creating a reference to a system command using the executable\n"
	"path, command arguments sequence, and optional environment variables.\n"
	"Once created, the invocation can be reused with different sets of file descriptors for\n"
	"managing standard input, output, and error.\n\n"

	"#!/pl/python\n"
	"\tinv = kernel.Invocation(command_path, (command_argument, ...), envkey1=value, ..., envkeyN=value)\n"
	"\tpid = inv((pipe_read_side, 0), (pipe_write_side_stdout, 1), (pipe_write_side_stderr, 2))\n"

	"This designated object for invocation improves performance of repeat invocations by avoiding repeat conversion.\n"
);

PyTypeObject
InvocationType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	FACTOR_PATH("Invocation"),  /* tp_name */
	sizeof(struct Invocation),  /* tp_basicsize */
	0,                          /* tp_itemsize */
	invocation_dealloc,         /* tp_dealloc */
	NULL,                       /* tp_print */
	NULL,                       /* tp_getattr */
	NULL,                       /* tp_setattr */
	NULL,                       /* tp_compare */
	NULL,                       /* tp_repr */
	NULL,                       /* tp_as_number */
	NULL,                       /* tp_as_sequence */
	NULL,                       /* tp_as_mapping */
	NULL,                       /* tp_hash */
	invocation_spawn,           /* tp_call */
	NULL,                       /* tp_str */
	NULL,                       /* tp_getattro */
	NULL,                       /* tp_setattro */
	NULL,                       /* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_DEFAULT,         /* tp_flags */
	invocation_doc,             /* tp_doc */
	NULL,                       /* tp_traverse */
	NULL,                       /* tp_clear */
	NULL,                       /* tp_richcompare */
	0,                          /* tp_weaklistoffset */
	NULL,                       /* tp_iter */
	NULL,                       /* tp_iternext */
	invocation_methods,         /* tp_methods */
	NULL,                       /* tp_members */
	NULL,                       /* tp_getset */
	NULL,                       /* tp_base */
	NULL,                       /* tp_dict */
	NULL,                       /* tp_descr_get */
	NULL,                       /* tp_descr_set */
	0,                          /* tp_dictoffset */
	NULL,                       /* tp_init */
	NULL,                       /* tp_alloc */
	invocation_new,             /* tp_new */
};

static PyObj
set_process_title(PyObj mod, PyObj title)
{
	PyObj bytes;

	#if defined(__MACH__) || defined(__linux__)
		;
	#else
		/*
			// no support on darwin
		*/
		bytes = PyUnicode_AsUTF8String(title);

		if (bytes == NULL)
			return(NULL);

		setproctitle("%s", PyBytes_AS_STRING(bytes));
		Py_DECREF(bytes);
	#endif

	Py_RETURN_NONE;
}

static pthread_mutex_t forking_mutex = PTHREAD_MUTEX_INITIALIZER;
int forking_pipe[2] = {-1,-1};
struct inherit {
	pid_t process_id;
};

/**
	// Communicate child's parent to parent.

	// This allows fork to track (system:manual)`fork`'s that weren't explicitly performed by
	// an &.library interface.
*/
static void
prepare(void)
{
	pthread_mutex_lock(&forking_mutex);
	pipe(forking_pipe);
}

static struct inherit fork_data = {-1};

/**
	// Execute the &.library._after_fork_parent object from a pending call.
*/
static int
_after_fork_parent(void *pc_param)
{
	PyObj rob, ctx;

	rob = PyObject_CallMethod(process_module, "_after_fork_parent", "i", (int) pc_param);
	Py_XDECREF(rob);

	return(rob == NULL ? -1 : 0);
}

static void
parent(void)
{
	if (read(forking_pipe[0], &fork_data, sizeof(fork_data)) < sizeof(fork_data))
	{
		fork_data.process_id = -1;
		errno = 0;
	}

	close(forking_pipe[0]);
	close(forking_pipe[1]);
	forking_pipe[0] = -1;
	forking_pipe[1] = -1;
	pthread_mutex_unlock(&forking_mutex);

	retry:
	{
		uintptr_t param = fork_data.process_id;

		if (Py_AddPendingCall(_after_fork_parent, (void *) param))
		{
			goto retry;
		}
	}

	fork_data.process_id = -1;
}

/**
	// Execute the &.library._after_fork_child object from a pending call.
*/
static int
_after_fork_child(void *pc_param)
{
	PyObj rob;
	rob = PyObject_CallMethod(process_module, "_after_fork_child", "");
	Py_XDECREF(rob);
	return(rob == NULL ? -1 : 0);
}

/**
	// Synchronize with the parent process.
*/
static void
child(void)
{
	PyObj rob = Py_True;
	struct inherit buf = {-1};

	buf.process_id = getpid();

	write(forking_pipe[1], &buf, sizeof(buf));
	errno = 0;

	close(forking_pipe[0]);
	close(forking_pipe[1]);
	forking_pipe[0] = -1;
	forking_pipe[1] = -1;
	pthread_mutex_unlock(&forking_mutex);

	retry:
	{
		if (Py_AddPendingCall(_after_fork_child, NULL))
		{
			goto retry;
		}
	}
}

static int
ltracefunc(PyObj ob, PyFrameObject *f, int event, PyObj arg)
{
	/*
		// TODO: debugger control tracefunc
	*/
	return(0);
}

/**
	// Set the trace object on a set of threads.
	// Only supports callable-object level.
	// This is intended for debuggers.
*/
static PyObj
trace(PyObj self, PyObj args)
{
	PyObj trace_func, thread_ids;
	Py_ssize_t i;
	long *tids;
	PyThreadState *start = PyThreadState_Get();
	PyThreadState *ts;
	Py_ssize_t nthreads;
	Py_tracefunc f = ltracefunc;

	if (!PyArg_ParseTuple(args, "OO", &trace_func, &thread_ids))
		return(NULL);

	nthreads = PySequence_Length(thread_ids);
	tids = PyMem_Malloc(nthreads * sizeof(long));
	if (tids == NULL)
		return(NULL);

	/*
		// Convert sequence to array of longs.
	*/
	for (i = 0; i < nthreads; ++i)
	{
		PyObj n = PySequence_GetItem(thread_ids, i);

		if (n != NULL)
		{
			tids[i] = PyLong_AsLong(n);
			Py_DECREF(n);
		}

		if (PyErr_Occurred())
		{
			/*
			 * Couldn't get item or failued to convert.
			 * Exit.
			 */
			PyMem_Free(tids);
			return(NULL);
		}
	}

	/*
		// Install the tracefunc on the matching threadstates.
	*/
	ts = start;
	do
	{
		ts = ts->next;

		/* XXX: O(NM) bloomfilter? ;) */
		for (i = 0; i < nthreads; ++i)
		{
			if (tids[i] == ts->thread_id)
			{
				ts->c_tracefunc = ltracefunc;
				ts->c_traceobj = trace_func;
				ts->c_profilefunc = NULL;
				Py_XDECREF(ts->c_profileobj);
				ts->c_profileobj = NULL;
				ts->use_tracing = 1;
			}
		}
	}
	while(ts != start);

	PyMem_Free(tids);

	Py_RETURN_NONE;
}


/**
	// Executed in atexit in order to preserve the signal's exit code.
*/
void
_exit_by_signal(void)
{
	/*
		// Ignore this if it somehow forked after the exit_by_signal was called.
	*/
	if (exit_for_pid == getpid())
	{
		signal(exit_signal, SIG_DFL);
		kill(getpid(), exit_signal);

		/* signal didn't end the process, abort */
		fprintf(stderr, "[kernel._exit_by_signal: signal, %d, did not terminate process]\n", exit_signal);
		abort();
	}
}

/**
	// Register low-level atexit handler for exiting via a signal.
*/
static PyObj
exit_by_signal(PyObj mod, PyObj ob)
{
	long signo;
	pid_t p;

	signo = PyLong_AsLong(ob);
	if (PyErr_Occurred())
		return(NULL);

	p = getpid();

	if (exit_signal == -1 || exit_for_pid != p)
	{
		exit_for_pid = p;
		exit_signal = signo;
		atexit(_exit_by_signal);
	}
	else
	{
		PyErr_SetString(PyExc_RuntimeError, "exit_by_signal already called in this process");
		return(NULL);
	}

	Py_RETURN_NONE;
}

/**
	// Ensure that the kport is preserved across process images.
	// Used by system to hold on to listening sockets.

	// Generally, most file descriptors created by &.system will have
	// the FD_CLOEXEC flag set as in only a few cases, preservation is desired.
*/
static PyObj
kport_clear_cloexec(PyObj mod, PyObj seq)
{
	long fd;

	PyLoop_ForEachLong(seq, &fd)
	{
		int flag = fcntl((int) fd, F_GETFD, 0);

		if (flag == -1)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			break;
		}

		flag = fcntl((int) fd, F_SETFD, (flag & (~FD_CLOEXEC)));
		if (flag == -1)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			break;
		}
	}
	PyLoop_CatchError(seq)
	{
		return(NULL);
	}
	PyLoop_End(seq)

	Py_RETURN_NONE;
}

/*
	// Retrieve a reference to the process_module module and register
	// the atfork handlers.

	// Only runs once and there is currently no way to update the
	// process_module entry meaning that system.process should not be reloaded.
*/
static PyObj
initialize(PyObj mod, PyObj ctx)
{
	if (process_module != NULL)
	{
		/*
			// Already configured.
		*/
		Py_RETURN_NONE;
	}

	process_module = ctx;
	Py_INCREF(process_module);

	if (pthread_atfork(prepare, parent, child))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	Py_RETURN_NONE;
}

#define PYTHON_TYPES() \
	ID(Invocation)

#define MODULE_FUNCTIONS() \
	PYMETHOD( \
		preserve, kport_clear_cloexec, METH_O, \
			"Preserve the given file descriptors across process image substitutions(exec).") \
	PYMETHOD( \
		set_process_title, set_process_title, METH_O, \
			"Set the process title on platforms supporting " \
			"(system/manual)`setproctitle`. " \
			"Does nothing if unsupported or unsafe.") \
	PYMETHOD( \
		exit_by_signal, exit_by_signal, METH_O, \
			"Register an (system/manual)`atexit` handler that causes the " \
			"process to exit with the given signal number." \
			"\n\nThis may only be called once per-process.") \
	PYMETHOD( \
		initialize, initialize, METH_O, \
			"Initialize the after fork callbacks. " \
			"Called once by &.process. Do not use.")

#include <fault/python/module.h>
INIT(module, PyDoc_STR("Interfaces for the operating system.\n"))
{
	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(module, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		PYTHON_TYPES()
	#undef ID

	if (PyModule_AddStringConstant(module, "fci_architecture", F_TARGET_ARCHITECTURE_STR))
		goto error;
	if (PyModule_AddStringConstant(module, "fci_system", F_SYSTEM_STR))
		goto error;

	return(0);

	error:
	{
		return(-1);
	}
}
