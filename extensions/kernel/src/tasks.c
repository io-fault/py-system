/**
	// Task queue implementation for Python.
*/
#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "tasks.h"

#ifndef INITIAL_TASKS_ALLOCATED
	#define INITIAL_TASKS_ALLOCATED 4
#endif

#ifndef MAX_TASKS_PER_SEGMENT
	#define MAX_TASKS_PER_SEGMENT 128
#endif

#define TASKQ_MEMORY_ACQUIRE PyMem_Malloc
#define TASKQ_MEMORY_RELEASE PyMem_Free

#define TASKQ_MEMORY_ERROR(string) \
	PyErr_SetString(PyExc_MemoryError, string)
#define TASKQ_PARALLEL_ERROR(string) \
	PyErr_SetString(PyExc_RuntimeError, string)

#define TASKQ_ALLOCATION_SIZE(STRUCT) \
	(sizeof(struct Tasks) + (INITIAL_TASKS_ALLOCATED * sizeof(STRUCT)))

/**
	// Append a memory allocation to the task queue.
*/
SYMBOL(int)
taskq_extend(TaskQueue tq)
{
	Tasks tail = tq->q_tail;
	Tasks new;
	size_t count = tail->t_allocated;

	if (count < MAX_TASKS_PER_SEGMENT)
		count *= 2;

	new = TASKQ_MEMORY_ACQUIRE((sizeof(struct Tasks) + (sizeof(PyObject *) * count)));
	if (new == NULL)
		return(-1);

	new->t_next = NULL;
	new->t_allocated = count;
	tq->q_tail->t_next = new;

	/* update position */
	tq->q_tail = new;
	tq->q_tailcursor = 0;

	return(0);
}

SYMBOL(int)
taskq_enqueue(TaskQueue tq, PyObj callable)
{
	Tasks tail = tq->q_tail;

	if (tq->q_tailcursor == tail->t_allocated)
	{
		/**
			// Error actually occurred in the prior call, but the insertion of &callable
			// did succeed.
		*/
		TASKQ_MEMORY_ERROR("task queue could not be extended and must be flushed");
		return(-1);
	}

	tail->t_vector[tq->q_tailcursor++] = callable;
	Py_INCREF(callable);

	if (tq->q_tailcursor == tail->t_allocated)
		return(taskq_extend(tq));

	return(0);
}

/**
	// Handle error cases using &errctl and &PyErr_WriteUnraisable
	// when it or other critical operations fail.
*/
static void
trap_execution_error(PyObj errctl, PyObj task)
{
	PyObj exc, val, tb;
	PyErr_Fetch(&exc, &val, &tb);

	if (errctl != Py_None)
	{
		PyObj ereturn;

		PyErr_NormalizeException(&exc, &val, &tb);
		if (PyErr_Occurred())
		{
			/* normalization failed? */
			PyErr_WriteUnraisable(task);
			PyErr_Clear();
		}
		else
		{
			if (tb != NULL)
			{
				PyException_SetTraceback(val, tb);
				Py_DECREF(tb);
			}

			ereturn = PyObject_CallFunctionObjArgs(errctl, task, val, NULL);
			if (ereturn != NULL)
				Py_DECREF(ereturn);
			else
			{
				/* errctl raised exception */
				PyErr_WriteUnraisable(task);
				PyErr_Clear();
			}
		}
	}
	else
	{
		/* explicitly discarded */
		Py_XDECREF(tb);
	}

	Py_XDECREF(exc);
	Py_XDECREF(val);
}

/**
	// Pop segments from &TaskQueue.q_executing.
*/
static int
taskq_continue(TaskQueue tq)
{
	Tasks n = NULL;

	n = TASKQ_MEMORY_ACQUIRE((TASKQ_ALLOCATION_SIZE(PyObject *)));
	if (n == NULL)
	{
		TASKQ_MEMORY_ERROR("could not allocate memory for queue continuation");
		return(-1);
	}

	tq->q_tail->t_allocated = tq->q_tailcursor;
	tq->q_executing = tq->q_loading;

	tq->q_tail = tq->q_loading = n;
	tq->q_loading->t_next = NULL;
	tq->q_loading->t_allocated = INITIAL_TASKS_ALLOCATED;

	tq->q_tailcursor = 0;

	return(0);
}

