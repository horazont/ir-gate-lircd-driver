#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "lirc_driver.h"

int set_custom_baud_rate(int fd, int rate);

static int irgatedrv_init() {
    struct stat s;
    if (stat(drv.device, &s) < 0) {
        logprintf(LIRC_ERROR, "failed to stat() device %s", drv.device);
        return 0;
    }

    if (!S_ISCHR(s.st_mode)) {
        logprintf(LIRC_ERROR, "device %s is not a character device", drv.device);
        return 0;
    }

    int fd = open(drv.device, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        logprintf(LIRC_ERROR, "failed to open device %s", drv.device);
        return 0;
    }

    struct termios cfg;
    if (tcgetattr(fd, &cfg) != 0) {
        logprintf(LIRC_ERROR, "failed to read current serial config");
        goto fail;
    }
    cfmakeraw(&cfg);
    cfg.c_cflag |= CREAD;
    cfg.c_iflag |= IGNBRK | IGNPAR;
    if (tcsetattr(fd, TCSANOW, &cfg) != 0) {
        logprintf(LIRC_ERROR, "failed to write current serial config");
        goto fail;
    }


    if (set_custom_baud_rate(fd, 100000) != 0) {
        logprintf(LIRC_ERROR, "failed to set custom baud rate");
        goto fail;
    }

    drv.fd = fd;
    return 1;

fail:
    close(fd);
    return 0;
}


static lirc_t irgatedrv_readdata(lirc_t timeout) {
    static int accum = 0;
    static int accum_is_pause = -1;

    // we have to loop here until we get something definitive.
    // luckily, we'll get a 0x00 at the end of the pause after the last pulse
    // we will use that to inject a fake 100ms pause, because lircd will have opinions there, I'm afraid.
    // I mean, it's not clear. The code is very difficult to follow.

    const int poll_timeout = (timeout == 0) ? -1 : (timeout / 1000);

    struct pollfd fd;
    fd.fd = drv.fd;
    fd.events = POLLIN | POLLERR | POLLHUP;
    uint8_t buf = 0;
    while (1) {
        if (read(drv.fd, &buf, 1) != 1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return 0;
            }
        } else {
            const uint8_t is_pause = buf & 0x1;
            int duration_us = (int)(buf & 0xfe) * 8;
            if (is_pause != accum_is_pause) {
                int old_accum = accum;
                int old_accum_is_pause = accum_is_pause;
                accum = duration_us;
                accum_is_pause = is_pause;

                if (old_accum_is_pause != -1) {
                    if (old_accum_is_pause == 0 && duration_us == 0 && old_accum > 10000) {
                        // special magic case: this might be the end-of-symbol strobe, so we fudge the duration to be a true end of symbol.
                        old_accum = 100000;
                    }
                    lirc_t result = old_accum;
                    if (result == 0) {
                        result += 1;
                    }
                    if (old_accum_is_pause == 0) {
                        return LIRC_SPACE(result);
                    } else {
                        return LIRC_PULSE(result);
                    }
                }
            } else {
                accum += duration_us;
            }
            continue;
        }

        // XXX: this is grossly incorrect (timeout-wise) if we have to do multiple reads, but eh.
        int poll_result;
        while ((poll_result = poll(&fd, 1, poll_timeout)) < 1) {
            if (poll_result == 0) {
                // timeout
                return LIRC_MODE2_TIMEOUT;
            }
            if (errno != EINTR) {
                return 0;
            }
        }
    }
}


static const uint8_t pulse_buf[16] = {
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfe, 0xfe, 0xfe, 0xfe,
    0xfe, 0xfe, 0xfe, 0xfe
};

static const uint8_t pause_buf[16] = {
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff
};

static int write_blocking(int fd, const uint8_t *buf, size_t len) {
    size_t offset = 0;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT | POLLERR;

    while (offset < len) {
        int written = write(fd, buf + offset, len - offset);
        if (written <= 0) {
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
        } else {
            offset += written;
        }

        if (offset >= len) {
            break;
        }

        while (poll(&pfd, 1, -1) < 1) {
            if (errno != EINTR) {
                return -1;
            }
        }
    }

    return 0;
}

static int send(const uint8_t is_pause, const int duration_16us) {
    int full_slots = duration_16us / 0x80;
    const int remainder = duration_16us % 0x80;
    while (full_slots > 0) {
        const int batch = (full_slots > 16 ? 16 : full_slots);
        const uint8_t *buf = is_pause ? pause_buf : pulse_buf;
        fprintf(stderr, "send %d times %02x\n", batch, buf[0]);
        if (write_blocking(drv.fd, buf, batch) != 0) {
            return -1;
        }
        full_slots -= batch;
    }

    if (remainder > 0) {
        const uint8_t code = (remainder << 1) | (is_pause ? 1 : 0);
        fprintf(stderr, "send: 0x%02x\n", code);
        if (write_blocking(drv.fd, &code, 1) != 0) {
            return -1;
        }
    }

    return 0;
}


static lirc_t irgatedrv_send(struct ir_remote* remote,
                             struct ir_ncode* code)
{
    if (!send_buffer_put(remote, code)) {
        return 0;
    }

    const int ncodes = send_buffer_length();
    const lirc_t *coding = send_buffer_data();

    for (int i = 0; i < ncodes; ++i) {
        const lirc_t code = coding[i];
        const int duration_16us = ((code & 0xffffff) + 8) / 16;
        // every other thing is a pause, and we start with pulses
        if (send(i % 2 != 0, duration_16us) != 0) {
            perror("send()");
            return 0;
        }
    }

    return 1;
}

static char* irgatedrv_recv(struct ir_remote *remotes)
{
    if (!rec_buffer_clear()) {
        return NULL;
    }

    return decode_all(remotes);
}

static const struct driver hw_irgate = {
	.name		=	"irgatedrv",
	.device		=	"/dev/ttyAMA0",
	.features	=	0,
	.send_mode	=	LIRC_MODE_MODE2,
	.rec_mode	=	LIRC_MODE_MODE2,
	.code_length	=	0,
	.init_func	=	irgatedrv_init,
	.deinit_func	=	NULL,
	.open_func	=	default_open,
	.close_func	=	default_close,
	.send_func	=	irgatedrv_send,
	.rec_func	=	irgatedrv_recv,
	.decode_func	=	receive_decode,
	.drvctl_func	=	NULL,
	.readdata	=	irgatedrv_readdata,
    .resolution = 16,
	.api_version	=	2,
	.driver_version = 	"0.1.0",
	.info		=	"No info available"
};

const struct driver* hardwares[] = {&hw_irgate, (struct driver*) NULL};
