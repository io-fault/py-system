/**
	// &.kernel module.
*/
#include <errno.h>
#include <spawn.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/utsname.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>
#include <frameobject.h>

#include "Scheduling.h"
#include "invocation.h"

/*
	// For fork callbacks.
*/
static PyObj process_module = NULL;
static int exit_signal = -1;
static pid_t exit_for_pid = -1;

static PyObj
get_hostname(PyObj mod)
{
	char buf[512];
	int r;

	r = gethostname(buf, 512);
	if (r != 0)
	{
		PyErr_SetFromErrno(PyExc_OSError);
		return(NULL);
	}
	buf[511] = '\0';

	return(PyBytes_FromString(buf));
}

static PyObj
get_uname(PyObj mod)
{
	PyObj rob;
	struct utsname un;
	int i;

	if (uname(&un) != 0)
	{
		return(NULL);
	}

	i = 0;
	while (un.sysname[i])
	{
		un.sysname[i] = tolower(un.sysname[i]);
		++i;
	}

	i = 0;
	while (un.machine[i])
	{
		un.machine[i] = tolower(un.machine[i]);
		++i;
	}

	rob = Py_BuildValue("ss", un.sysname, un.machine);
	return(rob);
}

static PyObj
get_clock_ticks(PyObj mod)
{
	int r;
	r = sysconf(_SC_CLK_TCK);
	return(PyLong_FromLong((long) r));
}

static PyObj
set_process_title(PyObj mod, PyObj title)
{
	PyObj bytes;

	#if defined(__MACH__) || defined(__linux__)
		;
	#else
		/*
			// No support on darwin.
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

static void
prepare(void)
{
	pthread_mutex_lock(&forking_mutex);
	pipe(forking_pipe);
}

static struct inherit fork_data = {-1};

/**
	// Execute the &.process._after_fork_parent object from a pending call.
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
	// Execute the &.process._after_fork_child object from a pending call.
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
		fprintf(stderr, "[!* kernel._exit_by_signal: signal, %d, did not terminate process]\n", exit_signal);
		abort();
	}
}

/**
	// Register low-level atexit handler for exiting via a signal.
*/
static PyObj
signalexit(PyObj mod, PyObj ob)
{
	long signo;
	pid_t p;

	signo = PyLong_AsLong(ob);
	if (PyErr_Occurred())
		return(NULL);

	p = getpid();

	if (exit_signal == -1 || exit_for_pid != p)
		atexit(_exit_by_signal);

	exit_for_pid = p;
	exit_signal = signo;

	Py_RETURN_NONE;
}

/**
	// Ensure that the kport is preserved across process images.
	// Used by system to hold on to listening sockets.

	// Generally, most file descriptors created by &.system will have
	// the FD_CLOEXEC flag set.
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

static PyObj
kport_set_cloexec(PyObj mod, PyObj seq)
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

		flag = fcntl((int) fd, F_SETFD, (flag | FD_CLOEXEC));
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
k_initialize(PyObj mod, PyObj ctx)
{
	if (process_module != NULL)
	{
		/* Already configured */
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

extern PyTypeObject EventType;
extern PyTypeObject LinkType;
extern PyTypeObject SchedulerType;
extern PyTypeObject InvocationType;

extern PyTypeObject KPortsType;
KPorts kports_alloc(kport_t, Py_ssize_t);
KPorts kports_create(kport_t[], Py_ssize_t);
struct KPortsAPI _kp_apis = {
	&KPortsType,
	kports_alloc,
	kports_create,
};

#include <kext.h>
CONCEAL(struct ExtensionInterfaces)
fault_python_ext_if = {
	&_kp_apis,
	NULL,
};

#define PortsType KPortsType
#define PYTHON_TYPES() \
	ID(Ports) \
	ID(Event) \
	ID(Link) \
	ID(Invocation) \
	ID(Scheduler)

#define k_preserve kport_clear_cloexec
#define k_released kport_set_cloexec
#define k_hostname get_hostname
#define k_machine_execution_context get_uname
#define k_machine get_uname
#define k_clockticks get_clock_ticks
#define k_set_process_title set_process_title
#define k_signalexit signalexit

#define PyMethod_Id(N) k_##N
#define MODULE_FUNCTIONS() \
	PyMethod_Sole(preserve), \
	PyMethod_Sole(released), \
	PyMethod_None(hostname), \
	PyMethod_None(machine_execution_context), \
	PyMethod_None(machine), \
	PyMethod_None(clockticks), \
	PyMethod_Sole(set_process_title), \
	PyMethod_Sole(signalexit), \
	PyMethod_Sole(initialize),

#include <fault/python/module.h>
INIT(module, 0, NULL)
{
	PyObj xi_ob = NULL, kp_api_ob = NULL;

	#define ID(NAME) \
		if (PyType_Ready((PyTypeObject *) &( NAME##Type ))) \
			goto error; \
		if (PyModule_AddObject(module, #NAME, (PyObj) &( NAME##Type )) < 0) \
			goto error;
		PYTHON_TYPES()
	#undef ID

	if (PyModule_AddStringConstant(module, "fv_architecture", FV_ARCHITECTURE_STR))
		goto error;
	if (PyModule_AddStringConstant(module, "fv_system", FV_SYSTEM_STR))
		goto error;

	if (PyModule_AddIntConstant(module, "machine_addressing", sizeof(void *) * 8))
		goto error;

	xi_ob = PyCapsule_New(&fault_python_ext_if, "__xi__", NULL);
	if (xi_ob == NULL)
		goto error;

	if (PyModule_AddObject(module, "__xi__", xi_ob))
		goto error;

	kp_api_ob = PyCapsule_New(&_kp_apis, "_kports_api", NULL);
	if (kp_api_ob == NULL)
		goto error;

	if (PyModule_AddObject(module, "_kports_api", kp_api_ob))
		goto error;

	return(0);

	error:
	{
		Py_XDECREF(xi_ob);
		Py_XDECREF(kp_api_ob);
		return(-1);
	}
}
#undef PyMethod_Id
