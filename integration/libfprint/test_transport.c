/* SPDX-License-Identifier: MIT */
/* Real GUsb replay, synthetic bytes only: no physical USB, keys or images. */
#include "tudor_transport.h"
#include <json-glib/json-glib.h>

static gpointer worker(gpointer opaque)
{
    GUsbDevice *usb=opaque;
    g_autoptr(GMainContext) context=g_main_context_new();
    g_main_context_push_thread_default(context);
    g_autoptr(GError) error=NULL;
    guint8 data[2]={0};
    g_assert_cmpint(tudor_usb_control(usb,G_USB_DEVICE_REQUEST_TYPE_VENDOR,
        0x14,0,data,2,NULL,&error),==,2);
    g_assert_no_error(error); g_assert_cmpuint(data[0],==,1);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,FALSE,0x81,data,2,2000,NULL,&error),==,1);
    g_assert_no_error(error); g_assert_cmpuint(data[0],==,2);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,FALSE,0x01,data,2,2000,NULL,&error),==,2);
    g_assert_no_error(error);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,TRUE,0x83,data,2,250,NULL,&error),==,-1);
    g_assert_error(error,G_USB_DEVICE_ERROR,G_USB_DEVICE_ERROR_TIMED_OUT);
    g_clear_error(&error);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,TRUE,0x83,data,2,250,NULL,&error),==,2);
    g_assert_no_error(error); g_assert_cmpuint(data[0],==,3);
    /* Cancellation must still deliver completion before stack buffers expire. */
    g_autoptr(GCancellable) cancel=g_cancellable_new();
    g_cancellable_cancel(cancel);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_control(usb,G_USB_DEVICE_REQUEST_TYPE_VENDOR,
        0x14,0,data,2,cancel,&error),==,-1);
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,FALSE,0x81,data,2,2000,cancel,&error),==,-1);
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,TRUE,0x84,data,2,250,cancel,&error),==,-1);
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    data[0]=data[1]=0;
    g_assert_cmpint(tudor_usb_transfer(usb,FALSE,0x82,data,2,2000,NULL,&error),==,-1);
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);
    g_main_context_pop_thread_default(context);
    return NULL;
}

int main(void)
{
    /* Deliberately skip GInitable: replay needs no libusb initialization or
     * hardware enumeration. context_load marks enumeration complete. */
    g_autoptr(GUsbContext) usb_context=g_object_new(G_USB_TYPE_CONTEXT,NULL);
    g_autoptr(JsonParser) parser=json_parser_new();
    g_autoptr(GError) error=NULL;
    const char *fixture="{\"UsbDevices\":[{\"PlatformId\":\"synthetic-tudor\",\"UsbEvents\":["
        "{\"Id\":\"ControlTransfer:Direction=0x00,RequestType=0x02,Recipient=0x00,Request=0x14,Value=0x0000,Idx=0x0000,Data=AAA=,Length=0x2\",\"Data\":\"AQA=\"},"
        "{\"Id\":\"BulkTransfer:Endpoint=0x81,Data=AAA=,Length=0x2\",\"Data\":\"Ag==\"},"
        "{\"Id\":\"BulkTransfer:Endpoint=0x01,Data=AAA=,Length=0x2\",\"Data\":\"AAA=\"},"
        "{\"Id\":\"InterruptTransfer:Endpoint=0x83,Data=AAA=,Length=0x2\",\"Status\":2},"
        "{\"Id\":\"InterruptTransfer:Endpoint=0x83,Data=AAA=,Length=0x2\",\"Data\":\"AwA=\"},"
        "{\"Id\":\"InterruptTransfer:Endpoint=0x84,Data=AAA=,Length=0x2\",\"Data\":\"AwA=\"}"
        "]}]}";
    g_assert(json_parser_load_from_data(parser,fixture,-1,&error));
    g_assert(g_usb_context_load(usb_context,json_node_get_object(json_parser_get_root(parser)),&error));
    g_assert_no_error(error);
    g_autoptr(GPtrArray) devices=g_usb_context_get_devices(usb_context);
    g_assert_cmpuint(devices->len,==,1);
    GUsbDevice *usb=g_ptr_array_index(devices,0);
    g_assert(g_usb_device_is_emulated(usb));
    /* Hold the application's context without dispatching it. The old GUsb
     * synchronous wrappers deadlock here; private async completions do not. */
    g_assert(g_main_context_acquire(g_main_context_default()));
    GThread *thread=g_thread_new("transport-replay",worker,usb);
    g_thread_join(thread);
    g_main_context_release(g_main_context_default());
    g_print("Real GUsb replay passed: control, bulk IN/OUT, short reply, interrupt, timeout, cancellation and missing event; application context never iterated.\n");
    return 0;
}
