/* SPDX-License-Identifier: MIT */
#pragma once
#include <gio/gio.h>
#include "verification.h"
typedef struct {
    struct tudor_verifier *verifier;
    guint count;
    guint8 pixels[TUDOR_ENROLLMENT_SCANS][TUDOR_IMAGE_PIXELS];
} TudorEnrollment;
TudorEnrollment *tudor_enrollment_new(void);
void tudor_enrollment_free(TudorEnrollment *e);
int tudor_enrollment_add(TudorEnrollment *e, const guint8 *pixels);
GVariant *tudor_enrollment_export(TudorEnrollment *e);
struct tudor_verifier *tudor_template_import(GVariant *data);
