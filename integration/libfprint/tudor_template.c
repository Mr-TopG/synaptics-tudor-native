/* SPDX-License-Identifier: MIT */
#include "tudor_template.h"
#include <string.h>
TudorEnrollment *tudor_enrollment_new(void)
{
    TudorEnrollment *e=g_new0(TudorEnrollment,1);
    e->verifier=tudor_verifier_create();
    if (!e->verifier) { g_free(e); return NULL; }
    return e;
}
void tudor_enrollment_free(TudorEnrollment *e)
{
    if (!e) return;
    tudor_verifier_free(e->verifier);
    volatile guint8 *p=(volatile guint8 *)e;
    for (gsize i=0;i<sizeof(*e);i++) p[i]=0;
    g_free(e);
}
int tudor_enrollment_add(TudorEnrollment *e, const guint8 *pixels)
{
    if (!e || !pixels) return TUDOR_BANK_INVALID;
    struct tudor_image_quality q;
    int result=tudor_verifier_enroll(e->verifier,pixels,TUDOR_IMAGE_PIXELS,&q);
    if (result==TUDOR_BANK_OK) memcpy(e->pixels[e->count++],pixels,TUDOR_IMAGE_PIXELS);
    return result;
}
GVariant *tudor_enrollment_export(TudorEnrollment *e)
{
    if (!e || e->count!=TUDOR_ENROLLMENT_SCANS) return NULL;
    return g_variant_ref_sink(g_variant_new("(uuuu@ay)",1u,
        (guint)TUDOR_EXPERIMENTAL_POLICY_VERSION,(guint)TUDOR_IMAGE_WIDTH,(guint)TUDOR_IMAGE_HEIGHT,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,e->pixels,sizeof(e->pixels),1)));
}
struct tudor_verifier *tudor_template_import(GVariant *data)
{
    if (!data || !g_variant_is_of_type(data,G_VARIANT_TYPE("(uuuuay)")) ||
        !g_variant_is_normal_form(data)) return NULL;
    guint version,policy,width,height;
    g_autoptr(GVariant) bytes=NULL;
    g_variant_get(data,"(uuuu@ay)",&version,&policy,&width,&height,&bytes);
    gsize size=0;
    const guint8 *pixels=g_variant_get_fixed_array(bytes,&size,1);
    if (version!=1 || policy!=TUDOR_EXPERIMENTAL_POLICY_VERSION || width!=TUDOR_IMAGE_WIDTH ||
        height!=TUDOR_IMAGE_HEIGHT || size!=TUDOR_ENROLLMENT_SCANS*TUDOR_IMAGE_PIXELS) return NULL;
    struct tudor_verifier *v=tudor_verifier_create();
    if (!v) return NULL;
    for (unsigned i=0;i<TUDOR_ENROLLMENT_SCANS;i++) {
        struct tudor_image_quality q;
        if (tudor_verifier_enroll(v,pixels+i*TUDOR_IMAGE_PIXELS,TUDOR_IMAGE_PIXELS,&q)!=TUDOR_BANK_OK) {
            tudor_verifier_free(v); return NULL;
        }
    }
    return v;
}
