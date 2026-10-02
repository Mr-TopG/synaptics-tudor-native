/* SPDX-License-Identifier: MIT */
#include "tudor_backend.h"
#include "capture_core.h"
#include "fpi-device.h"
#include "tudor_transport.h"
#include <string.h>

struct TudorBackend {
    GUsbDevice *usb;
    char *state_path, *authority_path;
    struct tudor_pairing_state state;
    struct tudor_version version;
    gboolean claimed, poisoned;
    GCancellable *cancel; /* Borrowed for the duration of the worker call. */
    GCancellable *open_cancel; /* Only pre-session transfers may abort immediately. */
    void (*notify)(void *,enum tudor_capture_event);
    void *notify_context;
    unsigned settling_ms;
};

TudorBackend *tudor_backend_new(GUsbDevice *usb, const char *state, const char *authority)
{
    TudorBackend *b=g_new0(TudorBackend,1);
    b->usb=g_object_ref(usb); b->state_path=g_strdup(state); b->authority_path=g_strdup(authority);
    return b;
}
void tudor_backend_free(TudorBackend *b)
{
    if (!b) return;
    tudor_state_clear(&b->state);
    g_clear_object(&b->usb); g_free(b->state_path); g_free(b->authority_path); g_free(b);
}

static int control(TudorBackend *b, GUsbDeviceRequestType type, guint8 request,
                   guint16 value, uint8_t *data, size_t size)
{
    g_autoptr(GError) error=NULL;
    gssize actual=tudor_usb_control(b->usb,type,request,value,data,size,b->open_cancel,&error);
    return actual>=0 && (gsize)actual<=size ? (int)actual : -1;
}
static int tls_status(void *opaque)
{
    uint8_t data[2];
    return control(opaque,G_USB_DEVICE_REQUEST_TYPE_VENDOR,0x14,0,data,2)==2 ? !!data[0] : -1;
}
static int exchange(void *opaque, const uint8_t *request, size_t size, uint8_t *response, size_t capacity)
{
    TudorBackend *b=opaque;
    g_autoptr(GError) error=NULL;
    if (!size || size>4096 || !capacity || capacity>65536) return -1;
    /* Finish a command/reply pair before acting on cancellation, including
     * GET_VERSION during open. Leaving an unread reply would dirty the pipe. */
    gssize actual=tudor_usb_transfer(b->usb,FALSE,0x01,(guint8 *)request,size,2000,NULL,&error);
    if (actual<0 || (gsize)actual!=size) return -1;
    actual=tudor_usb_transfer(b->usb,FALSE,0x81,response,capacity,2000,NULL,&error);
    return actual>=0 && (gsize)actual<=capacity ? (int)actual : -1;
}
static int interrupt(void *opaque, uint8_t response[8], unsigned timeout)
{
    TudorBackend *b=opaque;
    g_autoptr(GError) error=NULL;
    gssize actual=tudor_usb_transfer(b->usb,TRUE,0x83,response,8,timeout,NULL,&error);
    if (actual<0)
        return g_error_matches(error,G_USB_DEVICE_ERROR,G_USB_DEVICE_ERROR_TIMED_OUT) ? 0 : -1;
    return actual<=8 ? (int)actual : -1;
}
static int cancelled(void *opaque)
{
    return g_cancellable_is_cancelled(((TudorBackend *)opaque)->cancel);
}

