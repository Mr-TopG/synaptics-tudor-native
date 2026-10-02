/* SPDX-License-Identifier: MIT */
/* Public libfprint API, synthetic captures and real C template/matcher. */
#include "fpi-device.h"
#include "fpi-print.h"
#include "tudor_backend.h"
#include "tudor_template.h"
#include <math.h>
#include <string.h>

GType fpi_device_tudor_native_get_type(void);
struct TudorBackend { gboolean open; };
static GThread *main_thread;
static int scans, mode, progress_count, retries;
static gint started, cleaned;
static unsigned contact_notices;
static void ridges(guint8 *image, double phase)
{
    for (int y=0;y<TUDOR_IMAGE_HEIGHT;y++)
        for (int x=0;x<TUDOR_IMAGE_WIDTH;x++)
            image[y*TUDOR_IMAGE_WIDTH+x]=(guint8)(128+60*sin(0.55*x+0.006*y*y+phase));
}
TudorBackend *tudor_backend_new(GUsbDevice *usb,const char *state,const char *authority)
{ (void)usb; (void)state; (void)authority; return g_new0(TudorBackend,1); }
void tudor_backend_free(TudorBackend *b) { g_assert(!b->open); g_free(b); }
gboolean tudor_backend_open(TudorBackend *b,GCancellable *cancel,GError **error)
{ (void)cancel; (void)error; b->open=TRUE; return TRUE; }
gboolean tudor_backend_close(TudorBackend *b,GError **error)
{ (void)error; b->open=FALSE; return TRUE; }
gboolean tudor_backend_capture(TudorBackend *b,GCancellable *cancel,
    struct tudor_gray_frame *image,GError **error)
{
    g_assert(b->open && main_thread!=g_thread_self());
    if (mode==3) {
        g_atomic_int_set(&started,1);
        while (!g_cancellable_is_cancelled(cancel)) g_usleep(1000);
        g_usleep(20000); g_atomic_int_set(&cleaned,1);
    }
    if (g_cancellable_set_error_if_cancelled(cancel,error)) return FALSE;
    scans++;
    if (mode==2 || mode==5) memset(image->pixels,128,sizeof(image->pixels));
    else if (mode==6) {
        g_set_error_literal(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Synthetic capture cleanup failure");
        return FALSE;
    } else if (mode==7) {
        for (int y=0;y<TUDOR_IMAGE_HEIGHT;y++)
            for (int x=0;x<TUDOR_IMAGE_WIDTH;x++)
                image->pixels[y*TUDOR_IMAGE_WIDTH+x]=(guint8)(128+60*sin(0.91*x+0.011*y*y));
    } else if (mode==1) ridges(image->pixels,0.4); /* exact replay for API testing */
    else ridges(image->pixels,scans==2 ? 0.2 : 0.2*(scans-(scans>2)));
    return TRUE;
}
gboolean tudor_backend_capture_automatic(TudorBackend *b,GCancellable *cancel,
    struct tudor_gray_frame *image,void (*notify)(void *,enum tudor_capture_event),
    void *opaque,GError **error)
{
    g_assert(main_thread!=g_thread_self());
    notify(opaque,TUDOR_CAPTURE_NEED_LIFT);
    notify(opaque,TUDOR_CAPTURE_NEED_TOUCH);
    notify(opaque,TUDOR_CAPTURE_SETTLING);
    notify(opaque,TUDOR_CAPTURE_WAITING);
    return tudor_backend_capture(b,cancel,image,error);
}
static void finger_status_changed(GObject *object,GParamSpec *property,gpointer unused)
{
    (void)object; (void)property; (void)unused;
    g_assert(main_thread==g_thread_self()); contact_notices++;
}
static void progress(FpDevice *device,gint count,FpPrint *print,gpointer opaque,GError *error)
{
    (void)device; (void)print;
    g_assert(main_thread==g_thread_self());
    g_assert(count>=progress_count && count<=progress_count+1);
    if (error) { g_assert(error->domain==FP_DEVICE_RETRY); retries++; }
    progress_count=count;
    if (mode==4 && count==5) g_cancellable_cancel(opaque);
    if (mode==8 && count==10) g_cancellable_cancel(opaque);
    if (mode==9 && count==0) g_cancellable_cancel(opaque);
}
static gboolean cancel_tick(gpointer opaque)
{
    if (g_atomic_int_get(&started)) g_cancellable_cancel(opaque);
    return G_SOURCE_CONTINUE;
}
static FpPrint *enrollment(FpDevice *device,GCancellable *cancel,GError **error)
{
    scans=0; progress_count=0; retries=0;
    g_autoptr(FpPrint) input=fp_print_new(device); g_object_ref_sink(input);
    return fp_device_enroll_sync(device,input,cancel,progress,cancel,error);
}
int main(void)
{
    main_thread=g_thread_self();
    g_setenv("TUDOR_NATIVE_EXPERIMENTAL","1",TRUE);
    g_setenv("TUDOR_NATIVE_PAIRING_DIR","/synthetic/state",TRUE);
    g_setenv("TUDOR_NATIVE_AUTHORITY_FILE","/synthetic/authority",TRUE);
    g_setenv("TUDOR_NATIVE_MATCHING_EXPERIMENTAL","0",TRUE);
    g_setenv("TUDOR_NATIVE_AUTOMATIC_CONTACT","0",TRUE);
    g_autoptr(FpDevice) device=g_object_new(fpi_device_tudor_native_get_type(),NULL);
    g_assert_cmpstr(fp_device_get_driver(device),==,"tudor_native_lab");
    g_assert_cmpint(fp_device_get_nr_enroll_stages(device),==,10);
    g_assert(fp_device_has_feature(device,FP_DEVICE_FEATURE_VERIFY));
    g_assert(!fp_device_has_feature(device,FP_DEVICE_FEATURE_IDENTIFY));
    g_autoptr(GError) error=NULL;
    g_assert(fp_device_open_sync(device,NULL,&error));
    g_assert_null(enrollment(device,NULL,&error));
    g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_NOT_SUPPORTED); g_clear_error(&error);
    g_setenv("TUDOR_NATIVE_MATCHING_EXPERIMENTAL","1",TRUE);
    g_autoptr(FpPrint) print=enrollment(device,NULL,&error);
    g_assert_no_error(error); g_assert_nonnull(print);
    g_assert_cmpint(progress_count,==,10); g_assert_cmpint(scans,==,11); g_assert_cmpint(retries,==,1);

