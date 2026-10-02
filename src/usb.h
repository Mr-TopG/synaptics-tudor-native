/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_USB_H
#define TUDOR_USB_H
#include "protocol.h"
typedef int (*tudor_device_action)(int fd, const struct tudor_version *, void *context);
/* Action runs with interface claimed, identity/layout/version checked, TLS idle. */
int tudor_device_run(tudor_device_action action, void *context);
/* Returns 0 idle, 1 established, -1 on short/failed vendor status read. */
int tudor_device_tls_status(int fd);
int tudor_device_interrupt(int fd, uint8_t response[8], unsigned timeout_ms);
int tudor_exchange(int fd, const uint8_t *request, size_t request_size,
                   uint8_t *response, size_t capacity);
#endif
