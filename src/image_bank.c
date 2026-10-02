/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "image_bank.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef TUDOR_MATCH_MAX_WORKERS
#define TUDOR_MATCH_MAX_WORKERS 4
#endif
#if TUDOR_MATCH_MAX_WORKERS < 1 || TUDOR_MATCH_MAX_WORKERS > 4
#error TUDOR_MATCH_MAX_WORKERS must be between 1 and 4
#endif
#ifdef TUDOR_BANK_TESTING
extern int tudor_test_pthread_create(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);
extern struct tudor_prepared_probe *tudor_test_prepare_probe(const uint8_t *);
#define create_worker tudor_test_pthread_create
#define prepare_probe tudor_test_prepare_probe
#else
#define create_worker pthread_create
#define prepare_probe tudor_image_prepare_probe
#endif

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

struct comparison_work {
    const struct tudor_image_bank *bank;
    const struct tudor_prepared_probe *prepared;
    const uint8_t *probe;
    struct tudor_bank_scores *result;
    unsigned lane, lanes;
};

static void *compare_lane(void *opaque)
{
    struct comparison_work *w=opaque;
    for (unsigned i=w->lane;i<TUDOR_ENROLLMENT_SCANS;i+=w->lanes) {
        int status=w->prepared ?
            tudor_image_compare_prepared(w->bank->images[i],w->prepared,&w->result->scores[i]) :
            tudor_image_compare(w->bank->images[i],w->probe,&w->result->scores[i]);
        if (!status) w->result->valid[i]=1;
    }
    return NULL;
}

int tudor_image_bank_compare(const struct tudor_image_bank *bank, const uint8_t *probe,
                            size_t size, struct tudor_bank_scores *output)
{
    if (!bank || !probe || !output || size!=TUDOR_IMAGE_PIXELS) return TUDOR_BANK_INVALID;
    if (bank->count!=TUDOR_ENROLLMENT_SCANS) return TUDOR_BANK_INCOMPLETE;
    struct tudor_bank_scores result={.best_correlation=-2};
    struct tudor_prepared_probe *prepared=prepare_probe(probe);
    long cpus=sysconf(_SC_NPROCESSORS_ONLN);
    unsigned workers=cpus>0 && cpus<TUDOR_MATCH_MAX_WORKERS ? (unsigned)cpus : TUDOR_MATCH_MAX_WORKERS;
    if (!prepared || cpus<1) workers=1;
    struct comparison_work work[TUDOR_MATCH_MAX_WORKERS];
    pthread_t threads[TUDOR_MATCH_MAX_WORKERS];
    unsigned char started[TUDOR_MATCH_MAX_WORKERS]={0};
    for (unsigned i=0;i<workers;i++) {
        work[i]=(struct comparison_work){bank,prepared,probe,&result,i,workers};
        if (i && !create_worker(&threads[i],NULL,compare_lane,&work[i])) started[i]=1;
    }
    /* Disjoint score slots; reduction stays in reference order after joining.
     * Allocation/thread resource failures use the same complete serial search. */
    for (unsigned i=0;i<workers;i++) if (!started[i]) compare_lane(&work[i]);
    for (unsigned i=1;i<workers;i++) if (started[i]) {
        /* Valid joinable threads owned by this call cannot normally fail to
         * join. Never return/free shared image memory with a reader running. */
        if (pthread_join(threads[i],NULL)) abort();
    }
    tudor_image_prepared_free(prepared);
    for (unsigned i=0; i<TUDOR_ENROLLMENT_SCANS; i++) {
        if (!result.valid[i]) continue;
        result.valid_references++;
        if (result.scores[i].correlation>result.best_correlation)
            result.best_correlation=result.scores[i].correlation;
    }
    if (!result.valid_references) return TUDOR_BANK_UNUSABLE;
    *output=result;
    return TUDOR_BANK_OK;
}
