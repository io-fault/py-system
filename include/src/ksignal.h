/**
	// Normally, this header would contain a single macro providing the list.
	// However, to manage signals like SIGINFO, it's desirable to conditionally
	// express them.
*/
#ifdef SIGCONT
	SIGNAL(SIGCONT, "process/continue")
#endif

#ifdef SIGTERM
	SIGNAL(SIGTERM, "process/terminate")
#endif

#ifdef SIGINT
	SIGNAL(SIGINT, "process/interrupt")
#endif

#ifdef SIGQUIT
	SIGNAL(SIGQUIT, "process/quit")
#endif

#ifdef SIGABRT
	SIGNAL(SIGABRT, "process/abort")
#endif

#ifdef SIGSTOP
	SIGNAL(SIGSTOP, "process/stop")
#endif

#ifdef SIGTRAP
	SIGNAL(SIGTRAP, "process/trap")
#endif

#ifdef SIGKILL
	SIGNAL(SIGKILL, "process/kill")
#endif

#ifdef SIGCHLD
	SIGNAL(SIGCHLD, "event/child-process-delta")
#endif

#ifdef SIGURG
	SIGNAL(SIGURG, "event/urgent-condition")
#endif

#ifdef SIGIO
	SIGNAL(SIGIO, "event/io")
#endif

#ifdef SIGTSTP
	SIGNAL(SIGTSTP, "terminal/stop")
#endif

#ifdef SIGHUP
	SIGNAL(SIGHUP, "terminal/closed")
#endif

#ifdef SIGINFO
	SIGNAL(SIGINFO, "terminal/query")
#endif

#ifdef SIGWINCH
	SIGNAL(SIGWINCH, "terminal/delta")
#endif

#ifdef SIGTTIN
	SIGNAL(SIGTTIN, "terminal/background-read")
#endif

#ifdef SIGTTOU
	SIGNAL(SIGTTOU, "terminal/background-write")
#endif

#ifdef SIGUSR1
	SIGNAL(SIGUSR1, "user/1")
#endif

#ifdef SIGUSR2
	SIGNAL(SIGUSR2, "user/2")
#endif

#ifdef SIGXCPU
	SIGNAL(SIGXCPU, "limit/cpu")
#endif

#ifdef SIGXFSZ
	SIGNAL(SIGXFSZ, "limit/file")
#endif

#ifdef SIGVTALRM
	SIGNAL(SIGVTALRM, "limit/time")
#endif

#ifdef SIGPROF
	SIGNAL(SIGPROF, "limit/profiling")
#endif

#ifdef SIGFPE
	SIGNAL(SIGFPE, "exception/floating-point")
#endif

#ifdef SIGPIPE
	SIGNAL(SIGPIPE, "exception/broken-pipe")
#endif

#ifdef SIGILL
	SIGNAL(SIGILL, "error/illegal-instruction")
#endif

#ifdef SIGBUS
	SIGNAL(SIGBUS, "error/bus")
#endif

#ifdef SIGSEGV
	SIGNAL(SIGSEGV, "error/segmentation-violation")
#endif

#ifdef SIGSYS
	SIGNAL(SIGSYS, "error/invalid-system-call")
#endif
