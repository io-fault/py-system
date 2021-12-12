/**
	// Mappings for signal codes, identifiers, and names.
*/
#include <signal.h>
#include <fault/posix/signal.h>
#include <fault/posix/signal-invariants.h>

static inline const char *
signal_string(int sig)
{
	switch (sig)
	{
		#define SIG(SID, CAT, SYM, ...) \
			case SIG##SID: return #CAT "/" #SYM; break;

			FAULT_SIGNAL_LIST()
		#undef SIG

		default:
			return "";
		break;
	}
}
