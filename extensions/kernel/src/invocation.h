/**
	// &.kernel.Invocation C-API.
*/
#ifndef _SYSTEM_KERNEL_INVOCATION_H_included_
#define _SYSTEM_KERNEL_INVOCATION_H_included_

struct Invocation {
	PyObject_HEAD

	char *ki_path;
	char **ki_argv;
	char **ki_environ;

	posix_spawnattr_t ki_spawnattr;
	char ki_spawnattr_init;
	char ki_options;
};

typedef struct Invocation *Invocation;
#endif
