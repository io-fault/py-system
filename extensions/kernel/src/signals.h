/**
	// Signals that kernel.Events will listen for automatically.

	// SIGINT is handled process.control() with signal.signal.
	// SIGUSR2 is *explicitly* used to trigger interjections.

	// *All* Kernel instances will receive signals.
*/

/**
	// List of signals that are available for application connections.
	// SIGINFO, SIGTERM, SIGTSTP have default actions.
	// The others are listened for, but discarded by default.
*/
#define SIGNEVER 0

#define SIGNALS() \
	SIGNAME(SIGNEVER) \
	SIGNAME(SIGINFO) \
	SIGNAME(SIGTERM) \
	SIGNAME(SIGTSTP) \
	SIGNAME(SIGCONT) \
	SIGNAME(SIGWINCH) \
	SIGNAME(SIGPIPE) \
	SIGNAME(SIGIO) \
	SIGNAME(SIGHUP) \
	SIGNAME(SIGUSR1) \
	SIGNAME(SIGURG)

static inline const char *
signal_string(int sig)
{
	switch (sig)
	{
		#define SIGNAL(SID, SYM, ...) case SID: return SYM; break;
			#include <ksignal.h>
		#undef SIGNAL

		default:
			return "";
		break;
	}
}
