/**
	// Common defines and interfaces.
*/

#include <sys/types.h>
#include <sys/time.h>

#ifndef HAVE_STDINT_H
	#include <stdint.h>
#endif

#ifndef LOCAL_MONOTONIC_CLOCK_ID
	#define LOCAL_MONOTONIC_CLOCK_ID CLOCK_MONOTONIC
#endif

#ifndef LOCAL_REAL_CLOCK_ID
	#define LOCAL_REAL_CLOCK_ID CLOCK_REALTIME
#endif

#define NS_IN_SEC 1000000000
#define SUBSECOND_LIMIT NS_IN_SEC

/**
	// System Clock basetype providing offset control.
*/
struct Clockwork {
	PyObject_HEAD
	long long cw_offset;
	clockid_t cw_clockid;
};
typedef struct Clockwork *Clockwork;

