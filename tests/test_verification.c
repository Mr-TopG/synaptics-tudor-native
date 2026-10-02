/* SPDX-License-Identifier: MIT */
#include "verification.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void ridges(uint8_t *image, double phase)
{
    for (int y=0; y<TUDOR_IMAGE_HEIGHT; y++)
        for (int x=0; x<TUDOR_IMAGE_WIDTH; x++)
            image[y*TUDOR_IMAGE_WIDTH+x]=(uint8_t)(128+60*sin(0.55*x+0.006*y*y+phase));
}

static void test_quality(void)
{
    uint8_t image[TUDOR_IMAGE_PIXELS];
    struct tudor_image_quality q;
    assert(tudor_image_assess(NULL,sizeof(image),&q)==-1 && q.flags==TUDOR_QUALITY_INVALID);
    assert(tudor_image_assess(image,sizeof(image)-1,&q)==-1);
    assert(tudor_image_assess(image,sizeof(image),NULL)==-1);
    memset(image,128,sizeof(image));
    assert(!tudor_image_assess(image,sizeof(image),&q));
    assert(q.flags&TUDOR_QUALITY_LOW_CONTRAST);
    assert(q.flags&TUDOR_QUALITY_LOW_COVERAGE);
    assert(q.flags&TUDOR_QUALITY_LOW_STRUCTURE);
    ridges(image,0);
    assert(!tudor_image_assess(image,sizeof(image),&q) && !q.flags);
    for (int y=0; y<TUDOR_IMAGE_HEIGHT; y++)
        for (int x=0; x<TUDOR_IMAGE_WIDTH; x++)
            image[y*TUDOR_IMAGE_WIDTH+x]=(uint8_t)(128+60*sin(0.55*x));
    assert(!tudor_image_assess(image,sizeof(image),&q));
    assert(q.flags&TUDOR_QUALITY_DIRECTIONAL);
    uint32_t state=1234567;
    for (size_t i=0; i<sizeof(image); i++) {
        state=state*1664525u+1013904223u; image[i]=(uint8_t)(state>>24);
    }
    assert(!tudor_image_assess(image,sizeof(image),&q));
    assert(q.flags&TUDOR_QUALITY_LOW_STRUCTURE);
    ridges(image,0);
    memset(image,128,sizeof(image)*3/4);
    assert(!tudor_image_assess(image,sizeof(image),&q));
    assert(q.flags&TUDOR_QUALITY_LOW_COVERAGE);
    for (size_t i=0; i<sizeof(image); i++) image[i]=(i&1) ? 255 : 0;
    assert(!tudor_image_assess(image,sizeof(image),&q));
    assert(q.flags&TUDOR_QUALITY_CLIPPED);
}

static struct tudor_bank_scores scores(double best)
{
    struct tudor_bank_scores s={.valid_references=10,.best_correlation=best};
    for (unsigned i=0; i<10; i++) {
        s.valid[i]=1; s.scores[i].correlation=best; s.scores[i].overlap_fraction=0.6;
    }
    return s;
}

