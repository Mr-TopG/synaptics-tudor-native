/* SPDX-License-Identifier: MIT */
/* Scorer-usability equivalence against the full search; synthetic pixels only. */
#include "image_score.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned cases;
static void check(const uint8_t *image)
{
    uint8_t before[TUDOR_IMAGE_PIXELS];
    memcpy(before,image,sizeof(before));
    struct tudor_image_score ignored;
    int expected=tudor_image_compare(image,image,&ignored);
    assert(tudor_image_self_check(image)==expected);
    assert(!memcmp(before,image,sizeof(before)));
    cases++;
}

int main(void)
{
    uint8_t image[TUDOR_IMAGE_PIXELS];
    static const uint8_t constants[]={0,1,63,127,128,129,192,254,255};
    static const unsigned positions[]={0,1,103,104,105,104*42+52,104*43+53,8942,8943};
    for (unsigned i=0;i<sizeof(constants);i++) {
        memset(image,constants[i],sizeof(image)); check(image);
    }
    for (unsigned i=0;i<sizeof(positions)/sizeof(*positions);i++) {
        memset(image,128,sizeof(image)); image[positions[i]]=129; check(image);
        image[positions[i]]=255; check(image);
    }
    for (unsigned mode=0;mode<14;mode++) {
        uint32_t seed=193;
        for (unsigned y=0;y<TUDOR_IMAGE_HEIGHT;y++) for (unsigned x=0;x<TUDOR_IMAGE_WIDTH;x++) {
            seed=seed*1664525u+1013904223u;
            unsigned at=y*TUDOR_IMAGE_WIDTH+x;
            switch (mode) {
            case 0: image[at]=(uint8_t)(128+((x+y)&1u)); break;
            case 1: image[at]=(uint8_t)(255*((x+y)&1u)); break;
            case 2: image[at]=(uint8_t)(x*2); break;
            case 3: image[at]=(uint8_t)(y*2); break;
            case 4: image[at]=(uint8_t)(x==52 ? 255:128); break;
            case 5: image[at]=(uint8_t)(y==43 ? 255:128); break;
            case 6: image[at]=(uint8_t)(128+60*sin(.55*x+.006*y*y)); break;
            case 7: image[at]=(uint8_t)(128+sin(.55*x+.006*y*y)); break;
            case 8: image[at]=(uint8_t)(128+60*sin(.7*x)); break;
            case 9: image[at]=(uint8_t)(128+60*sin(.7*y)); break;
            case 10: image[at]=(uint8_t)(seed>>24); break;
            case 11: image[at]=(uint8_t)(128+((seed>>24)&1u)); break;
            case 12: image[at]=(uint8_t)((seed>>24)<4 ? 255:128); break;
            default: image[at]=(uint8_t)(x<4 && y<4 ? 255:128); break;
            }
        }
        check(image);
    }
    assert(tudor_image_self_check(NULL)==-1);
    printf("Self-check matches full-search acceptance for %u constant, sparse, aliasing, periodic and ridge cases; inputs unchanged.\n",cases);
    return 0;
}
