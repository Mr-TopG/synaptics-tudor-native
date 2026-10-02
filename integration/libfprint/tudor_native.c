/* SPDX-License-Identifier: MIT */
/* Private FpDevice adapter. Matching requires a separate experimental build. */
#include "fpi-device.h"
#include "fpi-image.h"
#include "tudor_backend.h"
#include <string.h>
#ifdef TUDOR_MATCHING_LAB
#include "fpi-print.h"
#include "tudor_template.h"
#endif

typedef struct {
    FpDevice parent;
    TudorBackend *backend;
    GCancellable *cancel;
    gboolean removed;
#ifdef TUDOR_MATCHING_LAB
    TudorEnrollment *enrollment;
    guint attempts;
    guint serial;
#endif
} FpDeviceTudorNative;
typedef FpDeviceClass FpDeviceTudorNativeClass;
GType fpi_device_tudor_native_get_type(void);
G_DEFINE_TYPE(FpDeviceTudorNative,fpi_device_tudor_native,FP_TYPE_DEVICE)

enum Operation { OPEN, CLOSE, CAPTURE, ENROLL, VERIFY };
struct Job { enum Operation operation; struct tudor_gray_frame image;
#ifdef TUDOR_MATCHING_LAB
    GVariant *template;
    gboolean automatic;
    guint serial;
    int decision;
    struct tudor_verification_result comparison;
#endif
};
static void start(FpDevice *device, enum Operation operation);

static void free_job(gpointer data)
{
    struct Job *job=data;
    tudor_gray_frame_clear(&job->image);
#ifdef TUDOR_MATCHING_LAB
    g_clear_pointer(&job->template,g_variant_unref);
#endif
    g_free(job);
}

#ifdef TUDOR_MATCHING_LAB
struct ContactNotice { FpDeviceTudorNative *self; guint serial; enum tudor_capture_event event; };
static gboolean report_contact(gpointer opaque)
{
    struct ContactNotice *n=opaque;
    FpDevice *device=FP_DEVICE(n->self);
    if (n->serial==n->self->serial && !n->self->removed &&
        fpi_device_get_current_action(device)!=FPI_DEVICE_ACTION_NONE &&
        fpi_device_get_current_action(device)!=FPI_DEVICE_ACTION_CLOSE) {
        if (n->event==TUDOR_CAPTURE_NEED_TOUCH)
            fpi_device_report_finger_status(device,FP_FINGER_STATUS_NEEDED);
        else if (n->event==TUDOR_CAPTURE_SETTLING || n->event==TUDOR_CAPTURE_WAITING)
            fpi_device_report_finger_status(device,FP_FINGER_STATUS_NEEDED|FP_FINGER_STATUS_PRESENT);
    }
    return G_SOURCE_REMOVE;
}
static void free_notice(gpointer opaque)
{ struct ContactNotice *n=opaque; g_object_unref(n->self); g_free(n); }
static void queue_contact(void *opaque,enum tudor_capture_event event)
{
    GTask *task=opaque;
    struct Job *job=g_task_get_task_data(task);
    struct ContactNotice *n=g_new0(struct ContactNotice,1);
    n->self=g_object_ref(g_task_get_source_object(task)); n->serial=job->serial; n->event=event;
    GSource *source=g_idle_source_new();
    g_source_set_callback(source,report_contact,n,free_notice);
    g_source_attach(source,g_task_get_context(task)); g_source_unref(source);
}
#endif
static gboolean capture_job(GTask *task,FpDeviceTudorNative *self,struct Job *job,
                            GCancellable *cancel,GError **error)
{
#ifdef TUDOR_MATCHING_LAB
    if (job->automatic) return tudor_backend_capture_automatic(self->backend,cancel,
        &job->image,queue_contact,task,error);
#else
    (void)task;
#endif
    return tudor_backend_capture(self->backend,cancel,&job->image,error);
}

