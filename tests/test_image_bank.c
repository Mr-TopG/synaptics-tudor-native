/* SPDX-License-Identifier: MIT */
#include "image_bank.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct tudor_image_bank *bank=tudor_image_bank_create(); assert(bank);
    struct tudor_bank_scores out,before;
    memset(&out,0x5a,sizeof(out)); memcpy(&before,&out,sizeof(out));
    uint8_t pixels[TUDOR_IMAGE_PIXELS]={0},probe[TUDOR_IMAGE_PIXELS];
    assert(tudor_image_bank_count(bank)==0);
    assert(tudor_image_bank_add(bank,pixels,sizeof(pixels))==TUDOR_BANK_FLAT);
    assert(tudor_image_bank_add(bank,pixels,sizeof(pixels)-1)==TUDOR_BANK_INVALID);
    assert(tudor_image_bank_compare(bank,pixels,sizeof(pixels),&out)==TUDOR_BANK_INCOMPLETE);
    assert(!memcmp(&out,&before,sizeof(out)));
    uint32_t seed=173;
    for (unsigned i=0; i<TUDOR_ENROLLMENT_SCANS; i++) {
        for (size_t j=0; j<sizeof(pixels); j++) {
            seed=seed*1664525u+1013904223u; pixels[j]=(uint8_t)(seed>>24);
        }
        assert(tudor_image_bank_add(bank,pixels,sizeof(pixels))==TUDOR_BANK_OK);
        if (!i) {
            memcpy(probe,pixels,sizeof(probe));
            assert(tudor_image_bank_add(bank,pixels,sizeof(pixels))==TUDOR_BANK_DUPLICATE);
        }
        assert(tudor_image_bank_count(bank)==i+1);
        if (i<9) assert(tudor_image_bank_compare(bank,pixels,sizeof(pixels),&out)==TUDOR_BANK_INCOMPLETE);
    }
    memset(pixels,128,sizeof(pixels));
    assert(tudor_image_bank_add(bank,pixels,sizeof(pixels))==TUDOR_BANK_FULL);
    assert(tudor_image_bank_compare(bank,pixels,sizeof(pixels),&out)==TUDOR_BANK_UNUSABLE);
    assert(!memcmp(&out,&before,sizeof(out)));
    assert(tudor_image_bank_compare(bank,probe,sizeof(probe),&out)==TUDOR_BANK_OK);
    assert(out.valid_references==10 && fabs(out.best_correlation-1)<1e-9);
    assert(out.valid[0] && fabs(out.scores[0].correlation-1)<1e-9);
    assert(tudor_image_bank_count(bank)==10);
    assert(tudor_image_bank_compare(NULL,probe,sizeof(probe),&out)==TUDOR_BANK_INVALID);
    assert(tudor_image_bank_compare(bank,NULL,sizeof(probe),&out)==TUDOR_BANK_INVALID);
    assert(tudor_image_bank_compare(bank,probe,sizeof(probe),NULL)==TUDOR_BANK_INVALID);
    assert(tudor_image_bank_compare(bank,probe,sizeof(probe)-1,&out)==TUDOR_BANK_INVALID);
    assert(tudor_image_bank_add(NULL,probe,sizeof(probe))==TUDOR_BANK_INVALID);
    assert(tudor_image_bank_add(bank,NULL,sizeof(probe))==TUDOR_BANK_INVALID);
    tudor_image_bank_free(bank); tudor_image_bank_free(NULL);
    puts("Native bank controls passed: exactly ten independent images, duplicate/flat/bounds rejection, owned input copies, held-out scoring, unchanged bank and failure output, cleanup.");
    return 0;
}
