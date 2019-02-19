/**
	# tty device interfaces
*/
#include <sys/ttycom.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>

struct Device {
	PyObject_HEAD

	int dev_fd;
	struct termios dev_ts;
};
typedef struct Device *Device;
