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
