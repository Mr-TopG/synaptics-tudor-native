/* SPDX-License-Identifier: MIT */
#pragma once
#include <gusb.h>

/* Worker-only: the caller must push a private thread-default context. */
gssize tudor_usb_control(GUsbDevice *usb, GUsbDeviceRequestType type,
    guint8 request, guint16 value, guint8 *data, gsize size,
    GCancellable *cancel, GError **error);
gssize tudor_usb_transfer(GUsbDevice *usb, gboolean interrupt, guint8 endpoint,
    guint8 *data, gsize size, guint timeout, GCancellable *cancel, GError **error);
