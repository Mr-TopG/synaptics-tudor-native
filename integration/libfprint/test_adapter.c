/* SPDX-License-Identifier: MIT */
/* Actual libfprint public lifecycle, fake blocking backend, no USB or biometrics. */
#include "fpi-device.h"
#include "tudor_backend.h"
#include <string.h>

GType fpi_device_tudor_native_get_type(void);
struct TudorBackend { gboolean open; };
static gint mode, started, cleaned, freed;
static GThread *main_thread;

TudorBackend *tudor_backend_new(GUsbDevice *usb, const char *state, const char *authority)
{ (void)usb; (void)state; (void)authority; return g_new0(TudorBackend,1); }
void tudor_backend_free(TudorBackend *b)
{ g_assert(!b->open); g_atomic_int_inc(&freed); g_free(b); }
gboolean tudor_backend_open(TudorBackend *b, GCancellable *cancel, GError **error)
{
    g_assert(g_thread_self()!=main_thread);
    if (mode==7) {
        g_atomic_int_set(&started,1);
        while (!g_cancellable_is_cancelled(cancel)) g_usleep(1000);
        g_usleep(20000);
        g_atomic_int_set(&cleaned,1);
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_CANCELLED,"Synthetic cancelled open");
        return FALSE;
    }
    if (mode==1) { g_set_error_literal(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Synthetic open failure"); return FALSE; }
    b->open=TRUE; return TRUE;
}
gboolean tudor_backend_close(TudorBackend *b, GError **error)
{ (void)error; g_assert(g_thread_self()!=main_thread); b->open=FALSE; return TRUE; }
gboolean tudor_backend_capture(TudorBackend *b, GCancellable *cancel,
                               struct tudor_gray_frame *image, GError **error)
{
    g_assert(g_thread_self()!=main_thread && b->open);
    g_atomic_int_set(&started,1);
    if (mode==2 || mode==4 || mode==5 || mode==6) {
        while (!g_cancellable_is_cancelled(cancel)) g_usleep(1000);
        g_usleep(20000); /* Cleanup must finish before libfprint completion. */
        g_atomic_int_set(&cleaned,1);
        if (mode!=6) {
            g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_CANCELLED,"Synthetic cancelled capture");
            return FALSE;
        }
    }
    if (mode==3) {
        g_atomic_int_set(&cleaned,1);
        g_set_error_literal(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Synthetic cleanup failure");
        return FALSE;
    }
    for (unsigned i=0;i<TUDOR_GRAY_BYTES;i++) image->pixels[i]=(uint8_t)(i%251);
    g_atomic_int_set(&cleaned,1);
    return TRUE;
}

struct Run { GMainLoop *loop; gboolean completed, cancelled, suspended;
    GCancellable *cancel; FpDevice *device; guint ticks; };
static void suspend_done(GObject *source, GAsyncResult *result, gpointer opaque)
{
    struct Run *r=opaque;
    g_autoptr(GError) error=NULL;
    g_assert(!fp_device_suspend_finish(FP_DEVICE(source),result,&error));
    g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_NOT_SUPPORTED);
    g_assert_cmpint(g_atomic_int_get(&cleaned),==,1);
    r->suspended=TRUE;
    if (r->completed) g_main_loop_quit(r->loop);
}
static gboolean tick(gpointer opaque)
{
    struct Run *r=opaque;
    r->ticks++;
    if ((mode==2 || mode>=4) && !r->cancelled && g_atomic_int_get(&started)) {
        r->cancelled=TRUE;
        if (mode==4) fp_device_suspend(r->device,NULL,suspend_done,r);
        else if (mode==5) fpi_device_remove(r->device);
        else g_cancellable_cancel(r->cancel);
    }
    return G_SOURCE_CONTINUE;
}
static gboolean watchdog(gpointer unused) { (void)unused; g_error("Adapter test timed out"); return G_SOURCE_REMOVE; }
static void capture_done(GObject *source, GAsyncResult *result, gpointer opaque)
{
    struct Run *r=opaque;
    g_assert(g_thread_self()==main_thread);
    g_autoptr(GError) error=NULL;
    g_autoptr(FpImage) image=fp_device_capture_finish(FP_DEVICE(source),result,&error);
    g_assert_cmpint(g_atomic_int_get(&cleaned),==,1);
    if (mode==0) {
        g_assert_no_error(error); g_assert_nonnull(image);
        gsize size; const guint8 *pixels=fp_image_get_data(image,&size);
        g_assert_cmpuint(size,==,TUDOR_GRAY_BYTES);
        for (unsigned i=0;i<size;i++) g_assert_cmpuint(pixels[i],==,i%251);
    } else {
        g_assert_null(image);
        if (mode==2 || mode==6) g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
        else if (mode==4) g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_BUSY);
        else if (mode==5) g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_REMOVED);
        else g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO);
    }
    r->completed=TRUE;
    if (mode!=4 || r->suspended) g_main_loop_quit(r->loop);
}
static void run_case(int scenario)
{
    mode=scenario; g_atomic_int_set(&started,0); g_atomic_int_set(&cleaned,0);
    g_autoptr(FpDevice) device=g_object_new(fpi_device_tudor_native_get_type(),NULL);
    g_assert_cmpuint(fp_device_get_features(device),==,FP_DEVICE_FEATURE_CAPTURE);
    g_autoptr(GError) error=NULL;
    if (mode==7) {
        g_autoptr(GCancellable) cancel=g_cancellable_new();
        struct Run r={.cancel=cancel,.device=device};
        guint timer=g_timeout_add(1,tick,&r), guard=g_timeout_add_seconds(5,watchdog,NULL);
        g_assert(!fp_device_open_sync(device,cancel,&error));
        g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
        g_assert_cmpint(g_atomic_int_get(&cleaned),==,1);
        g_assert_cmpuint(r.ticks,>,1);
        g_assert(!fp_device_is_open(device));
        g_source_remove(timer); g_source_remove(guard);
        return;
    }
    gboolean opened=fp_device_open_sync(device,NULL,&error);
    if (mode==1) { g_assert(!opened); g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO); return; }
    g_assert(opened); g_assert_no_error(error);
    if (mode==0) {
        g_autoptr(FpPrint) template=fp_print_new(device);
        g_object_ref_sink(template);
        FpPrint *enrolled=fp_device_enroll_sync(device,template,NULL,NULL,NULL,&error);
        g_assert_null(enrolled); g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_NOT_SUPPORTED);
        g_clear_error(&error);
        gboolean matched=TRUE;
        g_assert(!fp_device_verify_sync(device,template,NULL,NULL,NULL,&matched,NULL,&error));
        g_assert(!matched); g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_NOT_SUPPORTED);
        g_clear_error(&error);
        g_assert_null(fp_device_capture_sync(device,FALSE,NULL,&error));
        g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_NOT_SUPPORTED);
        g_clear_error(&error);
    }
    struct Run run={.loop=g_main_loop_new(NULL,FALSE),.cancel=g_cancellable_new(),.device=device};
    guint timer=g_timeout_add(1,tick,&run);
    guint deadline=g_timeout_add_seconds(5,watchdog,NULL);
    fp_device_capture(device,TRUE,run.cancel,capture_done,&run);
    g_main_loop_run(run.loop);
    g_assert(run.completed);
    if (mode==2 || mode>=4) g_assert(run.cancelled && run.ticks>1);
    g_source_remove(timer); g_source_remove(deadline);
    g_object_unref(run.cancel); g_main_loop_unref(run.loop);
    if (mode==4) { g_assert(fp_device_resume_sync(device,NULL,&error)); g_assert_no_error(error); }
    gboolean closed=fp_device_close_sync(device,NULL,&error);
    if (mode==5) { g_assert(!closed); g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_REMOVED); }
    else { g_assert(closed); g_assert_no_error(error); }
    g_assert(!fp_device_is_open(device));
}
int main(void)
{
    main_thread=g_thread_self();
    g_setenv("TUDOR_NATIVE_EXPERIMENTAL","0",TRUE);
    {
        g_autoptr(FpDevice) device=g_object_new(fpi_device_tudor_native_get_type(),NULL);
        g_autoptr(GError) error=NULL;
        g_assert(!fp_device_open_sync(device,NULL,&error));
        g_assert_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_NOT_SUPPORTED);
        g_assert_cmpint(g_atomic_int_get(&freed),==,0);
    }
    g_setenv("TUDOR_NATIVE_EXPERIMENTAL","1",TRUE);
    g_setenv("TUDOR_NATIVE_PAIRING_DIR","/synthetic/state",TRUE);
    g_setenv("TUDOR_NATIVE_AUTHORITY_FILE","/synthetic/authority",TRUE);
    for (int i=0;i<8;i++) run_case(i);
    g_assert_cmpint(g_atomic_int_get(&freed),==,8);
    g_print("Adapter lifecycle passed: explicit opt-in, capture-only features, rejected enrollment/verification/unconditional capture, worker/main-thread separation, image handoff, open failure, cancellation and completion race, cleanup failure, suspend/resume, removal and release.\n");
    return 0;
}
