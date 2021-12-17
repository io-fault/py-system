/**
	// &.kernel.Link implementation joining events with tasks, an event connection.
*/
#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "Scheduling.h"

STATIC(PyMethodDef)
ln_methods[] = {
	{NULL,},
};

#define LF(FLAG) \
	STATIC(PyObj) \
	ln_get_##FLAG(Link ln, void *closure) \
	{ \
		if (Link_Get(ln, FLAG)) \
			Py_RETURN_TRUE; \
		Py_RETURN_FALSE; \
	}

	LINK_FLAG_LIST()
#undef LF

STATIC(PyGetSetDef)
ln_getset[] = {
	#define LF(FLAG) \
		{#FLAG, ln_get_##FLAG, NULL, NULL},

		LINK_FLAG_LIST()
	#undef LF
	{NULL,},
};

STATIC(PyMemberDef)
ln_members[] = {
	{"context", T_OBJECT, offsetof(struct Link, ln_context), READONLY},
	{"event", T_OBJECT, offsetof(struct Link, ln_event), READONLY},
	{"task", T_OBJECT, offsetof(struct Link, ln_task), READONLY},
	{NULL,},
};

STATIC(Py_hash_t)
ln_hash(Link ln)
{
	Py_hash_t r = (Py_hash_t) ln;

	if (r == -1)
		r = -2;

	return(r);
}

STATIC(PyObj)
ln_richcompare(Link ln, PyObj operand, int cmpop)
{
	Link op;

	if (cmpop != Py_EQ)
		Py_RETURN_NOTIMPLEMENTED;

	/* Exact only. */
	if (ln == operand)
		Py_RETURN_TRUE;

	Py_RETURN_FALSE;
}

STATIC(PyObj)
ln_call(Link ln, PyObj args, PyObj kw)
{
	PyObj rob;

	if (Link_Get(ln, executing))
	{
		PyErr_SetString(PyExc_RuntimeError, "event task already executing");
		return(NULL);
	}

	Link_Set(ln, executing);
	rob = PyObject_CallFunction(ln->ln_task, "O", (PyObj) ln);
	Link_Clear(ln, executing);
	return(rob);
}

STATIC(void)
ln_clear(Link ln)
{
	Py_CLEAR(ln->ln_context);
	Py_CLEAR(ln->ln_event);
	Py_CLEAR(ln->ln_task);
}

STATIC(int)
ln_traverse(PyObj self, visitproc visit, void *arg)
{
	Link ln = (Link) self;

	Py_VISIT(ln->ln_context);
	Py_VISIT(ln->ln_event);
	Py_VISIT(ln->ln_task);
	return(0);
}

STATIC(PyObj)
ln_new(PyTypeObject *typ, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"event", "task", "context", NULL,};
	Event ev = NULL;
	PyObj task = NULL;
	PyObj ctx = NULL;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O!O|$O", kwlist, &EventType, &ev, &task, &ctx))
		return(NULL);

	return(Link_Create(typ, ctx, ev, task));
}

STATIC(void)
ln_dealloc(Link ln)
{
	ln_clear(ln);
	Py_TYPE(ln)->tp_free(ln);
}

/**
	// &.kernel.Link
*/
CONCEAL(PyTypeObject)
LinkType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name = FACTOR_PATH("Link"),
	.tp_basicsize = sizeof(struct Link),
	.tp_itemsize = 0,
	.tp_call = ln_call,
	.tp_flags =
		Py_TPFLAGS_BASETYPE|
		Py_TPFLAGS_DEFAULT|
		Py_TPFLAGS_HAVE_GC,

	.tp_members = ln_members,
	.tp_methods = ln_methods,
	.tp_getset = ln_getset,

	.tp_hash = ln_hash,
	.tp_richcompare = ln_richcompare,

	.tp_clear = ln_clear,
	.tp_traverse = ln_traverse,
	.tp_new = ln_new,
	.tp_dealloc = ln_dealloc,
};

CONCEAL(PyObj)
Link_Create(PyTypeObject *typ, PyObj ctx, Event ev, PyObj task)
{
	Link ln;

	ln = typ->tp_alloc(typ, 0);
	if (ln == NULL)
		return(NULL);

	ln->ln_context = ctx;
	ln->ln_flags = 0;
	ln->ln_event = (PyObj) ev;
	ln->ln_task = task;

	Py_INCREF(ln->ln_event);
	Py_INCREF(ln->ln_task);
	Py_XINCREF(ln->ln_context);

	return(ln);
}
