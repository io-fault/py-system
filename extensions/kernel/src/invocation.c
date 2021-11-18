#include <errno.h>
#include <spawn.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "invocation.h"

#define IOPTION_SET_PGROUP 1

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

extern char **environ;
static PyObj
inv_spawn(PyObj self, PyObj args, PyObj kw)
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
	if (pgrp < 0 && inv->ki_options & IOPTION_SET_PGROUP)
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
	if (posix_spawnattr_getflags(&(inv->ki_spawnattr), &flags))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}

	if (pgrp >= 0)
	{
		flags |= POSIX_SPAWN_SETPGROUP;
		posix_spawnattr_setpgroup(&(inv->ki_spawnattr), pgrp);
	}
	else
	{
		flags &= ~POSIX_SPAWN_SETPGROUP;
		posix_spawnattr_setpgroup(&(inv->ki_spawnattr), 0);
	}

	if (posix_spawnattr_setflags(&(inv->ki_spawnattr), flags))
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

	r = posix_spawn(&child, (const char *) inv->ki_path, &fa,
		&(inv->ki_spawnattr),
		inv->ki_argv,
		inv->ki_environ == NULL ? environ : inv->ki_environ);

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
inv_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"path", "arguments", "environ", "set_process_group", NULL,};
	sigset_t sigreset;
	PyObj rob;
	Invocation inv;

	pid_t child = 0;
	short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
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

	inv->ki_environ = NULL;
	inv->ki_argv = NULL;
	inv->ki_options = 0;

	if (set_pgroup)
		inv->ki_options |= IOPTION_SET_PGROUP;

	inv->ki_path = malloc(pathlen+1);
	if (inv->ki_path == NULL)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}

	memcpy(inv->ki_path, path, pathlen+1);

	if (posix_spawnattr_init(&(inv->ki_spawnattr)) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}

	inv->ki_spawnattr_init = 1;

	#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
		flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
	#endif

	if (posix_spawnattr_setflags(&(inv->ki_spawnattr), flags) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}

	sigfillset(&sigreset);
	if (posix_spawnattr_setsigdefault(&(inv->ki_spawnattr), &sigreset) != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		Py_DECREF(rob);
		return(NULL);
	}

	sigemptyset(&sigreset);
	if (posix_spawnattr_setsigmask(&(inv->ki_spawnattr), &sigreset) != 0)
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
		inv->ki_environ = NULL;
	}
	else
	{
		unsigned long k = 0;
		Py_ssize_t keysize, valuesize, dl;
		char *key, *value;
		char **envp;

		dl = PyDict_Size(env);
		envp = inv->ki_environ = malloc(sizeof(char *) * (dl + 1));

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

		argv = inv->ki_argv = malloc(sizeof(void *) * (al + 1));
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
					PyErr_SetString(PyExc_ValueError,
						"execfile command arguments must be bytes or str");
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
inv_dealloc(PyObj self)
{
	Invocation inv = (Invocation) self;

	/*
		// cleanup code. errors here are ignored.
	*/
	if (inv->ki_path != NULL)
		free(inv->ki_path);

	free_null_terminated_array(free, inv->ki_argv);
	inv->ki_argv = NULL;

	if (inv->ki_environ != NULL)
	{
		free_null_terminated_array(free, inv->ki_environ);
		inv->ki_environ = NULL;
	}

	if (inv->ki_spawnattr_init)
	{
		if (posix_spawnattr_destroy(&(inv->ki_spawnattr)) != 0)
		{
			/*
			 * A warning would be appropriate.
			PyErr_SetFromErrno(PyExc_OSError);
			 */
		}
	}

	Py_TYPE(self)->tp_free(self);
}

static PyMethodDef
inv_methods[] = {
	#define PyMethod_Id(N) inv_##N
		PyMethod_Keywords(spawn),
	#undef PyMethod_Id
	{NULL,},
};

/**
	// &.kernel.Invocation
*/
PyTypeObject
InvocationType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	FACTOR_PATH("Invocation"),  /* tp_name */
	sizeof(struct Invocation),  /* tp_basicsize */
	0,                          /* tp_itemsize */
	inv_dealloc,                /* tp_dealloc */
	NULL,                       /* tp_print */
	NULL,                       /* tp_getattr */
	NULL,                       /* tp_setattr */
	NULL,                       /* tp_compare */
	NULL,                       /* tp_repr */
	NULL,                       /* tp_as_number */
	NULL,                       /* tp_as_sequence */
	NULL,                       /* tp_as_mapping */
	NULL,                       /* tp_hash */
	inv_spawn,                  /* tp_call */
	NULL,                       /* tp_str */
	NULL,                       /* tp_getattro */
	NULL,                       /* tp_setattro */
	NULL,                       /* tp_as_buffer */
	Py_TPFLAGS_BASETYPE|
	Py_TPFLAGS_DEFAULT,         /* tp_flags */
	NULL,                       /* tp_doc */
	NULL,                       /* tp_traverse */
	NULL,                       /* tp_clear */
	NULL,                       /* tp_richcompare */
	0,                          /* tp_weaklistoffset */
	NULL,                       /* tp_iter */
	NULL,                       /* tp_iternext */
	inv_methods,                /* tp_methods */
	NULL,                       /* tp_members */
	NULL,                       /* tp_getset */
	NULL,                       /* tp_base */
	NULL,                       /* tp_dict */
	NULL,                       /* tp_descr_get */
	NULL,                       /* tp_descr_set */
	0,                          /* tp_dictoffset */
	NULL,                       /* tp_init */
	NULL,                       /* tp_alloc */
	inv_new,                    /* tp_new */
};
