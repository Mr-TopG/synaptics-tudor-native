/* SPDX-License-Identifier: MIT */
#include <fprint.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

static gboolean wait_for_enter(GCancellable *cancel)
{
    gint64 end=g_get_monotonic_time()+20*G_USEC_PER_SEC;
    while (!g_cancellable_is_cancelled(cancel) && g_get_monotonic_time()<end) {
        while (g_main_context_iteration(NULL,FALSE)) { }
        struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN};
        if (poll(&input,1,100)>0) {
            char answer=0;
            return (input.revents&POLLIN) && read(STDIN_FILENO,&answer,1)==1 && answer=='\n';
        }
    }
    return FALSE;
}

static gboolean cancel_signal(gpointer data)
{
    if (!g_cancellable_is_cancelled(data))
        g_printerr("Cancellation requested; waiting for USB completion and cleanup.\n");
    g_cancellable_cancel(data);
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    gboolean capture=argc==2 && !strcmp(argv[1],"--capture");
    if (!capture && !(argc==2 && !strcmp(argv[1],"--list"))) {
        fprintf(stderr,"Usage: %s --list | --capture\n",argv[0]); return 2;
    }
    if (capture) g_printerr("libfprint: discovering the native device.\n");
    g_autoptr(FpContext) context=fp_context_new();
    GPtrArray *devices=fp_context_get_devices(context);
    FpDevice *device=NULL;
    unsigned found=0;
    for (guint i=0;i<devices->len;i++) {
        FpDevice *candidate=g_ptr_array_index(devices,i);
        if (!strcmp(fp_device_get_driver(candidate),"tudor_native")) { device=candidate; found++; }
    }
    if (!capture) {
        printf("{\"driver\":\"tudor_native\",\"devices_found\":%u,\"authentication_supported\":false}\n",found);
        return found==1 ? 0 : 1;
    }
    if (found!=1) { fprintf(stderr,"Expected exactly one native Tudor device.\n"); return 1; }
    if (fp_device_has_feature(device,FP_DEVICE_FEATURE_VERIFY) ||
        fp_device_has_feature(device,FP_DEVICE_FEATURE_IDENTIFY)) {
        fprintf(stderr,"Unexpected authentication feature in capture-only build.\n"); return 1;
    }
    g_autoptr(GError) error=NULL;
    g_autoptr(GCancellable) cancel=g_cancellable_new();
    guint sigint=g_unix_signal_add(SIGINT,cancel_signal,cancel);
    guint sigterm=g_unix_signal_add(SIGTERM,cancel_signal,cancel);
    int result=1;
    guint width=0,height=0;
    gsize bytes=0;
    g_printerr("libfprint: opening the native device.\n");
    if (!fp_device_open_sync(device,cancel,&error)) {
        if (fp_device_is_open(device)) goto close;
        goto done;
    }
    fprintf(stderr,"Place your finger on the sensor and hold still for one second.\n"
        "Press Enter with your other hand to capture through libfprint (20-second limit; q cancels).\n");
    if (!wait_for_enter(cancel)) { fprintf(stderr,"Cancelled before acquisition.\n"); goto close; }
    g_usleep(300000);
    FpImage *image=fp_device_capture_sync(device,TRUE,cancel,&error);
    if (image) {
        fp_image_get_data(image,&bytes);
        width=fp_image_get_width(image); height=fp_image_get_height(image);
        g_object_unref(image); result=0;
    }
close:;
    GError *close_error=NULL;
    if (!fp_device_close_sync(device,NULL,&close_error)) {
        fprintf(stderr,"Close failed: %s\n",close_error->message);
        g_clear_error(&close_error); result=1;
    } else fprintf(stderr,"libfprint: device closed.\n");
done:
    if (error) fprintf(stderr,"libfprint operation failed: %s\n",error->message);
    if (!result) {
        printf("{\"libfprint_capture_succeeded\":true,\"driver\":\"tudor_native\","
            "\"width\":%u,\"height\":%u,\"image_bytes\":%zu,\"device_closed\":true,"
            "\"image_files_written\":false,\"authentication_supported\":false}\n",width,height,bytes);
        if (fflush(stdout)) result=1;
    }
    g_source_remove(sigint); g_source_remove(sigterm);
    return result;
}
