/**
	// KPorts header providing access to &.kernel.KPorts objects.
*/
#ifndef _SYSTEM_KERNEL_KPORTS_H_included_
#define _SYSTEM_KERNEL_KPORTS_H_included_ 1

struct KPorts {
	PyObject_VAR_HEAD
	kport_t kp_array[0];
};

typedef struct KPorts *KPorts;

#define KPorts_GetArray(KP) (KP->kp_array)
#define KPorts_GetItem(KP, IDX) (KPorts_GetArray(KP)[IDX])
#define KPorts_SetItem(KP, IDX, VAL) (KPorts_GetArray(KP)[IDX]) = VAL
#define KPorts_GetLength(KP) Py_SIZE(KP)

struct KPortsAPI {
	PyTypeObject *type;
	KPorts (*alloc)(kport_t, Py_ssize_t);
	KPorts (*create)(kport_t[], Py_ssize_t);
};
#endif
