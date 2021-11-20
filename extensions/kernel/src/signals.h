/**
	// Signals that kernel.Scheduler will listen for automatically.

	// SIGINT is handled process.control() with signal.signal.
	// SIGUSR2 is *explicitly* used to trigger interjections.

	// *All* Kernel instances will receive signals.
*/

#include <signal.h>
#include <fault/posix/signal.h>

#define SIGNAL_CONNECTIONS() \
	SIG(SIGCONT) \
	SIG(SIGHUP) \
	SIG(SIGINFO) \
	SIG(SIGUSR1) \
	SIG(SIGTERM) \
	SIG(SIGTSTP) \
	SIG(SIGWINCH) \
	SIG(SIGPIPE) \
	SIG(SIGIO) \
	SIG(SIGURG)

static inline const char *
signal_string(int sig)
{
	switch (sig)
	{
		#define SIG(CAT, SYM, SID, ...) \
			case SID: return #CAT "/" #SYM; break;

			FAULT_SIGNAL_LIST()
		#undef SIG

		default:
			return "";
		break;
	}
}