static void work(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    FpDeviceTudorNative *self=source;
    struct Job *job=data;
    GError *error=NULL;
    gboolean ok;
    /* The transport submits GUsb async transfers and dispatches completion
     * on this private context, never the application's main context. */
    GMainContext *context=g_main_context_new();
    g_main_context_push_thread_default(context);
    if (job->operation==OPEN) ok=tudor_backend_open(self->backend,cancel,&error);
    else if (job->operation==CLOSE) ok=tudor_backend_close(self->backend,&error);
#ifdef TUDOR_MATCHING_LAB
    else if (job->operation==VERIFY) {
        struct tudor_verifier *v=tudor_template_import(job->template);
        ok=FALSE;
        if (!v) error=fpi_device_error_new_msg(FP_DEVICE_ERROR_DATA_INVALID,"Invalid experimental ten-scan template");
        else if (capture_job(task,self,job,cancel,&error)) {
            struct tudor_verification_result result;
            if (tudor_verifier_check(v,job->image.pixels,sizeof(job->image.pixels),&result))
                error=fpi_device_error_new(FP_DEVICE_ERROR_DATA_INVALID);
            else { job->decision=result.decision; job->comparison=result; ok=TRUE; }
        }
        tudor_verifier_free(v);
    }
#endif
    else {
        ok=capture_job(task,self,job,cancel,&error);
#ifdef TUDOR_MATCHING_LAB
        if (ok && job->operation==ENROLL) {
            int added=tudor_enrollment_add(self->enrollment,job->image.pixels);
            if (added!=TUDOR_BANK_OK) {
                ok=FALSE;
                error=added<0 ? fpi_device_error_new(FP_DEVICE_ERROR_GENERAL) :
                    fpi_device_retry_new_msg(FP_DEVICE_RETRY_GENERAL,"Unusable or duplicate scan; lift and replace the same finger");
            }
        }
#endif
    }
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    if (ok) g_task_return_boolean(task,TRUE);
    else g_task_return_error(task,error ? error : fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
}

static void completed(GObject *source, GAsyncResult *result, gpointer unused)
{
    FpDeviceTudorNative *self=(FpDeviceTudorNative *)source;
    FpDevice *device=FP_DEVICE(self);
    GTask *task=G_TASK(result);
    struct Job *job=g_task_get_task_data(task);
    GError *error=NULL;
    (void)unused;
    g_task_propagate_boolean(task,&error);
    /* A cancellation racing worker completion must never publish an image. */
    if (job->operation>=CAPTURE && !error &&
        (g_cancellable_is_cancelled(self->cancel) || fpi_device_action_is_cancelled(device) || self->removed))
        error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Capture cancelled after cleanup");
    g_clear_object(&self->cancel);
    if (job->operation==OPEN) {
        if (error) { tudor_backend_free(self->backend); self->backend=NULL; }
        fpi_device_open_complete(device,error);
    } else if (job->operation==CLOSE) {
        tudor_backend_free(self->backend); self->backend=NULL;
        fpi_device_close_complete(device,error);
#ifdef TUDOR_MATCHING_LAB
    } else if (job->operation==ENROLL) {
        tudor_gray_frame_clear(&job->image);
        if (fpi_device_action_is_cancelled(device) || self->removed) {
            g_clear_error(&error);
            error=g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Enrollment cancelled");
        }
        self->attempts++;
        if (error && error->domain!=FP_DEVICE_RETRY) {
            tudor_enrollment_free(self->enrollment); self->enrollment=NULL;
            fpi_device_enroll_complete(device,NULL,error); return;
        }
        guint count=self->enrollment->count;
        fpi_device_enroll_progress(device,(gint)count,NULL,error);
        /* Progress callbacks may cancel while the user repositions a finger. */
        if (fpi_device_action_is_cancelled(device) || self->removed) {
            tudor_enrollment_free(self->enrollment); self->enrollment=NULL;
            fpi_device_enroll_complete(device,NULL,g_error_new_literal(G_IO_ERROR,G_IO_ERROR_CANCELLED,"Enrollment cancelled"));
        } else if (count==TUDOR_ENROLLMENT_SCANS) {
            FpPrint *print=NULL;
            fpi_device_get_enroll_data(device,&print);
            g_autoptr(GVariant) data=tudor_enrollment_export(self->enrollment);
            fpi_print_set_type(print,FPI_PRINT_RAW);
            fpi_print_set_device_stored(print,FALSE);
            g_object_set(print,"fpi-data",data,NULL);
            tudor_enrollment_free(self->enrollment); self->enrollment=NULL;
            fpi_device_enroll_complete(device,g_object_ref(print),NULL);
        } else if (self->attempts>=30) {
            tudor_enrollment_free(self->enrollment); self->enrollment=NULL;
            fpi_device_enroll_complete(device,NULL,fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL,"Enrollment exceeded 30 attempts"));
        } else start(device,ENROLL);
    } else if (job->operation==VERIFY) {
        tudor_gray_frame_clear(&job->image);
        if (error && error->domain!=FP_DEVICE_RETRY) { fpi_device_verify_complete(device,error); return; }
        if (!error) {
            const char *decision=job->decision==TUDOR_DECISION_CANDIDATE_MATCH ? "candidate_match" :
                job->decision==TUDOR_DECISION_CANDIDATE_NONMATCH ? "candidate_nonmatch" : "retry";
            char score[G_ASCII_DTOSTR_BUF_SIZE];
            if (job->comparison.best_correlation>=-1)
                g_ascii_formatd(score,sizeof(score),"%.6f",job->comparison.best_correlation);
            else g_strlcpy(score,"not_computed",sizeof(score));
            g_printerr("Native comparison: decision=%s best_correlation=%s quality_flags=%u retry_reason=%d policy=%u automatic_contact=%s\n",
                decision,score,job->comparison.quality.flags,job->comparison.retry_reason,
                job->comparison.policy_version,job->automatic ? "true" : "false");
        }
        if (error || job->decision==TUDOR_DECISION_RETRY)
            fpi_device_verify_report(device,FPI_MATCH_ERROR,NULL,error ? error :
                fpi_device_retry_new_msg(FP_DEVICE_RETRY_GENERAL,"Experimental comparison uncertain; lift and retry"));
        else fpi_device_verify_report(device,job->decision==TUDOR_DECISION_CANDIDATE_MATCH ?
            FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,NULL,NULL);
        fpi_device_verify_complete(device,NULL);
#endif
    } else {
        FpImage *image=NULL;
        if (!error) {
            image=fp_image_new(TUDOR_FRAME_WIDTH,TUDOR_FRAME_HEIGHT);
            memcpy(image->data,job->image.pixels,TUDOR_GRAY_BYTES);
            image->ppmm=0; /* Unknown physical sampling resolution; capture only. */
        }
        tudor_gray_frame_clear(&job->image);
        fpi_device_capture_complete(device,image,error);
    }
}