    guint8 *serialized=NULL; gsize size=0;
    g_assert(fp_print_serialize(print,&serialized,&size,&error));
    g_autoptr(FpPrint) restored=fp_print_deserialize(serialized,size,&error);
    g_assert_no_error(error); g_assert_nonnull(restored); g_free(serialized);
    g_assert(fp_print_equal(print,restored));
    gboolean matched=FALSE;
    mode=1;
    g_assert(fp_device_verify_sync(device,restored,NULL,NULL,NULL,&matched,NULL,&error));
    g_assert_no_error(error); g_assert(matched);
    mode=7; matched=TRUE;
    g_assert(fp_device_verify_sync(device,restored,NULL,NULL,NULL,&matched,NULL,&error));
    g_assert_no_error(error); g_assert(!matched);
    mode=2; matched=TRUE;
    g_assert(!fp_device_verify_sync(device,restored,NULL,NULL,NULL,&matched,NULL,&error));
    g_assert_error(error,FP_DEVICE_RETRY,FP_DEVICE_RETRY_GENERAL); g_assert(!matched); g_clear_error(&error);

    /* Invalid shape and version fail before capture. */
    g_autoptr(GVariant) valid=NULL;
    g_object_get(restored,"fpi-data",&valid,NULL);
    GVariant *bad[]={g_variant_new_string("wrong"),g_variant_new("(uuuu@ay)",2u,1u,104u,86u,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,"",0,1))};
    for (unsigned i=0;i<G_N_ELEMENTS(bad);i++) {
        g_variant_ref_sink(bad[i]); g_object_set(restored,"fpi-data",bad[i],NULL); g_variant_unref(bad[i]);
        int before=scans; matched=TRUE;
        g_assert(!fp_device_verify_sync(device,restored,NULL,NULL,NULL,&matched,NULL,&error));
        g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_DATA_INVALID);
        g_assert(!matched); g_assert_cmpint(scans,==,before); g_clear_error(&error);
    }
    g_object_set(restored,"fpi-data",valid,NULL);
    {
        guint format,policy,width,height;
        g_autoptr(GVariant) array=NULL;
        g_variant_get(valid,"(uuuu@ay)",&format,&policy,&width,&height,&array);
        gsize count=0;
        const guint8 *pixels=g_variant_get_fixed_array(array,&count,1);
        guint8 *duplicate=g_memdup2(pixels,count);
        memcpy(duplicate+TUDOR_IMAGE_PIXELS,duplicate,TUDOR_IMAGE_PIXELS);
        g_autoptr(GVariant) invalid=g_variant_ref_sink(g_variant_new("(uuuu@ay)",format,policy,width,height,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,duplicate,count,1)));
        g_free(duplicate);
        g_assert_null(tudor_template_import(invalid));
        g_autoptr(GVariant) wrong_policy=g_variant_ref_sink(g_variant_new("(uuuu@ay)",format,policy+1,width,height,
            g_variant_ref(array)));
        g_assert_null(tudor_template_import(wrong_policy));
    }
    g_autoptr(GCancellable) cancel=g_cancellable_new();
    mode=3;
    guint timer=g_timeout_add(1,cancel_tick,cancel);
    g_assert(!fp_device_verify_sync(device,restored,cancel,NULL,NULL,&matched,NULL,&error));
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED); g_assert(!matched);
    g_assert_cmpint(g_atomic_int_get(&cleaned),==,1); g_source_remove(timer); g_clear_error(&error);
    g_cancellable_reset(cancel); mode=4;
    g_assert_null(enrollment(device,cancel,&error));
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED); g_clear_error(&error);
    g_assert_cmpint(progress_count,==,5);
    for (int test=8;test<=9;test++) {
        g_cancellable_reset(cancel); mode=test;
        g_assert_null(enrollment(device,cancel,&error));
        g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED); g_clear_error(&error);
        g_assert_cmpint(progress_count,==,test==8 ? 10 : 0);
    }
    mode=5;
    g_assert_null(enrollment(device,NULL,&error));
    g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_GENERAL); g_clear_error(&error);
    g_assert_cmpint(scans,==,30); g_assert_cmpint(progress_count,==,0);
    mode=6;
    g_assert_null(enrollment(device,NULL,&error));
    g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO); g_clear_error(&error);
    g_assert(!fp_device_verify_sync(device,restored,NULL,NULL,NULL,&matched,NULL,&error));
    g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO); g_assert(!matched); g_clear_error(&error);
    mode=1;
    g_setenv("TUDOR_NATIVE_AUTOMATIC_CONTACT","1",TRUE);
    g_signal_connect(device,"notify::finger-status",G_CALLBACK(finger_status_changed),NULL);
    g_assert(fp_device_verify_sync(device,restored,NULL,NULL,NULL,&matched,NULL,&error));
    g_assert(matched); g_assert_no_error(error);
    g_assert_cmpuint(contact_notices,>=,2);
    g_assert(fp_device_close_sync(device,NULL,&error));
    g_print("Matching API passed: opt-in, ten accepted distinct scans, duplicate retry, serialized FpPrint roundtrip, exact replay, quality retry, invalid templates, cancellation, bounded retries, cleanup failure and recovery. Synthetic data only; not biometric accuracy validation.\n");
    return 0;
}