/**
	// Execute the tasks in the &TaskQueue.q_executing,
	// and rotate &TaskQueue.q_loading for the next cycle.
*/
SYMBOL(PyObj)
taskq_execute(TaskQueue tq, PyObj errctl)
{
	Tasks exec = tq->q_executing;
	Tasks next = NULL;
	PyObj task, xo;
	int i, c, total = 0;

	if (exec == NULL)
	{
		TASKQ_PARALLEL_ERROR("concurrent task queue execution");
		return(NULL);
	}

	/* signals processing */
	tq->q_executing = NULL;

	do {
		for (i = 0, c = exec->t_allocated; i < c; ++i)
		{
			task = exec->t_vector[i];
			xo = PyObject_CallObject(task, NULL);
			total += 1;

			if (xo == NULL)
				trap_execution_error(errctl, task);
			else
				Py_DECREF(xo);

			Py_DECREF(task);
		}

		next = exec->t_next;
		TASKQ_MEMORY_RELEASE(exec);
		exec = next;
	}
	while (exec != NULL);

	if (TQ_LQUEUE_HAS_TASKS(tq))
	{
		if (taskq_continue(tq) == -1)
		{
			/* re-init executing somehow? force instance dropped? */
			return(NULL);
		}
	}
	else
	{
		/* loading queue is empty; create empty executing queue */
		tq->q_executing = TASKQ_MEMORY_ACQUIRE(sizeof(struct Tasks));
		tq->q_executing->t_allocated = 0;
		tq->q_executing->t_next = NULL;
	}

	return(PyLong_FromLong((long) total));
}

SYMBOL(void)
taskq_clear(TaskQueue tq)
{
	Tasks t, n;
	size_t i;

	Tasks kx = tq->q_executing;
	Tasks kt = tq->q_tail;
	Tasks kl = tq->q_loading;

	tq->q_executing = tq->q_loading = tq->q_tail = NULL;

	/*
		// Executing queue's t_allocated provides accurate count of the segment.
	*/
	t = kx;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_CLEAR(t->t_vector[i]);

		TASKQ_MEMORY_RELEASE(t);
		t = n;
	}

	/*
		// Special case for final segment in loading.
	*/
	t = kt;
	if (t != NULL)
	{
		for (i = 0; i < tq->q_tailcursor; ++i)
			Py_CLEAR(t->t_vector[i]);

		kt->t_allocated = 0;
	}

	/*
		// Prior loop on tail sets allocated to zero maintaining the invariant.
	*/
	t = kl;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_CLEAR(t->t_vector[i]);

		TASKQ_MEMORY_RELEASE(t);
		t = n;
	}
}

SYMBOL(int)
taskq_traverse(TaskQueue tq, visitproc visit, void *arg)
{
	Tasks t, n;
	size_t i;

	t = tq->q_executing;
	while (t != NULL)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_VISIT(t->t_vector[i]);
		t = n;
	}

	t = tq->q_tail;
	if (t != NULL)
	{
		for (i = 0; i < tq->q_tailcursor; ++i)
			Py_VISIT(t->t_vector[i]);
	}

	t = tq->q_loading;
	while (t != NULL && t != tq->q_tail)
	{
		n = t->t_next;
		for (i = 0; i < t->t_allocated; ++i)
			Py_VISIT(t->t_vector[i]);
		t = n;
	}
}

SYMBOL(int)
taskq_initialize(TaskQueue tq)
{
	tq->q_loading = TASKQ_MEMORY_ACQUIRE(
		sizeof(struct Tasks) + (INITIAL_TASKS_ALLOCATED * sizeof(PyObject *))
	);
	if (tq->q_loading == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	tq->q_loading->t_next = NULL;
	tq->q_loading->t_allocated = INITIAL_TASKS_ALLOCATED;

	tq->q_executing = TASKQ_MEMORY_ACQUIRE(
		sizeof(struct Tasks) + (0 * sizeof(PyObject *))
	);
	if (tq->q_executing == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	tq->q_executing->t_next = NULL;
	tq->q_executing->t_allocated = 0;

	tq->q_tail = tq->q_loading;
	tq->q_tailcursor = 0;
	return(0);
}
