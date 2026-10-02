/* SPDX-License-Identifier: MIT */
#include "image_bank.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int failures;
static atomic_uint creates;
struct tudor_prepared_probe *tudor_test_prepare_probe(const uint8_t *image)
{
    return failures==3 ? NULL : tudor_image_prepare_probe(image);
}
int tudor_test_pthread_create(pthread_t *thread,const pthread_attr_t *attr,
                              void *(*fn)(void *),void *arg)
{
    unsigned index=creates++;
    if (failures==1 || (failures==2 && index%2==0)) return EAGAIN;
    return pthread_create(thread,attr,fn,arg);
}

struct concurrent { const struct tudor_image_bank *bank; const uint8_t *probe;
                    struct tudor_bank_scores scores; };
static void *concurrent_compare(void *opaque)
{
    struct concurrent *c=opaque;
    assert(!tudor_image_bank_compare(c->bank,c->probe,TUDOR_IMAGE_PIXELS,&c->scores));
    return NULL;
}

int main(void)
{
    uint8_t refs[10][TUDOR_IMAGE_PIXELS],probe[TUDOR_IMAGE_PIXELS],saved[TUDOR_IMAGE_PIXELS];
    uint32_t seed=0x348932u;
    struct tudor_image_bank *bank=tudor_image_bank_create(); assert(bank);
    for (unsigned i=0;i<10;i++) {
        for (unsigned j=0;j<TUDOR_IMAGE_PIXELS;j++) {
            seed=seed*1664525u+1013904223u; refs[i][j]=(uint8_t)(seed>>24);
        }
        assert(tudor_image_bank_add(bank,refs[i],sizeof(refs[i]))==0);
    }
    memcpy(probe,refs[3],sizeof(probe)); memcpy(saved,probe,sizeof(probe));
    struct tudor_prepared_probe *prepared=tudor_image_prepare_probe(probe); assert(prepared);
    struct tudor_image_score expected[10];
    for (unsigned i=0;i<10;i++) {
        struct tudor_image_score actual;
        assert(!tudor_image_compare(refs[i],probe,&expected[i]));
        assert(!tudor_image_compare_prepared(refs[i],prepared,&actual));
        assert(!memcmp(&actual,&expected[i],sizeof(actual)));
    }
    struct tudor_image_score invalid={17,19},before=invalid;
    assert(tudor_image_compare_prepared(NULL,prepared,&invalid)==-1);
    assert(tudor_image_compare_prepared(probe,NULL,&invalid)==-1);
    assert(tudor_image_compare_prepared(probe,prepared,NULL)==-1);
    assert(!memcmp(&invalid,&before,sizeof(invalid)));
    assert(!tudor_image_prepare_probe(NULL));
    tudor_image_prepared_free(prepared); tudor_image_prepared_free(NULL);
    for (failures=0;failures<=3;failures++) {
        creates=0;
        struct tudor_bank_scores actual;
        assert(!tudor_image_bank_compare(bank,probe,sizeof(probe),&actual));
        assert(actual.valid_references==10);
        double best=-2;
        for (unsigned i=0;i<10;i++) {
            assert(actual.valid[i]);
            assert(!memcmp(&actual.scores[i],&expected[i],sizeof(expected[i])));
            if (expected[i].correlation>best) best=expected[i].correlation;
        }
        assert(actual.best_correlation==best);
        assert(!memcmp(probe,saved,sizeof(probe)));
        if (failures==3) assert(!creates);
    }
    failures=0;
    struct concurrent c[2]={{.bank=bank,.probe=probe},{.bank=bank,.probe=probe}};
    pthread_t t[2];
    for (unsigned i=0;i<2;i++) assert(!pthread_create(&t[i],NULL,concurrent_compare,&c[i]));
    for (unsigned i=0;i<2;i++) assert(!pthread_join(t[i],NULL));
    for (unsigned j=0;j<2;j++) for (unsigned i=0;i<10;i++)
        assert(!memcmp(&c[j].scores.scores[i],&expected[i],sizeof(expected[i])));
    tudor_image_bank_free(bank);
    puts("Prepared/parallel scores identical; allocation/all/partial thread failures fall back correctly; concurrent readers, inputs and failure outputs preserved.");
    return 0;
}
