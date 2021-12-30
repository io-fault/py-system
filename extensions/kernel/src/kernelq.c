/**
	// Common KernelQueue functions often supported via macro abstractions.
*/
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fault/libc.h>
#include <fault/internal.h>
#include <fault/python/environ.h>

#include "Scheduling.h"

CONCEAL(void)
kernelq_clear(KernelQueue kq)
{
	Py_CLEAR(kq->kq_references);
	Py_CLEAR(kq->kq_cancellations);
}

CONCEAL(int)
kernelq_traverse(KernelQueue kq, visitproc visit, void *arg)
{
	Py_VISIT(kq->kq_references);
	Py_VISIT(kq->kq_cancellations);
	return(0);
}

/**
	// Replace the link in the reference dictionary, but return
	// the existing object if any.
*/
CONCEAL(int)
kernelq_reference_update(KernelQueue kq, Link ln, PyObj *current)
{
	*current = PyDict_GetItem(kq->kq_references, ln->ln_event);

	if (*current != NULL)
	{
		/* Acquire the necessary reference to allow substitution. */
		if (PyList_Append(kq->kq_cancellations, *current) < 0)
			return(-1);
	}

	/* Insert or replace. */
	if (PyDict_SetItem(kq->kq_references, ln->ln_event, ln) < 0)
		return(-2);

	return(0);
}

CONCEAL(int)
kernelq_reference_delete(KernelQueue kq, Event ev)
{
	PyObj current = PyDict_GetItem(kq->kq_references, (PyObj) ev);

	if (current != NULL)
	{
		/* Acquire the necessary reference to allow substitution. */
		if (PyList_Append(kq->kq_cancellations, current) < 0)
			return(-1);
	}

	if (PyDict_DelItem(kq->kq_references, ev) < 0)
		return(-2);

	return(0);
}


/**
	// Set or clear the cyclic flag on &ln or raise an error if
	// it's not acceptable for the given filter.
*/
CONCEAL(int)
kernelq_cyclic_event(KernelQueue kq, int cyclic, Link ln, kevent_t *kev)
{
	switch (cyclic)
	{
		case -1:
		{
			/* Inherit from &kernelq_identify. */
			if (AEV_CYCLIC(kev))
				Link_Set(ln, cyclic);
			else
				Link_Clear(ln, cyclic);
		}
		break;

		case 0:
		{
			/* All events support one shot. */
			Link_Clear(ln, cyclic);
			AEV_CYCLIC_DISABLE(kev);
		}
		break;

		case 1:
		{
			/* Not all events allow cyclic. (process exit) */
			if (!AEV_CYCLIC(kev))
			{
				PyErr_SetString(PyExc_ValueError, "cyclic behavior not supported on event");
				return(-1);
			}

			Link_Set(ln, cyclic);
		}
		break;

		default:
		{
			PyErr_SetString(PyExc_ValueError, "invalid cyclic identifier");
			return(-2);
		}
	}

	return(0);
}

CONCEAL(int)
kernelq_transition(KernelQueue kq, TaskQueue tq)
{
	PyObj refset = kq->kq_references;
	PyObj cancelset = kq->kq_cancellations;

	for (; kq->kq_event_position < kq->kq_event_count; ++(kq->kq_event_position))
	{
		PyObj task;
		Link ln;
		kevent_t *kev;

		kev = &(kq->kq_array[kq->kq_event_position]);
		ln = AEV_LINK(kev);

		/* &.kernel.Scheduler.interrupt */
		if (ln == NULL)
		{
			kernelq_interrupt_accept(kq);
			continue;
		}

		assert(Py_TYPE(ln) == &LinkType);

		if (taskq_enqueue(tq, ln) < 0)
			return(-1);
		Py_INCREF(ln);

		/**
			// If the task is not cyclic, cancel.
		*/
		if (!Link_Get(ln, cyclic))
		{
			kport_t kp = -1;
			kp = Event_GetKPort(ln->ln_event);

			/**
				// If the kevent was recognized as cyclic by the system,
				// use kernelq_delta to delete the event. Otherwise,
				// presume that the event was deleted already.
			*/
			if (AEV_CYCLIC(kev))
			{
				if (kernelq_delta(kq, AEV_DELETE, kp, kev) < 0)
				{
					PyErr_SetFromErrno(PyExc_OSError);
					PyErr_WriteUnraisable(ln);
				}
			}

			Link_Set(ln, cancelled);
			if (PyDict_DelItem(refset, ln->ln_event) < 0)
				PyErr_WriteUnraisable(ln);
		}
	}

	/*
		// Clear cancellation references.
	*/
	if (PyList_GET_SIZE(cancelset) > 0)
	{
		PyObj r = PyObject_CallMethod(cancelset, "clear", "", NULL);
		if (r == NULL)
			PyErr_WriteUnraisable(NULL);
		else
			Py_DECREF(r);
	}

	return(0);
}

CONCEAL(PyObj)
kernelq_cancel(KernelQueue kq, Event ev)
{
	kport_t kp = -1;
	kevent_t kev = {0,};
	PyObj original;

	if (kernelq_identify(&kev, Event_Specification(ev)) < 0)
	{
		/* Unrecognized EV_TYPE */
		PyErr_SetString(PyExc_TypeError, "unrecognized event type");
		return(NULL);
	}

	/*
		// Prepare for cancellation by adding the existing
		// scheduled &ev to the cancellation list. This
		// allows any concurrently collected event to be
		// safely enqueued.
	*/

	original = PyDict_GetItem(kq->kq_references, ev); /* borrowed */
	if (original == NULL)
	{
		/* No event in table. No cancellation necessary. */
		if (PyErr_Occurred())
			return(NULL);
		else
			Py_RETURN_NONE;
	}

	/*
		// Maintain reference count in case of concurrent use(collected event).
	*/
	if (PyList_Append(kq->kq_cancellations, original) < 0)
		return(NULL);

	/* Delete link from references */
	if (PyDict_DelItem(kq->kq_references, ev) < 0)
	{
		/* Nothing has actually changed here, so just return the error. */
		/* Cancellation list will be cleared by &.kernel.Scheduler.wait */
		return(NULL);
	}

	if (kernelq_delta(kq, AEV_DELETE, Event_GetKPort(ev), &kev) < 0)
	{
		Link prior = (Link) original;

		/*
			// Cancellation failed.
			// Likely a critical error, but try to recover.
		*/
		if (PyDict_SetItem(kq->kq_references, prior->ln_event, original) < 0)
		{
			/*
				// Could not restore the reference position.
				// Prefer leaking the operation over risking a SIGSEGV
				// after the kq_cancellations list has been cleared.
			*/
			Py_INCREF(original);
			PyErr_WarnFormat(PyExc_RuntimeWarning, 0,
				"event link leaked due to cancellation failure");
		}

		return(NULL);
	}

	Py_RETURN_NONE;
}
