/* SPDX-License-Identifier: MIT */
#include "protocol.h"

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int tudor_status(const uint8_t *data, size_t size)
{
    if (!data || size < 2) return -1;
    int status = le16(data);
    return status == 0x412 || status == 0x5cc ? 0 : status;
}

int tudor_parse_frame(const uint8_t *data, size_t size, struct tudor_frame *out)
{
    if (!data || !out || size != TUDOR_FRAME_HEADER_SIZE + TUDOR_FRAME_BYTES) return -1;
    int status = tudor_status(data, size);
    if (status) return status;
    uint16_t flags = le16(data + 2);
    if (le16(data + 8) != TUDOR_FRAME_BYTES || !(flags & 1) || (flags & 2)) return -1;
    *out = (struct tudor_frame){.pixels=data + TUDOR_FRAME_HEADER_SIZE,
        .pixel_bytes=TUDOR_FRAME_BYTES, .flags=flags, .index=le16(data + 6)};
    return 0;
}

int tudor_parse_version(const uint8_t *data, size_t size, struct tudor_version *out)
{
    int status = tudor_status(data, size);
    if (status) return status;
    if (!out || size != TUDOR_VERSION_SIZE) return -1;
    *out = (struct tudor_version){
        .build = le32(data + 6), .major = data[10], .minor = data[11],
        .product = data[13], .provision = data[37] & 0x0f,
        .advanced_security = !!(data[24] & 1), .key_flag = !!(data[25] & 0x20)
    };
    return 0;
}

/* Only accept the observed interface/alternate-setting/endpoint layout.
 * A different layout needs investigation, never blind writes to endpoint 1. */
int tudor_validate_config(const uint8_t *data, size_t size)
{
    if (!data || size < 9 || data[0] != 9 || data[1] != 2 ||
        le16(data + 2) != size || data[4] != 1 || data[5] != 1) return -1;
    unsigned endpoints = 0, interfaces = 0;
    for (size_t off = 9; off < size;) {
        if (size - off < 2 || data[off] < 2 || data[off] > size - off) return -1;
        const uint8_t *p = data + off;
        if (p[1] == 4) {
            if (p[0] != 9 || ++interfaces != 1 || p[2] != 0 ||
                p[3] != 0 || p[4] != 3 || p[5] != 0xff) return -1;
        } else if (p[1] == 5) {
            if (p[0] != 7 || interfaces != 1) return -1;
            unsigned bit;
            if (p[2] == 0x01 && p[3] == 2 && le16(p + 4) == 64) bit = 1;
            else if (p[2] == 0x81 && p[3] == 2 && le16(p + 4) == 64) bit = 2;
            else if (p[2] == 0x83 && p[3] == 3 && le16(p + 4) == 8) bit = 4;
            else return -1;
            if (endpoints & bit) return -1;
            endpoints |= bit;
        } else return -1;
        off += p[0];
    }
    return interfaces == 1 && endpoints == 7 ? 0 : -1;
}
