/**
	// Mutable integer array implementation specifically for holding file descriptors.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>
#include <fault/python/injection.h>

#include <kcore.h>
#include <kports.h>

PyTypeObject KPortsType;
#define _kports_alloc(x) KPortsType.tp_alloc(&KPortsType, x)

CONCEAL(KPorts)
kports_alloc(kport_t fill, Py_ssize_t length)
{
	Py_ssize_t i;
	KPorts kp;

	PYTHON_RECEPTACLE(NULL, ((PyObj *) &kp), _kports_alloc, length);
	if (kp == NULL)
		return(NULL);

	for (i = 0; i < length; ++i)
		KPorts_SetItem(kp, i, fill);

	return(kp);
}

CONCEAL(KPorts)
kports_create(kport_t data[], Py_ssize_t length)
{
	KPorts kp;

	PYTHON_RECEPTACLE(NULL, ((PyObj *) &kp), _kports_alloc, length);
	if (kp == NULL)
		return(NULL);

	memcpy(KPorts_GetArray(kp), data, length * sizeof(kport_t));

	return(kp);
}

STATIC(PyObj)
kports_close(PyObj self)
{
	Py_ssize_t pos;
	KPorts kp = (KPorts) self;

	for (pos = 0; pos < KPorts_GetLength(kp); ++pos)
	{
		/* Check and report failures */
		close(KPorts_GetItem(kp, pos));
	}

	Py_RETURN_NONE;
}

STATIC(PyObj)
kports_enter(PyObj self)
{
	Py_RETURN_NONE;
}

STATIC(PyObj)
kports_exit(PyObj self, PyObj args)
{
	PyObj exc, val, tb;

	if (!PyArg_ParseTuple(args, "OOO", &exc, &val, &tb))
		return(NULL);

	if (exc != NULL)
		kports_close(self);

	Py_RETURN_NONE;
}

STATIC(PyObj)
kports_allocate(PyTypeObject *subtype, PyObj length)
{
	KPorts kp;
	PyObj rob;
	Py_ssize_t l;

	l = PyLong_AsSsize_t(length);

	if (PyErr_Occurred())
		return(NULL);

	rob = subtype->tp_alloc(subtype, l);
	if (rob == NULL)
		return(NULL);

	kp = (KPorts) rob;

	for (--l; l > -1; --l)
		KPorts_SetItem(kp, l, -1);

	return(rob);
}

STATIC(PyMethodDef)
kports_methods[] = {
	{"__enter__",
		(PyCFunction) kports_enter,
		METH_NOARGS,
		PyDoc_STR("Context manager open returning &None.")
	},

	{"__exit__",
		(PyCFunction) kports_exit,
		METH_VARARGS,
		PyDoc_STR("Close the contained file descriptors upon error.")
	},

	{"allocate",
		(PyCFunction) kports_allocate,
		METH_O|METH_CLASS,
		PyDoc_STR("Allocate an instance according to the given length.")
	},

	{"close",
		(PyCFunction) kports_close,
		METH_NOARGS,
		PyDoc_STR("Close all non-negative file descriptors contained in the sequence.")
	},

	{NULL,}
};

STATIC(PyObj)
kports_richcompare(PyObj self, PyObj x, int op)
{
	KPorts a = (KPorts) self, b = (KPorts) x;
	PyObj rob;

	if (!PyObject_IsInstance(x, ((PyObj) &KPortsType)))
	{
		Py_INCREF(Py_NotImplemented);
		return(Py_NotImplemented);
	}

	switch (op)
	{
		case Py_NE:
		case Py_EQ:
			rob = Py_False;

			if (KPorts_GetLength(a) == KPorts_GetLength(b))
			{
				char *amb = (char *) KPorts_GetArray(a);
				char *bmb = (char *) KPorts_GetArray(b);
				if (memcmp(amb, bmb, KPorts_GetLength(a)*sizeof(kport_t)) == 0)
				{
					rob = Py_True;
					Py_INCREF(rob);
					break;
				}
			}

			if (op == Py_NE)
			{
				/*
					// Invert result.
				*/
				rob = (rob == Py_True) ? Py_False : Py_True;
			}
			Py_INCREF(rob);
		break;

		default:
			PyErr_SetString(PyExc_TypeError, "kernel.Ports only supports equality");
			rob = NULL;
		break;
	}

	return(rob);
}

