/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_FRAME_DECODE_H
#define TUDOR_FRAME_DECODE_H
#include "protocol.h"
#define TUDOR_GRAY_BYTES (TUDOR_FRAME_WIDTH * TUDOR_FRAME_HEIGHT)
struct tudor_gray_frame {
    int contrast_low, contrast_high;
    uint8_t pixels[TUDOR_GRAY_BYTES]; /* row-major, 104x86, unsigned 8-bit */
};
/* Same provisional signed-16 LE, column-major, 1st/99th percentile rendering as
 * the local preview. This does not calibrate sensor resolution or quality.
 * Return 0 success, 1 no contrast, -1 invalid input/allocation failure.
 * Input and output may not overlap. Every failure wipes output. No file I/O.
 */
int tudor_frame_decode(const uint8_t *response, size_t size, struct tudor_gray_frame *output);
void tudor_gray_frame_clear(struct tudor_gray_frame *frame);
#endif