static void start(FpDevice *device, enum Operation operation)
{
    FpDeviceTudorNative *self=(FpDeviceTudorNative *)device;
    g_assert(self->cancel==NULL);
    self->cancel=g_cancellable_new();
    if (operation!=CLOSE && (fpi_device_action_is_cancelled(device) || self->removed))
        g_cancellable_cancel(self->cancel);
    struct Job *job=g_new0(struct Job,1);
    job->operation=operation;
#ifdef TUDOR_MATCHING_LAB
    job->automatic=!g_strcmp0(g_getenv("TUDOR_NATIVE_AUTOMATIC_CONTACT"),"1");
    job->serial=++self->serial;
    if (operation==VERIFY) {
        FpPrint *print=NULL; fpi_device_get_verify_data(device,&print);
        g_object_get(print,"fpi-data",&job->template,NULL);
    }
#endif
    GTask *task=g_task_new(self,self->cancel,completed,NULL);
    /* Always join cleanup; GTask must not short-circuit a cancelled capture. */
    g_task_set_check_cancellable(task,FALSE);
    g_task_set_return_on_cancel(task,FALSE);
    g_task_set_task_data(task,job,free_job);
    g_task_run_in_thread(task,work);
    g_object_unref(task);
}

static void open_device(FpDevice *device)
{
    FpDeviceTudorNative *self=(FpDeviceTudorNative *)device;
    const char *enabled=g_getenv("TUDOR_NATIVE_EXPERIMENTAL");
    const char *state=g_getenv("TUDOR_NATIVE_PAIRING_DIR");
    const char *authority=g_getenv("TUDOR_NATIVE_AUTHORITY_FILE");
    if (g_strcmp0(enabled,"1") || !state || !authority ||
        !g_path_is_absolute(state) || !g_path_is_absolute(authority)) {
#ifndef TUDOR_ADAPTER_TEST
        /* libfprint opens the USB handle before this vfunc, and does not close
         * it on open failure. Release it so a corrected open can be retried. */
        g_usb_device_close(fpi_device_get_usb_device(device),NULL);
#endif
        fpi_device_open_complete(device,fpi_device_error_new_msg(FP_DEVICE_ERROR_NOT_SUPPORTED,
            "Experimental capture requires explicit opt-in and absolute pairing/authority paths"));
        return;
    }
#ifdef TUDOR_ADAPTER_TEST
    GUsbDevice *usb=NULL;
#else
    GUsbDevice *usb=fpi_device_get_usb_device(device);
#endif
    self->backend=tudor_backend_new(usb,state,authority);
    start(device,OPEN);
}
static void close_device(FpDevice *device) { start(device,CLOSE); }
static void capture(FpDevice *device)
{
    gboolean wait;
    fpi_device_get_capture_data(device,&wait);
    if (!wait) {
        fpi_device_capture_complete(device,NULL,fpi_device_error_new(FP_DEVICE_ERROR_NOT_SUPPORTED));
        return;
    }
    start(device,CAPTURE);
}
#ifdef TUDOR_MATCHING_LAB
static gboolean matching_enabled(void)
{ return !g_strcmp0(g_getenv("TUDOR_NATIVE_MATCHING_EXPERIMENTAL"),"1"); }
static void enroll(FpDevice *device)
{
    FpDeviceTudorNative *self=(FpDeviceTudorNative *)device;
    if (!matching_enabled()) {
        fpi_device_enroll_complete(device,NULL,fpi_device_error_new(FP_DEVICE_ERROR_NOT_SUPPORTED)); return;
    }
    self->enrollment=tudor_enrollment_new(); self->attempts=0;
    if (!self->enrollment) {
        fpi_device_enroll_complete(device,NULL,fpi_device_error_new(FP_DEVICE_ERROR_GENERAL)); return;
    }
    /* The manual lab uses stage zero to prompt before the first capture.
     * Automatic service clients should see only accepted scans as progress. */
    if (g_strcmp0(g_getenv("TUDOR_NATIVE_AUTOMATIC_CONTACT"),"1"))
        fpi_device_enroll_progress(device,0,NULL,NULL);
    start(device,ENROLL);
}
static void verify(FpDevice *device)
{
    if (!matching_enabled()) {
        fpi_device_verify_complete(device,fpi_device_error_new(FP_DEVICE_ERROR_NOT_SUPPORTED)); return;
    }
    start(device,VERIFY);
}
#else
static void reject_enrollment(FpDevice *device)
{
    /* libfprint 1.94.7 assumes every driver provides an enroll vfunc, even
     * without a verify feature. Explicitly reject instead of leaving it NULL. */
    fpi_device_enroll_complete(device,NULL,fpi_device_error_new_msg(FP_DEVICE_ERROR_NOT_SUPPORTED,
        "This experimental driver supports capture only"));
}
#endif
static void cancel_capture(FpDevice *device)
{
    FpDeviceTudorNative *self=(FpDeviceTudorNative *)device;
    if (self->cancel) g_cancellable_cancel(self->cancel);
}
static void removed(GObject *object, GParamSpec *property, gpointer unused)
{
    (void)property; (void)unused;
    /* libfprint waits for a pending action on removal; it does not cancel it
     * for us. Stop the worker and let its bounded cleanup finish first. */
    ((FpDeviceTudorNative *)object)->removed=TRUE;
    cancel_capture(FP_DEVICE(object));
}
static void fpi_device_tudor_native_init(FpDeviceTudorNative *self)
{
    g_signal_connect(self,"notify::removed",G_CALLBACK(removed),NULL);
}
static void fpi_device_tudor_native_class_init(FpDeviceTudorNativeClass *klass)
{
#ifdef TUDOR_ADAPTER_TEST
    static const FpIdEntry ids[]={{.virtual_envvar="TUDOR_FPRINTD_TEST_DEVICE"},{.virtual_envvar=NULL}};
#else
    static const FpIdEntry ids[]={{.vid=0x06cb,.pid=0x00be},{.vid=0,.pid=0}};
#endif
#ifdef TUDOR_MATCHING_LAB
    klass->id="tudor_native_lab";
#else
    klass->id="tudor_native";
#endif
    klass->full_name="Synaptics Tudor 06cb:00be experimental capture";
#ifdef TUDOR_ADAPTER_TEST
    klass->type=FP_DEVICE_TYPE_VIRTUAL;
#else
    klass->type=FP_DEVICE_TYPE_USB;
#endif
    klass->id_table=ids;
    klass->scan_type=FP_SCAN_TYPE_PRESS;
    klass->features=FP_DEVICE_FEATURE_CAPTURE;
    klass->open=open_device; klass->close=close_device;
    klass->capture=capture; klass->cancel=cancel_capture;
#ifdef TUDOR_MATCHING_LAB
    klass->features|=FP_DEVICE_FEATURE_VERIFY;
    klass->full_name="Synaptics Tudor experimental enrollment/verification lab";
    klass->nr_enroll_stages=TUDOR_ENROLLMENT_SCANS;
    klass->enroll=enroll; klass->verify=verify;
#else
    klass->enroll=reject_enrollment;
#endif
    /* Default libfprint suspend cancels and waits for capture completion. It
     * reports NOT_SUPPORTED; we do not pretend capture survives suspend. */
}
