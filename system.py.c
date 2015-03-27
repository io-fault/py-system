/*
 * Low-level Python (CPython) system interfaces.
 */
#include <pthread.h>
#include <frameobject.h>

static PyObj libfork = NULL;

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

/*
 * AddPendingCall callback
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

/*
 * Expose AddPendingCall C-API to the Python language.
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

static pthread_mutex_t forking_mutex = PTHREAD_MUTEX_INITIALIZER;
int forking_pipe[2] = {-1,-1};
struct inherit {
	pid_t process_id;
};

/*
 * Communicate child's parent to parent.
 *
 * This allows fork to track fork(2)'s that weren't explicitly performed by
 * a Context.spawn operation.
 */
static void
prepare(void)
{
	pthread_mutex_lock(&forking_mutex);
	pipe(forking_pipe);
}

static struct inherit fork_data = {-1};
static int
_after_fork_parent(void *pc_param)
{
	PyObj rob, ctx;

	rob = PyObject_CallMethod(libfork, "_after_fork_parent", "i", (int) pc_param);
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
	if (Py_AddPendingCall(_after_fork_parent, (void *) fork_data.process_id))
	{
		goto retry;
	}
	fork_data.process_id = -1;
}

/*
 * Pending Call
 */
static int
_after_fork_child(void *pc_param)
{
	PyObj rob;
	rob = PyObject_CallMethod(libfork, "_after_fork_child", "");
	Py_XDECREF(rob);
	return(rob == NULL ? -1 : 0);
}

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
	if (Py_AddPendingCall(_after_fork_child, NULL))
	{
		goto retry;
	}
}

static int
ltracefunc(PyObj ob, PyFrameObject *f, int event)
{
	/*
	 * TODO: debugger control tracefunc
	 */
}

/*
 * Set the trace object on a set of threads. Only supports callable-object level.
 * This is intended for debuggers.
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
	 * convert sequence to array of longs
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
	 * install the tracefunc on the matching threadstates.
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

static int exit_signal = -1;
static pid_t exit_for_pid = -1;

void
_exit_by_signal(void)
{
	/*
	 * Ignore this if it somehow forked after the exit_by_signal was called.
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

static PyObj
initialize(PyObj mod, PyObj ctx)
{
	if (libfork != NULL)
	{
		/*
		 * Already configured.
		 */
		Py_RETURN_NONE;
	}

	libfork = ctx;
	Py_INCREF(libfork);

	if (pthread_atfork(prepare, parent, child))
	{
		PyErr_SetFromErrno(PyExc_OSError);
		DROP_MODULE(mod);
		return(NULL);
	}

	Py_RETURN_NONE;
}

METHODS() = {
	{"interrupt", (PyCFunction) interrupt, METH_VARARGS, PyDoc_STR(
"Interrupt a Python *thread* with the given exception.")},

	{"interject", (PyCFunction) interject, METH_O, PyDoc_STR(
"Interject the callable in the *main thread* using Py_AddPendingCall. Usually, the called object should dispatch a task.")},

	{"trace", (PyCFunction) trace, METH_VARARGS, PyDoc_STR(
"Apply the trace function to the given thread identifiers. Normally used by Context injections that take over the process for debugging.")},

	{"exit_by_signal",
		(PyCFunction) exit_by_signal, METH_O,
		PyDoc_STR(
":returns: None\n"
"\n"
"Register an :manpage:`atexit(2)` handler that causes the process to exit with the given signal number.\n"
"This may only be called once per-process."
)},

	{"initialize", (PyCFunction) initialize, METH_O, PyDoc_STR("Configure the module to use for fork callbacks.")},

	{NULL}
};

INIT("Python system access")
{
	PyObj mod = NULL;

	CREATE_MODULE(&mod);
	if (mod == NULL)
		return(NULL);

	return(mod);
}
/*
 * vim: ts=3:sw=3:noet:
 */