STATIC(Py_ssize_t)
kports_length(PyObj self)
{
	return(KPorts_GetLength(((KPorts) self)));
}

STATIC(PyObj)
kports_concat(PyObj self, PyObj x)
{
	KPorts a = (KPorts) self, b = (KPorts) x;
	KPorts kp;
	Py_ssize_t al = KPorts_GetLength(a);

	kp = (KPorts) Py_TYPE(self)->tp_alloc(Py_TYPE(self), al + KPorts_GetLength(b));
	Py_SIZE(kp) = al + KPorts_GetLength(b);
	if (kp == NULL)
		return(NULL);

	memcpy(KPorts_GetArray(kp), KPorts_GetArray(a), sizeof(kport_t) * KPorts_GetLength(a));
	memcpy(&(KPorts_GetArray(kp)[al]), KPorts_GetArray(b), sizeof(kport_t) * KPorts_GetLength(b));

	return((PyObj) kp);
}

STATIC(PyObj)
kports_repeat(PyObj self, Py_ssize_t quantity)
{
	KPorts a = (KPorts) self;
	KPorts kp;
	ssize_t al = KPorts_GetLength(a);
	Py_ssize_t i;

	kp = (KPorts) Py_TYPE(self)->tp_alloc(Py_TYPE(self), al * quantity);
	if (kp == NULL)
		return(NULL);

	for (i = 0; i < quantity; ++i)
		memcpy(&(KPorts_GetArray(kp)[al*i]), KPorts_GetArray(a), al);

	return((PyObj) kp);
}

STATIC(PyObj)
kports_getitem(PyObj self, Py_ssize_t index)
{
	KPorts kp = (KPorts) self;

	if (index >= KPorts_GetLength(kp))
	{
		PyErr_SetString(PyExc_IndexError, "index out of bounds");
		return(NULL);
	}

	return(PyLong_FromLong((long) KPorts_GetItem(kp, index)));
}

STATIC(int)
kports_setitem(PyObj self, Py_ssize_t index, PyObj val)
{
	KPorts kp = (KPorts) self;

	long l;
	l = PyLong_AsLong(val);

	if (PyErr_Occurred())
		return(-1);

	if (l > INT_MAX || l < INT_MIN)
	{
		PyErr_SetString(PyExc_OverflowError, "assigned file descriptor is out of range");
		return(-1);
	}

	KPorts_SetItem(kp, index, (kport_t) l);
	return(0);
}

static PySequenceMethods
kports_as_sequence = {
	kports_length,
	kports_concat,
	kports_repeat,
	kports_getitem,
	NULL,
	kports_setitem,
};

STATIC(PyObj)
kports_subscript(PyObj self, PyObj item)
{
	PyObj rob;
	KPorts kp = (KPorts) self;

	if (PyObject_IsInstance(item, (PyObj) &PySlice_Type))
	{
		KPorts skp;
		Py_ssize_t start, stop, step, slen, i, ii = 0;
		if (PySlice_GetIndicesEx(item, Py_SIZE(self), &start, &stop, &step, &slen))
			return(NULL);

		skp = kports_alloc(-1, slen);
		if (skp == NULL)
			return(NULL);
		rob = skp;

		for (i = start; i < stop; i += step)
		{
			KPorts_SetItem(skp, ii, KPorts_GetItem(kp, i));
			++ii;
		}
	}
	else
	{
		PyObj lo;
		Py_ssize_t i;
		lo = PyNumber_Long(item);
		if (lo == NULL)
			return(NULL);

		i = PyLong_AsSsize_t(lo);
		Py_DECREF(lo);

		if (i < 0)
			i = i + Py_SIZE(self);

		if (i > Py_SIZE(self) || i < 0)
		{
			PyErr_SetString(PyExc_IndexError, "out of range");
			rob = NULL;
		}
		else
			rob = kports_getitem(self, i);
	}

	return(rob);
}

