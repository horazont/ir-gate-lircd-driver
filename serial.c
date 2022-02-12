#define winsize
#define termio
#include <asm/termios.h>
#undef winsize
#undef termio
#include <sys/ioctl.h>

int set_custom_baud_rate(int fd, int rate) {
    struct termios2 cfg;
    if (ioctl(fd, TCGETS2, &cfg) != 0) {
        return -1;
    }
    cfg.c_cflag &= ~CBAUD;
    cfg.c_cflag |= BOTHER;
    cfg.c_ospeed = rate;
    cfg.c_ispeed = rate;
    if (ioctl(fd, TCSETS2, &cfg) != 0) {
        return -1;
    }
    return 0;
}
