/* SPDX-License-Identifier: MIT
 *
 * Serial AT transport for the Rolling Wireless RW101R-GL (33f8:0301).
 *
 * Firmware 19512.0000.00.11.03.01 returns ERROR for its FCC commands through
 * the Fibocom MBIM AT service used by Lenovo's libmodemauthRW101, but accepts
 * the same commands through the modem's ttyUSB AT port.  The verified module
 * loads this file with LD_PRELOAD so the library keeps all of its original
 * challenge/response logic while these two preemptible transport symbols use
 * the ports that ModemManager already supplied to the dispatcher.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define AT_RESPONSE_SIZE 128
#define AT_TIMEOUT_MS 5000

static long long
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return ((long long)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

static int
valid_device_path(const char *path)
{
    size_t i;
    size_t length;

    if (!path || strncmp(path, "/dev/", 5) != 0)
        return 0;

    length = strlen(path);
    if (length <= 5 || length >= 32)
        return 0;

    for (i = 5; i < length; i++) {
        const char c = path[i];

        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
    }
    return 1;
}

/*
 * The vendor ABI supplies a 32-byte output buffer and expects a basename; its
 * caller prepends "/dev/" before opening the device.
 */
int
get_mbim_port(char *out)
{
    const char *path;
    size_t length;

    if (!out)
        return 0;

    path = getenv("WWAN_UNLOCK_MBIM_PORT");
    if (!valid_device_path(path))
        return 0;

    path += 5;
    length = strlen(path);
    if (length == 0 || length >= 32)
        return 0;

    memcpy(out, path, length + 1);
    return 1;
}

/*
 * Match libmodemauthRW101's send_at_of_mm ABI: response points to a 128-byte
 * zeroed buffer, success is 0, and failure is negative.  Preserve the modem's
 * raw CRLF framing because the unmodified worker parses fields at fixed offsets.
 */
int
send_at_of_mm(const char *command, char *response)
{
    const char *path = getenv("WWAN_UNLOCK_AT_PORT");
    struct termios attributes;
    struct pollfd descriptor;
    long long deadline;
    size_t command_length;
    size_t response_length = 0;
    int fd;
    int flags;

    if (!command || !response || !valid_device_path(path))
        return -1;

    response[0] = '\0';
    fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return -1;

    if (tcgetattr(fd, &attributes) == 0) {
        cfmakeraw(&attributes);
        attributes.c_cflag |= CLOCAL | CREAD;
        attributes.c_cc[VMIN] = 0;
        attributes.c_cc[VTIME] = 0;
        /* USB serial implementations may reject speed changes, so retain the
         * modem's existing speed and treat tcsetattr as best effort. */
        (void)tcsetattr(fd, TCSANOW, &attributes);
    }

    flags = fcntl(fd, F_GETFL);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    (void)tcflush(fd, TCIOFLUSH);

    command_length = strlen(command);
    if (write(fd, command, command_length) != (ssize_t)command_length ||
        write(fd, "\r", 1) != 1) {
        close(fd);
        return -1;
    }
    (void)tcdrain(fd);

    deadline = monotonic_milliseconds();
    if (deadline < 0) {
        close(fd);
        return -1;
    }
    deadline += AT_TIMEOUT_MS;

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    while (response_length + 1 < AT_RESPONSE_SIZE) {
        const long long now = monotonic_milliseconds();
        int remaining;
        int poll_result;
        ssize_t bytes_read;

        if (now < 0 || now >= deadline)
            break;
        remaining = (int)(deadline - now);
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, remaining);
        if (poll_result <= 0)
            break;
        if (!(descriptor.revents & POLLIN))
            break;

        bytes_read = read(fd, response + response_length,
                          AT_RESPONSE_SIZE - response_length - 1);
        if (bytes_read < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (bytes_read == 0)
            continue;

        response_length += (size_t)bytes_read;
        response[response_length] = '\0';
        if (strstr(response, "\r\nOK\r\n") ||
            strstr(response, "\r\nERROR\r\n"))
            break;
    }

    close(fd);
    if (!strstr(response, "\r\nOK\r\n") || strstr(response, "ERROR")) {
        fprintf(stderr, "AT command failed on %s: %s -> %s\n",
                path, command, response);
        return -1;
    }
    return 0;
}
