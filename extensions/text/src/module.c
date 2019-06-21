/**
	# System text services.
*/
#include <wchar.h>
#include <locale.h>
#include <langinfo.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

static PyObj
cells(PyObj self, PyObj parameter)
{
	int width;
	Py_ssize_t len, size;
	PyObj str;

	/**
		# str(parameter) here in order to allow callers to avoid the composition.
	*/
	str = PyObject_Str(parameter);
	if (str == NULL)
		return(NULL);

	if (!PyUnicode_Check(str))
	{
		PyErr_Format(PyExc_ValueError, "parameter was not properly converted to str object");
		goto error;
	}

	len = PyUnicode_GET_LENGTH(str);

	/**
		# Fast path for ascii strings.
		# XXX: Currently inconsistent with wcswidth regarding control characters.
	*/
	if (PyUnicode_IS_ASCII(str) || len == 0)
	{
		Py_DECREF(str);
		return(PyLong_FromSsize_t(len));
	}

	/**
		# Presume stack allocation is faster.
		# If the codepoint is represented with a surrogate pair, wcswidth currently
		# doesn't handle it anyways.
	*/
	if (len == 1)
	{
		wchar_t sawc[2];

		size = PyUnicode_AsWideChar(str, sawc, 1);
		if (size == -1)
			goto error;

		width = wcwidth(sawc[0]);
		goto fexit;
	}

	/*
		# goto skipped.
	*/
	{
		wchar_t *wstr;

		/*
			# Switch to heap.
		*/
		wstr = PyUnicode_AsWideCharString(str, &size);
		if (wstr == NULL)
			goto error;

		width = wcswidth(wstr, (size_t) size);
		PyMem_Free(wstr);
	}

	fexit:
		Py_DECREF(str);
		return(PyLong_FromLong((long) width));

	error:
		Py_DECREF(str);
		return(NULL);
}

static PyObj
dsetlocale(PyObj self)
{
	char *selection = setlocale(LC_ALL, "");

	if (selection == NULL)
	{
		PyErr_Format(PyExc_RuntimeError,
			"could not set native environment locale; setlocale(2) returned NULL");
		return(NULL);
	}

	return(PyUnicode_FromString(selection));
}

static PyObj
get_encoding(PyObj self)
{
	char *encoding = nl_langinfo(CODESET);
	if (encoding[0] == '\000')
		Py_RETURN_NONE;

	return(PyUnicode_FromString(encoding));
}

#define MODULE_FUNCTIONS() \
	PYMETHOD(cells, cells, METH_O, \
		"get the number of cells the given unicode string requires " \
		"for display in a monospaced character matrix.") \
	PYMETHOD(encoding, get_encoding, METH_NOARGS, \
		"get the CODESET string using the system's nl_langinfo(2)") \
	PYMETHOD(setlocale, dsetlocale, METH_NOARGS, \
		"limited setlocale(2) interface providing access to setting the native environment locale.")

#include <fault/python/module.h>
INIT("interfaces to system text services: wcswidth and setlocale.")
{
	PyObj mod = NULL;

	CREATE_MODULE(&mod);
	if (mod == NULL)
		return(NULL);

	return(mod);

	fail:
		DROP_MODULE(mod);
		return(NULL);
}
