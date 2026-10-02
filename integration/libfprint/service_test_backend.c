/* SPDX-License-Identifier: MIT */
/* TEST ONLY: compiled exclusively by --service-test, never hardware builds. */
#ifndef TUDOR_SERVICE_TEST
#error "Synthetic backend must not be compiled into a hardware build"
#endif
#include "tudor_backend.h"
#include "fpi-device.h"
#include <math.h>
#include <string.h>
struct TudorBackend { gboolean open; unsigned scans; };
TudorBackend *tudor_backend_new(GUsbDevice *usb,const char *state,const char *authority)
{ (void)usb; (void)state; (void)authority; return g_new0(TudorBackend,1); }
void tudor_backend_free(TudorBackend *b) { g_assert(!b->open); g_free(b); }
gboolean tudor_backend_open(TudorBackend *b,GCancellable *cancel,GError **error)
{ if (g_cancellable_set_error_if_cancelled(cancel,error)) return FALSE; b->open=TRUE; return TRUE; }
gboolean tudor_backend_close(TudorBackend *b,GError **error)
{ (void)error; b->open=FALSE; return TRUE; }
gboolean tudor_backend_capture(TudorBackend *b,GCancellable *cancel,
    struct tudor_gray_frame *image,GError **error)
{
    g_assert(b->open);
    const char *mode=g_getenv("TUDOR_SERVICE_TEST_MODE");
    if (!g_strcmp0(mode,"wait")) {
        g_printerr("Synthetic backend: capture is waiting for cancellation.\n");
        while (!g_cancellable_is_cancelled(cancel)) g_usleep(1000);
    }
    if (g_cancellable_set_error_if_cancelled(cancel,error)) return FALSE;
    double phase=!g_strcmp0(mode,"enroll") ? 0.2*++b->scans : 0.4;
    for (unsigned y=0;y<TUDOR_FRAME_HEIGHT;y++) for (unsigned x=0;x<TUDOR_FRAME_WIDTH;x++) {
        double value=!g_strcmp0(mode,"different") ? 0.91*x+0.011*y*y : 0.55*x+0.006*y*y+phase;
        image->pixels[y*TUDOR_FRAME_WIDTH+x]=!g_strcmp0(mode,"retry") ? 128 : (guint8)(128+60*sin(value));
    }
    return TRUE;
}
gboolean tudor_backend_capture_automatic(TudorBackend *b,GCancellable *cancel,
    struct tudor_gray_frame *image,void (*notify)(void *,enum tudor_capture_event),
    void *opaque,GError **error)
{
    notify(opaque,TUDOR_CAPTURE_NEED_LIFT);
    notify(opaque,TUDOR_CAPTURE_NEED_TOUCH);
    notify(opaque,TUDOR_CAPTURE_SETTLING);
    return tudor_backend_capture(b,cancel,image,error);
}
