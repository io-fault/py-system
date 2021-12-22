/**
	// Task queue implementation for Python.
*/
#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "taskq.h"

#ifndef INITIAL_TASK_COUNT
	#define INITIAL_TASK_COUNT 4
#endif

#ifndef MAX_TASKS_PER_SEGMENT
	#define MAX_TASKS_PER_SEGMENT 128
#endif

#define TASKQ_MEMORY_ERROR(string) \
	PyErr_SetString(PyExc_MemoryError, string)
#define TASKQ_PARALLEL_ERROR(string) \
	PyErr_SetString(PyExc_RuntimeError, string)

#define TASKQ_FULL(TQ) (TQ->q_tailcursor == TQ->q_tail->t_allocated)

#define TASKQ_ALLOCATION_SIZE(STRUCT, ITEM, COUNT) \
	(sizeof(STRUCT) + (COUNT * sizeof(ITEM)))

#define TASKQ_MEMORY_ACQUIRE PyMem_Malloc
#define TASKQ_MEMORY_RELEASE PyMem_Free
#define TASKQ_ALLOCATE(N) \
	TASKQ_MEMORY_ACQUIRE(TASKQ_ALLOCATION_SIZE(struct Tasks, PyObject *, N))

/**
	// Append a memory allocation to the task queue.
*/
CONCEAL(int)
taskq_extend(TaskQueue tq)
{
	Tasks new;
	Tasks tail = tq->q_tail;
	size_t count = tail->t_allocated;

	if (count < MAX_TASKS_PER_SEGMENT)
		count *= 2;
	else
		count = MAX_TASKS_PER_SEGMENT;

	new = TASKQ_ALLOCATE(count);
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

/**
	// Insert task at the end of the queue.
	// Presume available space.
*/
STATIC(int) inline
taskq_insert(TaskQueue tq, PyObj callable)
{
	Tasks tail = tq->q_tail;
	void **slot = &tail->t_vector[tq->q_tailcursor];

	++tq->q_tailcursor;
	*slot = (void *) callable;

	return(0);
}

/**
	// High-level append.
*/
CONCEAL(int)
taskq_enqueue(TaskQueue tq, PyObj callable)
{
	if (TASKQ_FULL(tq))
	{
		/**
			// Extend allocation.
		*/
		if (taskq_extend(tq) < 0)
		{
			TASKQ_MEMORY_ERROR("task queue could not be extended and must be flushed");
			return(-1);
		}
	}

	return(taskq_insert(tq, callable));
}

/**
	// Handle error cases using &errctl and &PyErr_WriteUnraisable
	// when it or other critical operations fail.
*/
STATIC(void)
trap_execution_error(PyObj errctl, PyObj errctx, PyObj task)
{
	PyObj rob;
	PyObj exc = NULL, val = NULL, tb = NULL;

	if (errctl == NULL)
	{
		/* Oddly trifling to avoid PyErr_PrintEx exiting the process. */

		if (PyErr_ExceptionMatches(PyExc_SystemExit))
		{
			PyObj sysexit = NULL;
			PyErr_Fetch(&exc, &sysexit, &tb);
			PyErr_NormalizeException(&exc, &sysexit, &tb);
			if (tb != NULL)
				PyException_SetTraceback(sysexit, tb);
			Py_CLEAR(exc);
			Py_CLEAR(tb);

			PyErr_SetString(PyExc_RuntimeError, "system exit raised in task");
			PyErr_Fetch(&exc, &val, &tb);
			PyErr_NormalizeException(&exc, &val, &tb);
			PyException_SetCause(val, sysexit);
			PyErr_Restore(exc, val, tb);
		}

		PyErr_PrintEx(0);
		return;
	}

	PyErr_Fetch(&exc, &val, &tb);
	PyErr_NormalizeException(&exc, &val, &tb);
	if (PyErr_Occurred())
	{
		/* normalization failed? */
		PyErr_WriteUnraisable(task);
	}
	else
	{
		if (tb != NULL)
			PyException_SetTraceback(val, tb);

		rob = PyObject_CallFunctionObjArgs(errctl, errctx, task, val, NULL);
		if (rob != NULL)
			Py_DECREF(rob);
		else
		{
			/* errctl raised exception */
			PyErr_WriteUnraisable(task);
		}
	}

	Py_XDECREF(tb);
	Py_XDECREF(exc);
	Py_XDECREF(val);
}

/**
	// Pop segments from &TaskQueue.q_executing.
*/
STATIC(int)
taskq_continue(TaskQueue tq)
{
	Tasks n = NULL;

	/* Allocate new loading queue. */
	n = TASKQ_ALLOCATE(INITIAL_TASK_COUNT);
	if (n == NULL)
	{
		TASKQ_MEMORY_ERROR("could not allocate memory for queue continuation");
		return(-1);
	}
	n->t_next = NULL;
	n->t_allocated = INITIAL_TASK_COUNT;

	/* Rotate loading into executing. */
	tq->q_tail->t_allocated = tq->q_tailcursor;
	tq->q_executing = tq->q_loading;

	/* Update loading to use new allocation. */
	tq->q_tail = tq->q_loading = n;
	tq->q_tailcursor = 0;

	return(0);
}

/**
	// Execute the tasks in the &TaskQueue.q_executing,
	// and rotate &TaskQueue.q_loading for the next cycle.
*/
CONCEAL(int)
taskq_execute(TaskQueue tq, PyObj errctl, PyObj errctx)
{
	Tasks exec = tq->q_executing;
	Tasks next = NULL;
	PyObj task, xo;
	int i, c, total = 0;

	if (exec == NULL)
	{
		TASKQ_PARALLEL_ERROR("concurrent task queue execution");
		return(-1);
	}

	/* signals processing */
	tq->q_executing = NULL;

	do {
		for (i = 0, c = exec->t_allocated; i < c; ++i)
		{
			task = exec->t_vector[i];
			exec->t_vector[i] = NULL;

			xo = PyObject_CallObject(task, NULL);
			total += 1;

			if (xo == NULL)
				trap_execution_error(errctl, errctx, task);
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
			return(-(total+1));
		}
	}
	else
	{
		/* loading queue is empty; create empty executing queue */
		tq->q_executing = TASKQ_ALLOCATE(0);
		tq->q_executing->t_allocated = 0;
		tq->q_executing->t_next = NULL;
	}

	return(total);
}

/**
	// Release all objects held by the queue.
*/
CONCEAL(void)
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
		// The segment's allocation is released in the normal case below.
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

/**
	// Container traversal for GC support.
*/
CONCEAL(int)
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

	return(0);
}

/**
	// Initialize the given, already allocated, task queue.
*/
CONCEAL(int)
taskq_initialize(TaskQueue tq)
{
	tq->q_loading = TASKQ_ALLOCATE(INITIAL_TASK_COUNT);
	if (tq->q_loading == NULL)
	{
		PyErr_SetString(PyExc_MemoryError, "could not allocate memory for queue");
		return(-1);
	}

	tq->q_loading->t_next = NULL;
	tq->q_loading->t_allocated = INITIAL_TASK_COUNT;

	tq->q_executing = TASKQ_ALLOCATE(0);
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
