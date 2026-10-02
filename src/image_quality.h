/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_IMAGE_QUALITY_H
#define TUDOR_IMAGE_QUALITY_H
#include <stddef.h>
#include <stdint.h>
#include "image_score.h"
enum tudor_quality_flags {
    TUDOR_QUALITY_LOW_CONTRAST=1u, TUDOR_QUALITY_LOW_COVERAGE=2u,
    TUDOR_QUALITY_LOW_STRUCTURE=4u, TUDOR_QUALITY_DIRECTIONAL=8u,
    TUDOR_QUALITY_CLIPPED=16u, TUDOR_QUALITY_INVALID=32u
};
struct tudor_image_quality {
    unsigned flags;
    double standard_deviation;
    double active_fraction;
    double local_coherence;
    double global_coherence;
    double clipped_fraction;
};
/* Fixed experimental heuristics on normalized 104x86 grayscale, not a
 * calibrated biometric quality score or liveness/spoof detector. Returns
 * -1 for invalid input (sets INVALID when output exists), otherwise 0.
 * Flags request another placement; they do not prove a sample is a fingerprint.
 */
int tudor_image_assess(const uint8_t *image, size_t size, struct tudor_image_quality *output);
#endif