gboolean tudor_backend_open(TudorBackend *b, GCancellable *cancel, GError **error)
{
    uint8_t descriptor[18], config[256], active;
    b->open_cancel=cancel;
    const char *stage="saved pairing validation";
    g_printerr("Native adapter: validating saved pairing.\n");
    if (g_cancellable_is_cancelled(cancel)) goto invalid;
    if (tudor_state_load(b->state_path,b->authority_path,&b->state)) goto invalid;
    stage="USB descriptor validation";
    g_printerr("Native adapter: checking USB descriptors.\n");
    if (g_cancellable_is_cancelled(cancel)) goto invalid;
    if (control(b,G_USB_DEVICE_REQUEST_TYPE_STANDARD,6,0x0100,descriptor,18)!=18 ||
        descriptor[0]!=18 || descriptor[1]!=1 || descriptor[8]!=0xcb || descriptor[9]!=6 ||
        descriptor[10]!=0xbe || descriptor[11]!=0) goto invalid;
    int n=control(b,G_USB_DEVICE_REQUEST_TYPE_STANDARD,6,0x0200,config,sizeof(config));
    if (n<0 || tudor_validate_config(config,(size_t)n) ||
        control(b,G_USB_DEVICE_REQUEST_TYPE_STANDARD,8,0,&active,1)!=1 || active!=1) goto invalid;
    if (g_cancellable_is_cancelled(cancel)) goto invalid;
    g_printerr("Native adapter: claiming interface and checking idle session.\n");
    if (!g_usb_device_claim_interface(b->usb,0,G_USB_DEVICE_CLAIM_INTERFACE_NONE,error)) goto failed;
    b->claimed=TRUE;
    stage="idle-session check";
    if (tls_status(b)!=0) goto invalid;
    if (g_cancellable_is_cancelled(cancel)) goto invalid;
    stage="firmware check";
    g_printerr("Native adapter: reading firmware version.\n");
    const uint8_t version=1;
    n=exchange(b,&version,1,config,sizeof(config));
    if (n<0 || tudor_parse_version(config,(size_t)n,&b->version) ||
        b->version.major!=10 || b->version.minor!=1 || b->version.product!=65 ||
        b->version.provision!=3 || !b->version.advanced_security || !b->version.key_flag) goto invalid;
    if (g_cancellable_is_cancelled(cancel)) goto invalid;
    b->open_cancel=NULL;
    g_printerr("Native adapter: open completed.\n");
    return TRUE;
invalid:
    if (!g_cancellable_set_error_if_cancelled(cancel,error))
        g_set_error(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Native adapter failed at %s",stage);
failed:
    b->open_cancel=NULL;
    if (b->claimed) {
        g_usb_device_release_interface(b->usb,0,G_USB_DEVICE_CLAIM_INTERFACE_NONE,NULL);
        b->claimed=FALSE;
    }
    tudor_state_clear(&b->state);
    g_usb_device_close(b->usb,NULL); /* Framework does not close failed opens. */
    return FALSE;
}
gboolean tudor_backend_close(TudorBackend *b, GError **error)
{
    gboolean ok=TRUE;
    if (b->claimed) ok=g_usb_device_release_interface(b->usb,0,G_USB_DEVICE_CLAIM_INTERFACE_NONE,error);
    b->claimed=FALSE;
    tudor_state_clear(&b->state);
    return ok;
}
static void contact_notify(void *opaque,enum tudor_capture_event event)
{
    TudorBackend *b=opaque;
    if (event==TUDOR_CAPTURE_NEED_LIFT) g_printerr("Automatic contact: lift your finger and keep the sensor clear.\n");
    else if (event==TUDOR_CAPTURE_NEED_TOUCH) g_printerr("Automatic contact: clear sensor confirmed; place your finger now.\n");
    else if (event==TUDOR_CAPTURE_SETTLING) g_printerr("Automatic contact: touch detected; hold still for %u milliseconds.\n",b->settling_ms);
    else if (event==TUDOR_CAPTURE_WAITING) g_printerr("Automatic contact: capturing; keep holding your finger still.\n");
    if (b->notify) b->notify(b->notify_context,event);
}
static gboolean capture_impl(TudorBackend *b, GCancellable *cancel,
                            struct tudor_gray_frame *image,gboolean automatic,GError **error)
{
    tudor_gray_frame_clear(image);
    b->settling_ms=1000;
    const char *settling=g_getenv("TUDOR_NATIVE_SETTLE_MS");
    if (automatic && settling) {
        if (!strcmp(settling,"500")) b->settling_ms=500;
        else if (strcmp(settling,"1000")) {
            g_set_error_literal(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_GENERAL,
                "TUDOR_NATIVE_SETTLE_MS must be 500 or 1000");
            return FALSE;
        }
    }
    if (!b->claimed || b->poisoned) {
        g_set_error_literal(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Reopen device after incomplete capture cleanup");
        return FALSE;
    }
    b->cancel=cancel;
    const struct tudor_capture_ops ops={.exchange=exchange,.tls_status=tls_status,
        .interrupt=interrupt,.cancelled=cancelled,.automatic_contact=automatic,
        .notify=automatic ? contact_notify : NULL,.settling_ms=b->settling_ms};
    struct tudor_capture_result result;
    int status=tudor_capture_run(&b->state,&b->version,&ops,b,&result);
    /* Manual mode expects settled contact supplied by the caller. Automatic
     * mode uses sensor events before acquisition. Neither mode reads a terminal. */
    if (!result.session_closed) b->poisoned=TRUE;
    gboolean ok=status==TUDOR_CAPTURE_OK && !tudor_frame_decode(result.response,result.response_size,image);
    tudor_capture_result_clear(&result); b->cancel=NULL;
    if (ok) return TRUE;
    if (status==TUDOR_CAPTURE_CANCELLED)
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_CANCELLED,"Capture cancelled after cleanup");
    else if (status==TUDOR_CAPTURE_TIMEOUT || status==TUDOR_CAPTURE_OK)
        g_set_error_literal(error,FP_DEVICE_RETRY,FP_DEVICE_RETRY_GENERAL,"No usable frame; lift and try again");
    else g_set_error_literal(error,FP_DEVICE_ERROR,FP_DEVICE_ERROR_PROTO,"Native capture failed; image withheld");
    return FALSE;
}

gboolean tudor_backend_capture(TudorBackend *b,GCancellable *cancel,
                               struct tudor_gray_frame *image,GError **error)
{ return capture_impl(b,cancel,image,FALSE,error); }

gboolean tudor_backend_capture_automatic(TudorBackend *b,GCancellable *cancel,
    struct tudor_gray_frame *image,void (*notify)(void *,enum tudor_capture_event),
    void *notify_context,GError **error)
{
    b->notify=notify; b->notify_context=notify_context;
    gboolean ok=capture_impl(b,cancel,image,TRUE,error);
    b->notify=NULL; b->notify_context=NULL;
    return ok;
}
