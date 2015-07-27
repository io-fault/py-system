#include <spawn.h>

struct Invocation {
	PyObject_HEAD

	char *invocation_path;
	char **invocation_argv;
	char **invocation_environ;
	posix_spawnattr_t invocation_spawnattr;
	char invocation_spawnattr_init;
};
typedef struct Invocation *Invocation;

#define SPAWN_ATTRIBUTES() \
	SA(POSIX_SPAWN_SETPGROUP, set_process_group) \
	SA(POSIX_SPAWN_SETSIGMASK, set_signal_mask) \
	SA(POSIX_SPAWN_SETSIGDEF, set_signal_defaults)

#define APPLE_SPAWN_EXTENSIONS() \
	SA(POSIX_SPAWN_SETEXEC, replace_process_image) \
	SA(POSIX_SPAWN_START_SUSPENDED, start_suspended)

/* SA(POSIX_SPAWN_CLOEXEC_DEFAULT, close_exec_default) */
/* CLOEXEC_DEFAULT is an apple extension that is unconditionally used; users */
/* are encourage to explicitly map (dup2) file descriptors using Invocation's call */

#define POSIX_SPAWN_ATTRIBUTES() \
	SA(POSIX_SPAWN_SETSCHEDULER, set_schedular_priority) \
	SA(POSIX_SPAWN_SETSCHEDPARAM, set_schedular_parameter)

static PyObj
invocation_call(PyObj self, PyObj args, PyObj kw)
{
	int r;
	pid_t child = 0;
	static char *kwlist[] = {"fdmap", "inherit", NULL,};

	PyObj fdmap = NULL;
	PyObj inherits = NULL;

	posix_spawn_file_actions_t fa;

	Invocation inv = (Invocation) self;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "|OO", kwlist, &fdmap, &inherits))
		return(NULL);

	if (posix_spawn_file_actions_init(&fa) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	/*
	 * Handle the fdmap parameter.
	 */
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

	/*
	 * run the spawn
	 */
	r = posix_spawn(&child, (const char *) inv->invocation_path, &fa,
			&(inv->invocation_spawnattr),
			inv->invocation_argv,
			inv->invocation_environ);

	if (posix_spawn_file_actions_destroy(&fa) != 0)
	{
		/*
		 * A warning would be appropriate.
		PyErr_SetFromErrno(PyExc_OSError);
		 */
	}

	if (r != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	return(PyLong_FromLong((long) child));
}

static PyObj
invocation_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"path", "arguments", "environ", NULL,};
	PyObj rob;
	Invocation inv;

	pid_t child = 0;
	short flags = 0;

	char *path;
	Py_ssize_t pathlen = 0;

	PyObj env = NULL, cargs;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "s#|OO", kwlist, &path, &pathlen, &cargs, &env))
		return(NULL);

	rob = subtype->tp_alloc(subtype, 0);
	inv = (Invocation) rob;
	if (inv == NULL)
		return(NULL);

	inv->invocation_environ = NULL;
	inv->invocation_argv = NULL;

	inv->invocation_path = malloc(pathlen+1);
	if (inv->invocation_path == NULL)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(inv);
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
	 * Environment
	 */
	if (env != NULL)
	{
		unsigned long k = 0;
		Py_ssize_t keysize, valuesize, dl = PyDict_Size(kw);
		char *key, *value;
		char **envp;

		envp = inv->invocation_environ = malloc(sizeof(void *) * ((2*dl) + 1));
		if (envp == NULL)
		{
			PyErr_SetFromErrno(PyExc_OSError);
			Py_DECREF(inv);
			return(NULL);
		}
		else
		{
			PyLoop_ForEachDictItem(env, "s#s#", &key, &keysize, &value, &valuesize)
			{
				envp[k] = malloc(keysize);
				strncpy(envp[k], key, keysize);

				k += 1;
				envp[k] = malloc(valuesize);
				strncpy(envp[k], value, valuesize);

				k += 1;
			}
			PyLoop_CatchError(env)
			{
				Py_DECREF(rob);
				return(NULL);
			}
			PyLoop_End(env)

			envp[k] = NULL;
		}
	}

	/*
	 * Command Arguments
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
			Py_DECREF(inv);
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
	 * cleanup code. errors here are ignored.
	 */
	if (inv->invocation_path != NULL)
		free(inv->invocation_path);

	free_null_terminated_array(free, inv->invocation_argv);
	inv->invocation_argv = NULL;

	free_null_terminated_array(free, inv->invocation_environ);
	inv->invocation_environ = NULL;

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
	{NULL,},
};

PyDoc_STRVAR(invocation_doc,
	"System command invocation interface.\n\n"
	"Invocation works by creating a reference to a system command using the executable\n"
	"path, command arguments sequence, and optional environment variables.\n"
	"Once created, the invocation can be reused with different sets of file descriptors for\n"
	"managing standard input, output, and error.\n\n"
	"\tinv = kernel.Invocation(command_path, (command_argument, ...), envkey1=value, ..., envkeyN=value)\n"
	"\tpid = inv((pipe_read_side, 0), (pipe_write_side_stdout, 1), (pipe_write_side_stderr, 2))\n"
	"This designated object for invocation improves performance of repeat invocations by avoiding repeat conversion.\n"
);

PyTypeObject
InvocationType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	QPATH("Invocation"),        /* tp_name */
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
	invocation_call,            /* tp_call */
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

	#ifdef __MACH__
		;
	#else
		/*
		 * no support on darwin
		 */
		bytes = PyUnicode_AsUTF8String(title);

		if (bytes == NULL)
			return(NULL);

		setproctitle("%s", PyBytes_AS_STRING(bytes));
		Py_DECREF(bytes);
	#endif

	Py_RETURN_NONE;
}

METHODS() = {
	{"set_process_title",
		(PyCFunction) set_process_title, METH_O,
		PyDoc_STR(
			":returns: None\n"
			"\n"
			"Set the process title on supporting platforms."
		)
	},

	{NULL,}
};

#define PYTHON_TYPES() \
	ID(Invocation)

INIT(PyDoc_STR("kernel interfaces for supporting fork.\n"))
{
	PyObj mod = NULL;

	#if TEST()
		Py_XDECREF(__EOVERRIDE__);
		__EOVERRIDE__ = PyDict_New();
		if (__EOVERRIDE__ == NULL)
			return(NULL); XCOVERAGE

		Py_XDECREF(__POVERRIDE__);
		__POVERRIDE__ = PyDict_New();
		if (__POVERRIDE__ == NULL)
			return(NULL); XCOVERAGE
	#endif

	CREATE_MODULE(&mod);
	if (mod == NULL)
		return(NULL);

	#if TEST()
		PyModule_AddObject(mod, "EOVERRIDE", __EOVERRIDE__);
		PyModule_AddObject(mod, "POVERRIDE", __POVERRIDE__);
	#endif

	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(mod, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		PYTHON_TYPES()
	#undef ID

	return(mod);

	error:
		DROP_MODULE(mod);
		return(NULL);
}
#if 0
"""
#endif
