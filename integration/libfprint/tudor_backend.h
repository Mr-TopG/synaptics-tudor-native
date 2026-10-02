/* SPDX-License-Identifier: MIT */
#pragma once
#include <gusb.h>
#include "frame_decode.h"
#include "capture_core.h"
typedef struct TudorBackend TudorBackend;
TudorBackend *tudor_backend_new(GUsbDevice *usb, const char *state, const char *authority);
void tudor_backend_free(TudorBackend *backend);
gboolean tudor_backend_open(TudorBackend *backend, GCancellable *cancel, GError **error);
gboolean tudor_backend_close(TudorBackend *backend, GError **error);
gboolean tudor_backend_capture(TudorBackend *backend, GCancellable *cancel,
                               struct tudor_gray_frame *image, GError **error);
gboolean tudor_backend_capture_automatic(TudorBackend *backend, GCancellable *cancel,
    struct tudor_gray_frame *image,
    void (*notify)(void *, enum tudor_capture_event), void *notify_context, GError **error);
