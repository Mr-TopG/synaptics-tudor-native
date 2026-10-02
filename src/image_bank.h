/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_IMAGE_BANK_H
#define TUDOR_IMAGE_BANK_H
#include <stddef.h>
#include "image_score.h"
#define TUDOR_ENROLLMENT_SCANS 10
struct tudor_image_bank;
enum tudor_bank_status {
    TUDOR_BANK_OK=0, TUDOR_BANK_FLAT=1, TUDOR_BANK_DUPLICATE=2,
    TUDOR_BANK_INCOMPLETE=3, TUDOR_BANK_UNUSABLE=4,
    TUDOR_BANK_INVALID=-1, TUDOR_BANK_FULL=-2
};
struct tudor_bank_scores {
    unsigned valid_references;
    double best_correlation;
    struct tudor_image_score scores[TUDOR_ENROLLMENT_SCANS];
    uint8_t valid[TUDOR_ENROLLMENT_SCANS];
};
/* Memory-only experimental references. No storage, USB, login or threshold.
 * create returns NULL on allocation failure. free(NULL) is permitted.
 * add copies one correctly sized, non-flat, nonduplicate grayscale image.
 * compare requires exactly ten accepted images; probes never alter the bank.
 * Failed add/compare operations leave the bank/output respectively unchanged.
 * free wipes the bank before deallocation; it does not wipe caller buffers.
 */
struct tudor_image_bank *tudor_image_bank_create(void);
void tudor_image_bank_free(struct tudor_image_bank *bank);
unsigned tudor_image_bank_count(const struct tudor_image_bank *bank);
int tudor_image_bank_add(struct tudor_image_bank *bank, const uint8_t *image, size_t size);
int tudor_image_bank_compare(const struct tudor_image_bank *bank, const uint8_t *probe,
                            size_t size, struct tudor_bank_scores *output);
#endif
