/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_VERIFICATION_H
#define TUDOR_VERIFICATION_H
#include "image_bank.h"
#include "image_quality.h"
#define TUDOR_EXPERIMENTAL_POLICY_VERSION 1
#define TUDOR_EXPERIMENTAL_MATCH_SCORE 0.65
#define TUDOR_EXPERIMENTAL_NONMATCH_SCORE 0.50
enum tudor_experimental_decision {
    TUDOR_DECISION_RETRY=0, TUDOR_DECISION_CANDIDATE_MATCH=1,
    TUDOR_DECISION_CANDIDATE_NONMATCH=2
};
enum tudor_retry_reason {
    TUDOR_RETRY_NONE=0, TUDOR_RETRY_INVALID=1, TUDOR_RETRY_ENROLLMENT=2,
    TUDOR_RETRY_QUALITY=3, TUDOR_RETRY_UNUSABLE=4, TUDOR_RETRY_UNCERTAIN=5
};
struct tudor_verification_result {
    unsigned policy_version;
    int decision;
    int retry_reason;
    double best_correlation;
    struct tudor_image_quality quality;
};
struct tudor_verifier;
/* EXPERIMENTAL ONLY. Thresholds are engineering choices informed by current
 * development samples, not validated biometric acceptance/error-rate limits.
 * Never use this API to grant system authentication in its current form.
 * All ten enrollment images must pass the fixed quality gate and be distinct.
 * No persistence, automatic retries, enrollment adaptation or USB access.
 */
struct tudor_verifier *tudor_verifier_create(void);
void tudor_verifier_free(struct tudor_verifier *verifier);
unsigned tudor_verifier_count(const struct tudor_verifier *verifier);
/* Same status codes as image_bank_add; quality failure is BANK_UNUSABLE.
 * Rejected images do not consume enrollment slots. */
int tudor_verifier_enroll(struct tudor_verifier *verifier, const uint8_t *image,
                         size_t size, struct tudor_image_quality *quality);
/* Return -1 on invalid input, otherwise 0. Every failure initializes output
 * to RETRY rather than leaving a stale candidate decision in caller memory. */
int tudor_verifier_check(const struct tudor_verifier *verifier, const uint8_t *probe,
                        size_t size, struct tudor_verification_result *output);
/* Pure policy function, also useful for independent boundary/error tests.
 * A complete, coherent ten-reference result and quality assessment are required. */
int tudor_experimental_decide(const struct tudor_image_quality *quality,
                             const struct tudor_bank_scores *scores,
                             struct tudor_verification_result *output);
#endif
