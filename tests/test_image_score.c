/* SPDX-License-Identifier: MIT */
#include "image_score.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t a[TUDOR_IMAGE_PIXELS],b[TUDOR_IMAGE_PIXELS],before[TUDOR_IMAGE_PIXELS];
    uint32_t state=193;
    for (int i=0; i<TUDOR_IMAGE_PIXELS; i++) {
        state=state*1664525u+1013904223u;
        a[i]=(uint8_t)(state>>24);
    }
    memcpy(before,a,sizeof(a));
    struct tudor_image_score score;
    assert(tudor_image_compare(a,a,&score)==0);
    assert(fabs(score.correlation-1)<1e-9 && score.overlap_fraction==1);
    assert(!memcmp(a,before,sizeof(a)));
    memset(b,128,sizeof(b));
    for (int y=4; y<TUDOR_IMAGE_HEIGHT; y++) for (int x=8; x<TUDOR_IMAGE_WIDTH; x++)
        b[y*TUDOR_IMAGE_WIDTH+x]=a[(y-4)*TUDOR_IMAGE_WIDTH+x-8];
    assert(tudor_image_compare(a,b,&score)==0);
    assert(score.correlation>0.95 && score.overlap_fraction>=0.6);
    memset(b,128,sizeof(b)); score=(struct tudor_image_score){17,19};
    assert(tudor_image_compare(b,b,&score)==1);
    assert(score.correlation==17 && score.overlap_fraction==19);
    assert(tudor_image_compare(NULL,b,&score)==-1);
    assert(tudor_image_compare(a,NULL,&score)==-1);
    assert(tudor_image_compare(a,b,NULL)==-1);
    puts("Native score C controls passed: exact replay, known translation, constant/NULL rejection, unchanged inputs and failure output.");
    return 0;
}
