/* SPDX-License-Identifier: MIT */
#include "tudor_transport.h"

struct Transfer {
    gboolean done;
    gssize bytes;
    GError *error;
    gssize (*finish)(GUsbDevice *, GAsyncResult *, GError **);
};

static void completed(GObject *source, GAsyncResult *result, gpointer opaque)
{
    struct Transfer *t=opaque;
    t->bytes=t->finish(G_USB_DEVICE(source),result,&t->error);
    t->done=TRUE;
}

static gssize wait_transfer(struct Transfer *t, GError **error)
{
    GMainContext *context=g_main_context_get_thread_default();
    /* GUsb 0.4.8's synchronous wrappers iterate the USB context's loop, but
     * their GTask completions target the calling thread's default context.
     * Submit asynchronously and dispatch that same private context instead.
     * Do not leave until completion: the transfer still owns the buffers. */
    while (!t->done) g_main_context_iteration(context,TRUE);
    if (t->error) g_propagate_error(error,t->error);
    return t->bytes;
}

static gboolean private_context(void)
{
    GMainContext *context=g_main_context_get_thread_default();
    return context && context!=g_main_context_default() && g_main_context_is_owner(context);
}

gssize tudor_usb_control(GUsbDevice *usb, GUsbDeviceRequestType type,
    guint8 request, guint16 value, guint8 *data, gsize size,
    GCancellable *cancel, GError **error)
{
    g_return_val_if_fail(private_context(),-1);
    struct Transfer t={.bytes=-1,.finish=g_usb_device_control_transfer_finish};
    g_usb_device_control_transfer_async(usb,G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
        type,G_USB_DEVICE_RECIPIENT_DEVICE,request,value,0,data,size,2000,cancel,completed,&t);
    return wait_transfer(&t,error);
}

gssize tudor_usb_transfer(GUsbDevice *usb, gboolean interrupt, guint8 endpoint,
    guint8 *data, gsize size, guint timeout, GCancellable *cancel, GError **error)
{
    g_return_val_if_fail(private_context(),-1);
    g_return_val_if_fail(timeout>0,-1);
    struct Transfer t={.bytes=-1,.finish=interrupt ? g_usb_device_interrupt_transfer_finish :
        g_usb_device_bulk_transfer_finish};
    if (interrupt)
        g_usb_device_interrupt_transfer_async(usb,endpoint,data,size,timeout,cancel,completed,&t);
    else
        g_usb_device_bulk_transfer_async(usb,endpoint,data,size,timeout,cancel,completed,&t);
    return wait_transfer(&t,error);
}
