/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "usb.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int read_number(const char *device, const char *file, unsigned *out, int hex)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/%s", device, file);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int rc = fscanf(f, hex ? "%x" : "%u", out);
    fclose(f);
    return rc == 1 ? 0 : -1;
}

static int find_device(char *path, size_t size)
{
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (!dir) { perror("USB sysfs"); return -1; }
    struct dirent *entry;
    unsigned count = 0;
    while ((entry = readdir(dir))) {
        unsigned vid, pid, bus, dev;
        if (read_number(entry->d_name, "idVendor", &vid, 1) ||
            read_number(entry->d_name, "idProduct", &pid, 1) ||
            vid != TUDOR_VID || pid != TUDOR_PID) continue;
        if (read_number(entry->d_name, "busnum", &bus, 0) ||
            read_number(entry->d_name, "devnum", &dev, 0)) continue;
        if (!bus || bus > 999 || !dev || dev > 127) continue;
        int n = snprintf(path, size, "/dev/bus/usb/%03u/%03u", bus, dev);
        if (n < 0 || (size_t)n >= size) { closedir(dir); return -1; }
        count++;
    }
    closedir(dir);
    if (count != 1) {
        fprintf(stderr, "Expected one 06cb:00be sensor; found %u.\n", count);
        return -1;
    }
    return 0;
}

static int control_in(int fd, unsigned char type, unsigned char request,
                      unsigned short value, void *data, unsigned short size)
{
    struct usbdevfs_ctrltransfer transfer = {
        .bRequestType = type, .bRequest = request, .wValue = value,
        .wIndex = 0, .wLength = size, .timeout = 2000, .data = data
    };
    return ioctl(fd, USBDEVFS_CONTROL, &transfer);
}

static int bulk(int fd, unsigned ep, void *data, unsigned size)
{
    struct usbdevfs_bulktransfer transfer = {
        .ep = ep, .len = size, .timeout = 2000, .data = data
    };
    return ioctl(fd, USBDEVFS_BULK, &transfer);
}

int tudor_device_tls_status(int fd)
{
    uint8_t status[2];
    int n = control_in(fd, 0xc0, 0x14, 0, status, sizeof(status));
    return n == 2 ? !!status[0] : -1;
}

int tudor_device_interrupt(int fd, uint8_t response[8], unsigned timeout_ms)
{
    if (!response || !timeout_ms || timeout_ms > 2000) { errno = EINVAL; return -1; }
    /* Linux usbfs routes USBDEVFS_BULK to an interrupt URB for this endpoint
     * type (drivers/usb/core/devio.c:do_proc_bulk). No asynchronous buffer lifetime. */
    struct usbdevfs_bulktransfer transfer = {.ep = 0x83, .len = 8,
        .timeout = timeout_ms, .data = response};
    return ioctl(fd, USBDEVFS_BULK, &transfer);
}

int tudor_exchange(int fd, const uint8_t *request, size_t request_size,
                   uint8_t *response, size_t capacity)
{
    if (!request || !response || !request_size || request_size > 4096 ||
        !capacity || capacity > 65536) { errno = EINVAL; return -1; }
    int n = bulk(fd, 0x01, (void *)request, (unsigned)request_size);
    if (n < 0) return -1;
    if ((size_t)n != request_size) { errno = EIO; return -1; }
    return bulk(fd, 0x81, response, (unsigned)capacity);
}

static int device_action(int fd, tudor_device_action action, void *context)
{
    uint8_t descriptor[18], config[256], active;
    int n = control_in(fd, 0x80, 6, 0x0100, descriptor, sizeof(descriptor));
    if (n != 18 || descriptor[0] != 18 || descriptor[1] != 1 ||
        descriptor[8] != 0xcb || descriptor[9] != 0x06 ||
        descriptor[10] != 0xbe || descriptor[11] != 0x00) {
        fprintf(stderr, "Device identity could not be verified on the open USB handle.\n");
        return 1;
    }
    n = control_in(fd, 0x80, 6, 0x0200, config, sizeof(config));
    if (n < 0 || tudor_validate_config(config, (size_t)n)) {
        fprintf(stderr, "Unexpected USB interface layout; refusing protocol commands.\n");
        return 1;
    }
    if (control_in(fd, 0x80, 8, 0, &active, 1) != 1 || active != 1) {
        fprintf(stderr, "Sensor configuration is not active; no configuration change attempted.\n");
        return 1;
    }
    unsigned interface = 0;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &interface) < 0) {
        perror("Claim interface 0 (close any other fingerprint application)");
        return 1;
    }
    int result = 1;
    uint8_t tls[2];
    n = control_in(fd, 0xc0, 0x14, 0, tls, sizeof(tls));
    if (n != 2) {
        fprintf(stderr, "Cannot read TLS state (%d); stopping before bulk commands.\n", n);
        goto done;
    }
    if (tls[0] != 0) {
        fprintf(stderr, "Sensor already has a TLS session. No reset or takeover attempted.\n");
        goto done;
    }
    uint8_t request = 0x01, response[256];
    n = bulk(fd, 0x01, &request, 1);
    if (n != 1) { perror("GET_VERSION send"); goto done; }
    n = bulk(fd, 0x81, response, sizeof(response));
    if (n < 0) { perror("GET_VERSION receive"); goto done; }
    struct tudor_version version;
    int status = tudor_parse_version(response, (size_t)n, &version);
    if (status != 0) {
        fprintf(stderr, "GET_VERSION rejected or malformed: result=%d, length=%d\n", status, n);
        goto done;
    }
    result = action(fd, &version, context);
done:
    if (ioctl(fd, USBDEVFS_RELEASEINTERFACE, &interface) < 0) {
        perror("Release interface");
        result = 1;
    }
    return result;
}

int tudor_device_run(tudor_device_action action, void *context)
{
    char path[64];
    if (!action || find_device(path, sizeof(path))) return 1;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        perror(path);
        fprintf(stderr, "Run from a terminal with host USB access (normally sudo).\n");
        return 1;
    }
    int result = device_action(fd, action, context);
    close(fd);
    return result;
}

#ifndef TUDOR_NO_MAIN
static int print_version(int fd, const struct tudor_version *v, void *context)
{
    (void)fd; (void)context;
    struct tudor_version version = *v;
    /* Deliberately omit bytes 18..23 (device identifier) and the raw response. */
    printf("{\n  \"usb_id\": \"06cb:00be\",\n"
           "  \"firmware\": \"%u.%u.%u\",\n  \"product_id\": %u,\n"
           "  \"provision_state\": %u,\n  \"advanced_security\": %s,\n"
           "  \"key_flag\": %s,\n  \"tls_session\": false,\n"
           "  \"authentication_supported\": false\n}\n",
           version.major, version.minor, version.build, version.product,
           version.provision, version.advanced_security ? "true" : "false",
           version.key_flag ? "true" : "false");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "detect") && strcmp(argv[1], "probe"))) {
        fprintf(stderr, "Usage: %s detect|probe\n"
                "detect: identify 06cb:00be via sysfs, without root\n"
                "probe: read TLS status and firmware version, requires USB access\n", argv[0]);
        return 2;
    }
    char path[64];
    if (find_device(path, sizeof(path))) return 1;
    if (!strcmp(argv[1], "detect")) {
        printf("Synaptics Tudor 06cb:00be at %s\n", path);
        return 0;
    }
    return tudor_device_run(print_version, NULL);
}
#endif
