/* SPDX-License-Identifier: MIT */
/* Compile the real probe against a fake kernel USB interface. Never opens USB. */
#define ioctl mock_ioctl
#define main unused_probe_main
#include "../src/probe.c"
#undef main
#undef ioctl
#include <assert.h>
#include <stdarg.h>

enum scenario { OK, WRONG_DEVICE, WRONG_LAYOUT, INACTIVE_CONFIG,
                BUSY, TLS_SHORT, TLS_ACTIVE, SHORT_VERSION, SENSOR_ERROR };
static enum scenario scenario;
static unsigned claims, releases, writes, reads;

int mock_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 42);
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == USBDEVFS_CONTROL) {
        struct usbdevfs_ctrltransfer *t = arg;
        assert(t->wIndex == 0 && t->timeout == 2000);
        uint8_t *p = t->data;
        memset(p, 0, t->wLength);
        if (t->bRequestType == 0x80 && t->bRequest == 6 && t->wValue == 0x100) {
            assert(t->wLength == 18);
            p[0] = 18; p[1] = 1; p[8] = 0xcb; p[9] = 6; p[10] = 0xbe;
            if (scenario == WRONG_DEVICE) p[10] = 0xe7;
            return 18;
        }
        if (t->bRequestType == 0x80 && t->bRequest == 6 && t->wValue == 0x200) {
            const uint8_t config[] = {
                9,2,39,0,1,1,0,0xa0,50, 9,4,0,0,3,0xff,0,0,0,
                7,5,1,2,64,0,0, 7,5,0x81,2,64,0,0, 7,5,0x83,3,8,0,4
            };
            assert(t->wLength >= sizeof(config));
            memcpy(p, config, sizeof(config));
            if (scenario == WRONG_LAYOUT) p[20] = 2;
            return (int)sizeof(config);
        }
        if (t->bRequestType == 0x80 && t->bRequest == 8 && t->wValue == 0) {
            assert(t->wLength == 1);
            p[0] = scenario == INACTIVE_CONFIG ? 0 : 1;
            return 1;
        }
        if (t->bRequestType == 0xc0 && t->bRequest == 0x14 && t->wValue == 0) {
            assert(t->wLength == 2 && claims == 1 && releases == 0);
            p[0] = scenario == TLS_ACTIVE ? 1 : 0;
            return scenario == TLS_SHORT ? 1 : 2;
        }
        assert(!"Unexpected control command");
    } else if (request == USBDEVFS_CLAIMINTERFACE) {
        assert(*(unsigned *)arg == 0);
        claims++;
        if (scenario == BUSY) { errno = EBUSY; return -1; }
        return 0;
    } else if (request == USBDEVFS_RELEASEINTERFACE) {
        assert(*(unsigned *)arg == 0 && claims == 1 && releases == 0);
        releases++;
        return 0;
    } else if (request == USBDEVFS_BULK) {
        struct usbdevfs_bulktransfer *t = arg;
        assert(t->timeout == 2000 && claims == 1 && releases == 0);
        if (t->ep == 1) {
            assert(t->len == 1 && *(uint8_t *)t->data == 1 && writes == 0);
            writes++;
            return 1;
        }
        assert(t->ep == 0x81 && t->len >= 38 && writes == 1 && reads == 0);
        reads++;
        uint8_t *p = t->data;
        memset(p, 0, 38);
        p[10] = 10; p[11] = 1; p[13] = 'A'; p[37] = 3;
        if (scenario == SENSOR_ERROR) { p[0] = 3; p[1] = 4; }
        return scenario == SHORT_VERSION ? 2 : 38;
    } else assert(!"Unexpected ioctl (including any reset or configuration write)");
    return -1;
}

int main(void)
{
    /* Capture expected diagnostics and JSON, keeping normal check output concise. */
    FILE *capture = tmpfile();
    assert(capture);
    int saved_out = dup(STDOUT_FILENO), saved_err = dup(STDERR_FILENO);
    assert(saved_out >= 0 && saved_err >= 0);
    assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    assert(dup2(fileno(capture), STDERR_FILENO) >= 0);
    for (scenario = OK; scenario <= SENSOR_ERROR; scenario++) {
        claims = releases = writes = reads = 0;
        int result = device_action(42, print_version, NULL);
        assert((result == 0) == (scenario == OK));
        if (scenario == WRONG_DEVICE || scenario == WRONG_LAYOUT || scenario == INACTIVE_CONFIG)
            assert(claims == 0 && releases == 0 && writes == 0);
        else if (scenario == BUSY)
            assert(claims == 1 && releases == 0 && writes == 0);
        else if (scenario == TLS_SHORT || scenario == TLS_ACTIVE)
            assert(claims == 1 && releases == 1 && writes == 0);
        else assert(claims == 1 && releases == 1 && writes == 1 && reads == 1);
    }
    fflush(stdout); fflush(stderr);
    assert(dup2(saved_out, STDOUT_FILENO) >= 0);
    assert(dup2(saved_err, STDERR_FILENO) >= 0);
    close(saved_out); close(saved_err); fclose(capture);
    puts("Probe transaction tests passed (9 simulated USB scenarios; no hardware involved).");
    return 0;
}
