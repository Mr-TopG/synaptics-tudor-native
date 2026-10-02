/* SPDX-License-Identifier: MIT */
#include "capture_core.h"
#include "frame_decode.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void all_zero(const void *data, size_t size)
{
    const unsigned char *p=data;
    for (size_t i=0;i<size;i++) assert(!p[i]);
}

int main(void)
{
    struct tudor_capture_result result;
    memset(&result,0xa5,sizeof(result));
    assert(tudor_capture_run(NULL,NULL,NULL,NULL,&result)==TUDOR_CAPTURE_INVALID);
    all_zero(&result,sizeof(result));
    assert(tudor_capture_run(NULL,NULL,NULL,NULL,NULL)==TUDOR_CAPTURE_INVALID);
    tudor_capture_result_clear(NULL);
    uint8_t response[TUDOR_FRAME_HEADER_SIZE+TUDOR_FRAME_BYTES]={0};
    response[2]=1; response[6]=2;
    response[8]=(uint8_t)(TUDOR_FRAME_BYTES&255);
    response[9]=(uint8_t)(TUDOR_FRAME_BYTES>>8);
    struct tudor_gray_frame image;
    memset(&image,0xa5,sizeof(image));
    assert(tudor_frame_decode(response,sizeof(response),&image)==1);
    all_zero(&image,sizeof(image));
    /* Alternate signed minimum/maximum in each column. */
    for (unsigned i=0;i<TUDOR_GRAY_BYTES;i++) {
        unsigned value=(i&1) ? 32767 : 32768;
        response[10+i*2]=(uint8_t)(value&255);
        response[11+i*2]=(uint8_t)(value>>8);
    }
    uint8_t original[sizeof(response)]; memcpy(original,response,sizeof(response));
    assert(!tudor_frame_decode(response,sizeof(response),&image));
    assert(!memcmp(original,response,sizeof(response)));
    assert(image.contrast_low==-32768 && image.contrast_high==32767);
    for (int y=0;y<86;y++) for (int x=0;x<104;x++)
        assert(image.pixels[y*104+x]==((y&1) ? 255 : 0));
    tudor_gray_frame_clear(&image); all_zero(&image,sizeof(image));
    assert(tudor_frame_decode(NULL,sizeof(response),&image)==-1);
    assert(tudor_frame_decode(response,sizeof(response),NULL)==-1);
    memset(&image,0xa5,sizeof(image));
    assert(tudor_frame_decode(response,sizeof(response)-1,&image)==-1);
    all_zero(&image,sizeof(image));
    response[2]=3; memset(&image,0xa5,sizeof(image));
    assert(tudor_frame_decode(response,sizeof(response),&image)==-1);
    all_zero(&image,sizeof(image));
    tudor_gray_frame_clear(NULL);
    puts("Capture API tests passed: null/size checks, stale-image wiping, signed extremes, exact layout and input preservation.");
    return 0;
}
