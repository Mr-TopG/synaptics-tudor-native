/* SPDX-License-Identifier: MIT */
#include "image_bank.h"
#include <stdlib.h>
#include <string.h>

struct tudor_image_bank {
    unsigned count;
    uint8_t images[TUDOR_ENROLLMENT_SCANS][TUDOR_IMAGE_PIXELS];
};

struct tudor_image_bank *tudor_image_bank_create(void)
{
    return calloc(1,sizeof(struct tudor_image_bank));
}

void tudor_image_bank_free(struct tudor_image_bank *bank)
{
    if (!bank) return;
    /* Volatile writes prevent dead-store elimination of sensitive cleanup. */
    volatile unsigned char *p=(volatile unsigned char *)bank;
    for (size_t i=0; i<sizeof(*bank); i++) p[i]=0;
    free(bank);
}

unsigned tudor_image_bank_count(const struct tudor_image_bank *bank)
{
    return bank ? bank->count : 0;
}

int tudor_image_bank_add(struct tudor_image_bank *bank, const uint8_t *image, size_t size)
{
    if (!bank || !image || size!=TUDOR_IMAGE_PIXELS) return TUDOR_BANK_INVALID;
    if (bank->count>=TUDOR_ENROLLMENT_SCANS) return TUDOR_BANK_FULL;
    for (unsigned i=0; i<bank->count; i++)
        if (!memcmp(bank->images[i],image,size)) return TUDOR_BANK_DUPLICATE;
    if (tudor_image_self_check(image)) return TUDOR_BANK_FLAT;
    memcpy(bank->images[bank->count],image,size);
    bank->count++;
    return TUDOR_BANK_OK;
}

int tudor_image_bank_compare(const struct tudor_image_bank *bank, const uint8_t *probe,
                            size_t size, struct tudor_bank_scores *output)
{
    if (!bank || !probe || !output || size!=TUDOR_IMAGE_PIXELS) return TUDOR_BANK_INVALID;
    if (bank->count!=TUDOR_ENROLLMENT_SCANS) return TUDOR_BANK_INCOMPLETE;
    struct tudor_bank_scores result={.best_correlation=-2};
    for (unsigned i=0; i<TUDOR_ENROLLMENT_SCANS; i++) {
        if (tudor_image_compare(bank->images[i],probe,&result.scores[i])) continue;
        result.valid[i]=1; result.valid_references++;
        if (result.scores[i].correlation>result.best_correlation)
            result.best_correlation=result.scores[i].correlation;
    }
    if (!result.valid_references) return TUDOR_BANK_UNUSABLE;
    *output=result;
    return TUDOR_BANK_OK;
}
