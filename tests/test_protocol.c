/* SPDX-License-Identifier: MIT */
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_frame(void)
{
    uint8_t bytes[TUDOR_FRAME_HEADER_SIZE + TUDOR_FRAME_BYTES + 1] = {0};
    bytes[2] = 1; bytes[6] = 2;
    bytes[8] = (uint8_t)TUDOR_FRAME_BYTES;
    bytes[9] = (uint8_t)(TUDOR_FRAME_BYTES >> 8);
    bytes[10] = 0x55; bytes[sizeof(bytes)-2] = 0xab;
    const size_t size = sizeof(bytes)-1;
    struct tudor_frame out;
    for (size_t n = 0; n < size; n++) {
        memset(&out, 0, sizeof(out));
        assert(tudor_parse_frame(bytes, n, &out) != 0 && !out.pixels);
    }
    assert(tudor_parse_frame(bytes, size+1, &out) != 0);
    assert(tudor_parse_frame(NULL, size, &out) != 0);
    assert(tudor_parse_frame(bytes, size, NULL) != 0);
    assert(tudor_parse_frame(bytes, size, &out) == 0);
    assert(out.pixels == bytes+10 && out.pixel_bytes == TUDOR_FRAME_BYTES && out.flags == 1 && out.index == 2);
    assert(out.pixels[0] == 0x55 && out.pixels[out.pixel_bytes-1] == 0xab);
    struct tudor_frame previous = out;
    bytes[8] ^= 1;
    assert(tudor_parse_frame(bytes, size, &out) != 0 && out.pixels == previous.pixels && out.index == previous.index);
    bytes[8] ^= 1;
    bytes[2] = 3; assert(tudor_parse_frame(bytes, size, &out) != 0);
    bytes[2] = 0; assert(tudor_parse_frame(bytes, size, &out) != 0);
    bytes[2] = 1; bytes[0] = 0xcb; bytes[1] = 5;
    assert(tudor_parse_frame(bytes, size, &out) == 0x5cb);
    bytes[0] = 0xcc;
    assert(tudor_parse_frame(bytes, size, &out) == 0);
}

int main(void)
{
    test_frame();
    /* Synthetic fixture: deliberately distinctive bytes at each field. */
    uint8_t response[39] = {0};
    response[6] = 0x78; response[7] = 0x56; response[8] = 0x34; response[9] = 0x12;
    response[10] = 10; response[11] = 1; response[13] = 'A';
    response[24] = 1; response[25] = 0x20; response[37] = 0xa3;
    struct tudor_version v;
    for (size_t n = 0; n < 38; n++) assert(tudor_parse_version(response, n, &v) != 0);
    assert(tudor_parse_version(response, 39, &v) != 0);
    assert(tudor_parse_version(NULL, 38, &v) != 0);
    assert(tudor_parse_version(response, 38, NULL) != 0);
    assert(tudor_parse_version(response, 38, &v) == 0);
    assert(v.build == 0x12345678 && v.major == 10 && v.minor == 1);
    assert(v.product == 'A' && v.provision == 3 && v.advanced_security && v.key_flag);
    response[0] = 3; response[1] = 4;
    assert(tudor_parse_version(response, 38, &v) == 0x403);
    response[0] = 0x12;
    assert(tudor_parse_version(response, 38, &v) == 0);
    response[0] = 0xcc; response[1] = 5;
    assert(tudor_parse_version(response, 38, &v) == 0);

    const uint8_t good[] = {
        9,2,39,0,1,1,0,0xa0,50, 9,4,0,0,3,0xff,0,0,0,
        7,5,1,2,64,0,0, 7,5,0x81,2,64,0,0, 7,5,0x83,3,8,0,4
    };
    assert(tudor_validate_config(good, sizeof(good)) == 0);
    assert(tudor_validate_config(NULL, sizeof(good)) != 0);
    for (size_t n = 0; n < sizeof(good); n++) assert(tudor_validate_config(good, n) != 0);
    uint8_t bad[sizeof(good)];
    memcpy(bad, good, sizeof(bad)); bad[25] = 0;
    assert(tudor_validate_config(bad, sizeof(bad)) != 0);
    memcpy(bad, good, sizeof(bad)); bad[27] = 1; /* duplicate OUT endpoint */
    assert(tudor_validate_config(bad, sizeof(bad)) != 0);
    memcpy(bad, good, sizeof(bad)); bad[28] = 3; /* wrong endpoint type */
    assert(tudor_validate_config(bad, sizeof(bad)) != 0);
    memcpy(bad, good, sizeof(bad)); bad[12] = 1; /* alternate setting */
    assert(tudor_validate_config(bad, sizeof(bad)) != 0);
    puts("Protocol parser tests passed (synthetic data; no hardware involved).");
    return 0;
}
