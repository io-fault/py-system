/**
	// Runtime control support.
*/
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>
#include <frameobject.h>

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

		/* XXX: O(NM) */
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
	// AddPendingCall callback
*/
static int
_call(void *ob)
{
	PyObject *callable = ob;
	PyObject *ret = NULL;

	ret = PyObject_CallObject(ob, NULL);
	Py_XDECREF(ret);
	Py_DECREF(callable);

	return(ret == NULL ? -1 : 0);
}

/**
	// Expose AddPendingCall C-API to the Python language.
*/
static PyObj
interject(PyObj self, PyObj callable)
{
	PyObj rob = Py_True;

	Py_INCREF(callable);
	if (Py_AddPendingCall(_call, callable))
	{
		Py_DECREF(callable);
		rob = Py_False;
	}

	Py_INCREF(rob);
	return(rob);
}

static PyObj
interrupt(PyObj self, PyObj args)
{
	long tid;
	PyObj exc;

	if (!PyArg_ParseTuple(args, "lO", &tid, &exc))
		return(NULL);

	if (!PyThreadState_SetAsyncExc(tid, exc))
		return(NULL);

	Py_INCREF(Py_None);
	return(Py_None);
}

#define PYTHON_TYPES()
#define MODULE_FUNCTIONS() \
	PYMETHOD( \
		interrupt, interrupt, METH_VARARGS, \
			"Interrupt a Python thread with the given exception.") \
	PYMETHOD( \
		interject, interject, METH_O, \
			"Interject the callable in the *main thread* using Py_AddPendingCall. " \
			"Usually, the called object should dispatch a task.") \
	PYMETHOD( \
		trace, trace, METH_VARARGS, \
			"Apply the trace function to the given thread identifiers. " \
			"Normally used by Context injections that take over the process for debugging.")

#include <fault/python/module.h>
INIT(module, 0, PyDoc_STR("Runtime control interfaces.\n"))
{
	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(module, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		PYTHON_TYPES()
	#undef ID

	return(0);

	error:
	{
		return(-1);
	}
}