static void test_policy(void)
{
    struct tudor_image_quality q={.standard_deviation=30,.active_fraction=1,
        .local_coherence=0.8,.global_coherence=0.7,.clipped_fraction=0.02};
    struct tudor_verification_result r;
    const double values[]={-1,0.5,nextafter(0.5,1),nextafter(0.65,0),0.65,1};
    const int expected[]={2,2,0,0,1,1};
    for (unsigned i=0; i<6; i++) {
        struct tudor_bank_scores s=scores(values[i]);
        assert(!tudor_experimental_decide(&q,&s,&r));
        assert(r.policy_version==1 && r.decision==expected[i]);
        assert(r.retry_reason==(expected[i] ? TUDOR_RETRY_NONE : TUDOR_RETRY_UNCERTAIN));
    }
    struct tudor_bank_scores s=scores(0.9);
    /* Every error must overwrite a previous candidate match. */
#define INVALID(expr) do { r.decision=TUDOR_DECISION_CANDIDATE_MATCH; \
    assert((expr)==-1); assert(r.decision==TUDOR_DECISION_RETRY); \
    assert(r.retry_reason==TUDOR_RETRY_INVALID); } while (0)
    INVALID(tudor_experimental_decide(NULL,&s,&r));
    INVALID(tudor_experimental_decide(&q,NULL,&r));
    assert(tudor_experimental_decide(&q,&s,NULL)==-1);
    s.scores[0].correlation=NAN; INVALID(tudor_experimental_decide(&q,&s,&r));
    s=scores(0.9); s.scores[0].overlap_fraction=INFINITY;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    s=scores(0.9); s.scores[0].overlap_fraction=0.59;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    s=scores(0.9); s.best_correlation=0.95;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    s=scores(0.9); s.valid[0]=2;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    s=scores(0.9); s.valid_references=9;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    s.valid[0]=0;
    assert(!tudor_experimental_decide(&q,&s,&r));
    assert(r.decision==0 && r.retry_reason==TUDOR_RETRY_UNUSABLE);
    s=scores(0.9); q.flags=64;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    q.flags=TUDOR_QUALITY_INVALID;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    q.flags=0; q.active_fraction=NAN;
    INVALID(tudor_experimental_decide(&q,&s,&r));
    q.active_fraction=0.1;
    assert(!tudor_experimental_decide(&q,&s,&r));
    assert(r.decision==0 && r.retry_reason==TUDOR_RETRY_QUALITY);
    q.active_fraction=1; q.flags=TUDOR_QUALITY_LOW_STRUCTURE;
    assert(!tudor_experimental_decide(&q,&s,&r));
    assert(r.decision==0 && r.retry_reason==TUDOR_RETRY_QUALITY);
#undef INVALID
}

static void test_lifecycle(void)
{
    uint8_t image[TUDOR_IMAGE_PIXELS];
    struct tudor_image_quality q;
    struct tudor_verification_result r;
    struct tudor_verifier *v=tudor_verifier_create();
    assert(v && tudor_verifier_count(v)==0 && tudor_verifier_count(NULL)==0);
    memset(image,128,sizeof(image));
    assert(tudor_verifier_enroll(v,image,sizeof(image),&q)==TUDOR_BANK_UNUSABLE);
    assert(tudor_verifier_count(v)==0);
    assert(!tudor_verifier_check(v,image,sizeof(image),&r));
    assert(r.decision==0 && r.retry_reason==TUDOR_RETRY_ENROLLMENT);
    ridges(image,0);
    assert(tudor_verifier_enroll(v,image,sizeof(image)-1,&q)==TUDOR_BANK_INVALID);
    assert(!tudor_verifier_enroll(v,image,sizeof(image),&q));
    assert(tudor_verifier_enroll(v,image,sizeof(image),&q)==TUDOR_BANK_DUPLICATE);
    assert(tudor_verifier_count(v)==1);
    for (unsigned i=1; i<10; i++) {
        ridges(image,i*0.08);
        assert(!tudor_verifier_enroll(v,image,sizeof(image),&q));
    }
    assert(tudor_verifier_count(v)==10);
    assert(tudor_verifier_enroll(v,image,sizeof(image),&q)==TUDOR_BANK_FULL);
    /* Exact replay is solely a synthetic software positive control. */
    assert(!tudor_verifier_check(v,image,sizeof(image),&r));
    assert(r.decision==TUDOR_DECISION_CANDIDATE_MATCH);
    assert(tudor_verifier_check(NULL,image,sizeof(image),&r)==-1 && r.decision==0);
    assert(tudor_verifier_check(v,NULL,sizeof(image),&r)==-1 && r.decision==0);
    assert(tudor_verifier_check(v,image,0,&r)==-1 && r.decision==0);
    memset(image,128,sizeof(image));
    assert(!tudor_verifier_check(v,image,sizeof(image),&r));
    assert(r.decision==0 && r.retry_reason==TUDOR_RETRY_QUALITY);
    assert(tudor_verifier_count(v)==10);
    tudor_verifier_free(v); tudor_verifier_free(NULL);
}

int main(void)
{
    test_quality(); test_policy(); test_lifecycle();
    puts("verification quality, boundary, invalid-input and lifecycle tests passed");
    return 0;
}
