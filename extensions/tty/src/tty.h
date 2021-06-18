/**
	// tty device interfaces
*/
#if !defined(__linux__)
	#include <sys/ttycom.h>
#endif
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

#ifndef SYSTEM_TTY_DEVICE_PATH
	#define SYSTEM_TTY_DEVICE_PATH "/dev/tty"
#endif

struct Device {
	PyObject_HEAD

	int dev_fd;
	struct termios dev_ts;
};
typedef struct Device *Device;