STATIC(PyMappingMethods)
kports_as_mapping = {
	kports_length,
	kports_subscript,
	NULL,
};

STATIC(int)
kports_getbuffer(PyObj self, Py_buffer *view, int flags)
{
	KPorts kp = (KPorts) self;
	return(PyBuffer_FillInfo(view, self, KPorts_GetArray(kp), Py_SIZE(self) * sizeof(kport_t), 0, flags));
}

STATIC(PyBufferProcs)
kports_buffer = {
	kports_getbuffer,
	NULL,
};

STATIC(PyObj)
kports_new(PyTypeObject *subtype, PyObj args, PyObj kw)
{
	static char *kwlist[] = {"iterable", NULL};
	PyObj rob, lob, iterable;

	KPorts kp;
	Py_ssize_t count, i;
	kport_t *kr;

	if (!PyArg_ParseTupleAndKeywords(args, kw, "O", kwlist, &iterable))
		return(NULL);

	if (!PyList_Check(iterable))
	{
		PyObj fdo = NULL;
		lob = PyList_New(0);

		PyLoop_ForEach(iterable, &fdo)
		{
			if (PyList_Append(lob, fdo))
				break;
		}
		PyLoop_CatchError(iterable)
		{
			Py_DECREF(lob);
			return(NULL);
		}
		PyLoop_End(iterable)
	}
	else
	{
		lob = iterable;
		Py_INCREF(lob);
	}

	count = PyList_GET_SIZE(lob);

	rob = subtype->tp_alloc(subtype, count);
	if (rob == NULL)
	{
		Py_DECREF(lob);
		return(NULL);
	}

	kp = (KPorts) rob;

	for (i = 0; i < count; ++i)
	{
		PyObj ob = PyList_GET_ITEM(lob, i);
		long l;

		l = PyLong_AsLong(ob);
		if (PyErr_Occurred())
			goto error;

		if (l > INT_MAX || l < INT_MIN)
		{
			PyErr_SetString(PyExc_OverflowError, "given file descriptor out of range");
			goto error;
		}

		KPorts_SetItem(kp, i, (kport_t) l);
	}

	Py_DECREF(lob);
	return((PyObj) kp);

	error:
	{
		Py_DECREF(rob);
		Py_DECREF(lob);
		return(NULL);
	}
}

STATIC(void)
kports_dealloc(PyObj self)
{
	Py_TYPE(self)->tp_free(self);
}

PyTypeObject
KPortsType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	PYTHON_MODULE_PATH("Ports"),    /* tp_name */
	sizeof(struct KPorts),          /* tp_basicsize */
	sizeof(kport_t),                /* tp_itemsize */
	kports_dealloc,                 /* tp_dealloc */
	NULL,                           /* tp_print */
	NULL,                           /* tp_getattr */
	NULL,                           /* tp_setattr */
	NULL,                           /* tp_compare */
	NULL,                           /* tp_repr */
	NULL,                           /* tp_as_number */
	&kports_as_sequence,            /* tp_as_sequence */
	&kports_as_mapping,             /* tp_as_mapping */
	NULL,                           /* tp_hash */
	NULL,                           /* tp_call */
	NULL,                           /* tp_str */
	NULL,                           /* tp_getattro */
	NULL,                           /* tp_setattro */
	&kports_buffer,                 /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,             /* tp_flags */
	NULL,                           /* tp_doc */
	NULL,                           /* tp_traverse */
	NULL,                           /* tp_clear */
	kports_richcompare,             /* tp_richcompare */
	0,                              /* tp_weaklistoffset */
	NULL,                           /* tp_iter */
	NULL,                           /* tp_iternext */
	kports_methods,                 /* tp_methods */
	NULL,                           /* tp_members */
	NULL,                           /* tp_getset */
	NULL,                           /* tp_base */
	NULL,                           /* tp_dict */
	NULL,                           /* tp_descr_get */
	NULL,                           /* tp_descr_set */
	0,                              /* tp_dictoffset */
	NULL,                           /* tp_init */
	NULL,                           /* tp_alloc */
	kports_new,                     /* tp_new */
};
