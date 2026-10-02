/* SPDX-License-Identifier: MIT */
#include "frame_decode.h"
#include <stdlib.h>

static void wipe(void *buffer, size_t size)
{
    volatile unsigned char *p=buffer;
    while (size--) *p++=0;
}

void tudor_gray_frame_clear(struct tudor_gray_frame *frame)
{
    if (frame) wipe(frame,sizeof(*frame));
}

static int signed_sample(const uint8_t *data)
{
    unsigned raw=(unsigned)data[0] | (unsigned)data[1]<<8;
    return raw>=32768 ? (int)raw-65536 : (int)raw;
}

int tudor_frame_decode(const uint8_t *response, size_t size, struct tudor_gray_frame *output)
{
    if (!output) return -1;
    tudor_gray_frame_clear(output);
    struct tudor_frame parsed;
    if (!response || tudor_parse_frame(response,size,&parsed)) return -1;
    unsigned *histogram=calloc(65536,sizeof(*histogram));
    if (!histogram) return -1;
    for (size_t i=0;i<TUDOR_GRAY_BYTES;i++) histogram[signed_sample(parsed.pixels+i*2)+32768]++;
    unsigned cumulative=0;
    int low=0, high=65535;
    for (int i=0;i<65536;i++) {
        cumulative+=histogram[i];
        if (cumulative>TUDOR_GRAY_BYTES/100) { low=i; break; }
    }
    cumulative=0;
    for (int i=65535;i>=0;i--) {
        cumulative+=histogram[i];
        if (cumulative>TUDOR_GRAY_BYTES/100) { high=i; break; }
    }
    wipe(histogram,65536*sizeof(*histogram)); free(histogram);
    if (high<=low) return 1;
    low-=32768; high-=32768;
    for (int y=0;y<TUDOR_FRAME_HEIGHT;y++) for (int x=0;x<TUDOR_FRAME_WIDTH;x++) {
        int value=signed_sample(parsed.pixels+(x*TUDOR_FRAME_HEIGHT+y)*2);
        int gray=value<=low ? 0 : value>=high ? 255 : (value-low)*255/(high-low);
        output->pixels[y*TUDOR_FRAME_WIDTH+x]=(uint8_t)gray;
    }
    output->contrast_low=low; output->contrast_high=high;
    return 0;
}
