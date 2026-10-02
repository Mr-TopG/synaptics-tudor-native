/* SPDX-License-Identifier: MIT */
/* Run the real session CLI against a Python peer over framed stdin/stdout. */
#define _POSIX_C_SOURCE 200809L
#define main session_main
#ifdef TUDOR_CAPTURE_TEST
#include "../src/capture.c"
#else
#include "../src/session.c"
#endif
#undef main
#include <errno.h>
#include <unistd.h>

static int wire_fd;
#ifdef TUDOR_CAPTURE_TEST
int tudor_device_interrupt(int fd, uint8_t response[8], unsigned timeout_ms)
{
    (void)fd; (void)timeout_ms;
    const char *mode = getenv("TUDOR_TEST_INTERRUPT");
    if (mode && !strcmp(mode, "cancel")) { raise(SIGINT); errno = EINTR; return -1; }
    if (mode && !strcmp(mode, "error")) { errno = EIO; return -1; }
    memset(response, 0, 8); response[5] = 1; return 8;
}
#endif
static int transfer_all(int fd, void *buffer, size_t size, int writing)
{
    uint8_t *p = buffer;
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

int tudor_exchange(int fd, const uint8_t *request, size_t size, uint8_t *response, size_t capacity)
{
    (void)fd;
    if (size > 65535) return -1;
    uint8_t header[] = {0,0,(uint8_t)(size >> 8),(uint8_t)size};
    if (transfer_all(wire_fd, header, 4, 1) || transfer_all(wire_fd, (void *)request, size, 1) ||
        transfer_all(0, header, 4, 0)) return -1;
    size_t incoming = (size_t)header[0] << 24 | (size_t)header[1] << 16 | (size_t)header[2] << 8 | header[3];
    if (incoming > capacity || transfer_all(0, response, incoming, 0)) return -1;
    return (int)incoming;
}

int tudor_device_run(tudor_device_action action, void *context)
{
    struct tudor_version version = {.major=10,.minor=1,.build=3077709,.product=65,
        .provision=3,.advanced_security=1,.key_flag=1};
    return action(42, &version, context);
}

int tudor_device_tls_status(int fd)
{
    (void)fd;
    static unsigned calls;
#ifdef TUDOR_CAPTURE_TEST
    return calls++ == 1 ? 1 : 0;
#else
    return calls++ == 0 ? 1 : 0;
#endif
}

int main(int argc, char **argv)
{
    wire_fd = dup(1);
    if (wire_fd < 0 || dup2(2, 1) < 0) return 1;
    int result = session_main(argc, argv);
    close(wire_fd);
    return result;
}
