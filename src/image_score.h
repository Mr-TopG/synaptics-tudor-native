/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_IMAGE_SCORE_H
#define TUDOR_IMAGE_SCORE_H
#include <stdint.h>
#define TUDOR_IMAGE_WIDTH 104
#define TUDOR_IMAGE_HEIGHT 86
#define TUDOR_IMAGE_PIXELS (TUDOR_IMAGE_WIDTH * TUDOR_IMAGE_HEIGHT)
struct tudor_image_score {
    double correlation;
    double overlap_fraction;
};
/* Row-major 8-bit grayscale, exactly TUDOR_IMAGE_PIXELS bytes each.
 * Diagnostic only: no authentication threshold, identity or login decision.
 * Returns 0 on success, 1 for insufficient variance/overlap, -1 for NULL input.
 * On failure, output is unchanged. Inputs are never modified or retained.
 * Fixed high-pass sigma=3, rotations -30..30 step 3, minimum overlap 60%.
 * Translation search is coarse-to-fine, not an exhaustive maximum guarantee.
 */
int tudor_image_compare(const uint8_t *first, const uint8_t *second,
                       struct tudor_image_score *output);
/* Same return value as compare(image,image,&score), without computing the
 * maximum score. Stops at the first successful fine-search candidate and wipes
 * all temporaries. This checks scorer usability only, not biometric quality. */
int tudor_image_self_check(const uint8_t *image);
#endif
