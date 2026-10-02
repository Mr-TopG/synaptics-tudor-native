/* SPDX-License-Identifier: MIT */
#include "verification.h"
#include <math.h>
#include <stdlib.h>

struct tudor_verifier { struct tudor_image_bank *bank; };

static void retry(struct tudor_verification_result *out, int reason)
{
    *out=(struct tudor_verification_result){.policy_version=TUDOR_EXPERIMENTAL_POLICY_VERSION,
        .decision=TUDOR_DECISION_RETRY,.retry_reason=reason,.best_correlation=-2};
}

struct tudor_verifier *tudor_verifier_create(void)
{
    struct tudor_verifier *v=calloc(1,sizeof(*v));
    if (v && !(v->bank=tudor_image_bank_create())) { free(v); return NULL; }
    return v;
}

void tudor_verifier_free(struct tudor_verifier *v)
{
    if (!v) return;
    tudor_image_bank_free(v->bank);
    volatile unsigned char *p=(volatile unsigned char *)v;
    for (size_t i=0; i<sizeof(*v); i++) p[i]=0;
    free(v);
}

unsigned tudor_verifier_count(const struct tudor_verifier *v)
{
    return v ? tudor_image_bank_count(v->bank) : 0;
}

int tudor_verifier_enroll(struct tudor_verifier *v, const uint8_t *image,
                         size_t size, struct tudor_image_quality *quality)
{
    if (quality) *quality=(struct tudor_image_quality){.flags=TUDOR_QUALITY_INVALID};
    if (!v || !quality || !image || size!=TUDOR_IMAGE_PIXELS) return TUDOR_BANK_INVALID;
    if (tudor_verifier_count(v)>=TUDOR_ENROLLMENT_SCANS) return TUDOR_BANK_FULL;
    if (tudor_image_assess(image,size,quality)) return TUDOR_BANK_INVALID;
    if (quality->flags) return TUDOR_BANK_UNUSABLE;
    return tudor_image_bank_add(v->bank,image,size);
}

static int quality_valid(const struct tudor_image_quality *q)
{
    return q && !(q->flags&~63u) && isfinite(q->standard_deviation) &&
        q->standard_deviation>=0 && q->standard_deviation<=127.5 &&
        isfinite(q->active_fraction) && q->active_fraction>=0 && q->active_fraction<=1 &&
        isfinite(q->local_coherence) && q->local_coherence>=0 && q->local_coherence<=1 &&
        isfinite(q->global_coherence) && q->global_coherence>=0 && q->global_coherence<=1 &&
        isfinite(q->clipped_fraction) && q->clipped_fraction>=0 && q->clipped_fraction<=1;
}

int tudor_experimental_decide(const struct tudor_image_quality *q,
                             const struct tudor_bank_scores *scores,
                             struct tudor_verification_result *out)
{
    if (!out) return -1;
    retry(out,TUDOR_RETRY_INVALID);
    if (!quality_valid(q) || (q->flags&TUDOR_QUALITY_INVALID) || !scores) return -1;
    out->quality=*q;
    if (q->flags || q->standard_deviation<5 || q->active_fraction<0.4 ||
        q->local_coherence<0.25 || q->global_coherence>0.995 || q->clipped_fraction>0.70) {
        out->retry_reason=TUDOR_RETRY_QUALITY; return 0;
    }
    unsigned count=0; double best=-2;
    for (unsigned i=0; i<TUDOR_ENROLLMENT_SCANS; i++) {
        if (scores->valid[i]>1) return -1;
        if (!scores->valid[i]) continue;
        double value=scores->scores[i].correlation, overlap=scores->scores[i].overlap_fraction;
        if (!isfinite(value) || value < -1 || value>1 || !isfinite(overlap) || overlap<0.6 || overlap>1)
            return -1;
        count++; if (value>best) best=value;
    }
    if (count!=scores->valid_references || !isfinite(scores->best_correlation) ||
        (count && fabs(best-scores->best_correlation)>1e-9)) return -1;
    if (count!=TUDOR_ENROLLMENT_SCANS) { out->retry_reason=TUDOR_RETRY_UNUSABLE; return 0; }
    out->best_correlation=best;
    out->retry_reason=TUDOR_RETRY_NONE;
    if (best>=TUDOR_EXPERIMENTAL_MATCH_SCORE) out->decision=TUDOR_DECISION_CANDIDATE_MATCH;
    else if (best<=TUDOR_EXPERIMENTAL_NONMATCH_SCORE) out->decision=TUDOR_DECISION_CANDIDATE_NONMATCH;
    else out->retry_reason=TUDOR_RETRY_UNCERTAIN;
    return 0;
}

int tudor_verifier_check(const struct tudor_verifier *v, const uint8_t *probe,
                        size_t size, struct tudor_verification_result *out)
{
    if (!out) return -1;
    retry(out,TUDOR_RETRY_INVALID);
    if (!v || !probe || size!=TUDOR_IMAGE_PIXELS) return -1;
    if (tudor_verifier_count(v)!=TUDOR_ENROLLMENT_SCANS) {
        out->retry_reason=TUDOR_RETRY_ENROLLMENT; return 0;
    }
    struct tudor_image_quality quality;
    if (tudor_image_assess(probe,size,&quality)) return -1;
    if (quality.flags) {
        out->quality=quality; out->retry_reason=TUDOR_RETRY_QUALITY; return 0;
    }
    struct tudor_bank_scores scores;
    if (tudor_image_bank_compare(v->bank,probe,size,&scores)) {
        out->quality=quality; out->retry_reason=TUDOR_RETRY_UNUSABLE; return 0;
    }
    return tudor_experimental_decide(&quality,&scores,out);
}
