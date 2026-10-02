/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_NATIVE_PROTOCOL_H
#define TUDOR_NATIVE_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>

#define TUDOR_VID 0x06cb
#define TUDOR_PID 0x00be
#define TUDOR_VERSION_SIZE 38
#define TUDOR_FRAME_WIDTH 104
#define TUDOR_FRAME_HEIGHT 86
#define TUDOR_FRAME_BYTES (TUDOR_FRAME_WIDTH * TUDOR_FRAME_HEIGHT * 2)
#define TUDOR_FRAME_HEADER_SIZE 10

struct tudor_frame {
    const uint8_t *pixels;
    size_t pixel_bytes;
    uint16_t flags, index;
};
/* Observed 00be/10.1 layout. Validates framing and completion, not image quality.
 * On failure, does not change *out or expose a pixel pointer. */
int tudor_parse_frame(const uint8_t *data, size_t size, struct tudor_frame *out);

struct tudor_version {
    uint32_t build;
    uint8_t major, minor, product, provision;
    int advanced_security, key_flag;
};
/* Returns 0, -1 for a malformed response, or the sensor's nonzero status. */
int tudor_status(const uint8_t *data, size_t size);
int tudor_parse_version(const uint8_t *data, size_t size, struct tudor_version *out);
int tudor_validate_config(const uint8_t *data, size_t size);
#endif
