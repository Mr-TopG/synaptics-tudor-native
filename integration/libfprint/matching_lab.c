/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fprint.h>
#include <glib-unix.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static void wipe(void *data, gsize size)
{ volatile guint8 *p=data; while (size--) *p++=0; }

static gboolean cancel_signal(gpointer data)
{
    g_cancellable_cancel(data);
    g_printerr("Cancellation requested; waiting for cleanup.\n");
    return G_SOURCE_CONTINUE;
}
static gboolean prompt(GCancellable *cancel, gboolean enroll)
{
    g_printerr("Lift your finger. Place %s and hold still for one second.\n"
               "Press Enter with your other hand when settled (60 seconds; q cancels).\n",
               enroll ? "the SAME enrollment finger again" : "your chosen test finger");
    gint64 end=g_get_monotonic_time()+60*G_USEC_PER_SEC;
    while (!g_cancellable_is_cancelled(cancel) && g_get_monotonic_time()<end) {
        while (g_main_context_iteration(NULL,FALSE)) { }
        if (g_cancellable_is_cancelled(cancel)) return FALSE;
        struct pollfd input={.fd=STDIN_FILENO,.events=POLLIN};
        int ready=poll(&input,1,100);
        if (ready<0 && errno!=EINTR) break;
        if (ready>0) {
            char ch=0;
            if ((input.revents&POLLIN) && read(STDIN_FILENO,&ch,1)==1 && ch=='\n') return TRUE;
            break;
        }
    }
    g_cancellable_cancel(cancel); return FALSE;
}
static void progress(FpDevice *device, gint completed, FpPrint *print, gpointer opaque, GError *error)
{
    (void)device; (void)print;
    GCancellable *cancel=opaque;
    if (error) g_printerr("Scan rejected: %s\n",error->message);
    g_printerr("Enrollment: %d/10 accepted.\n",completed);
    if (completed<10 && g_strcmp0(g_getenv("TUDOR_NATIVE_AUTOMATIC_CONTACT"),"1")) prompt(cancel,TRUE);
}

static FpPrint *load_print(const char *path, GError **error)
{
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    if (fd<0) goto failed;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
        (st.st_mode&077)!=0 || st.st_nlink!=1 || st.st_size<1 || st.st_size>1024*1024) {
        close(fd); goto failed;
    }
    gsize size=(gsize)st.st_size, done=0;
    guint8 *data=g_malloc(size);
    while (done<size) {
        ssize_t n=read(fd,data+done,size-done);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        done+=(gsize)n;
    }
    guint8 extra;
    ssize_t tail=read(fd,&extra,1);
    close(fd);
    FpPrint *print=NULL;
    if (done==size && tail==0) print=fp_print_deserialize(data,size,error);
    wipe(data,size); g_free(data);
    if (print || (error && *error)) return print;
failed:
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA,
        "Template must be a private, owned, regular single-link file (maximum 1 MiB)");
    return NULL;
}

static gboolean save_print(FpPrint *print, const char *path, GError **error)
{
    guint8 *data=NULL; gsize size=0,done=0;
    if (!fp_print_serialize(print,&data,&size,error)) return FALSE;
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) { wipe(data,size); g_free(data); goto failed; }
    while (done<size) {
        ssize_t n=write(fd,data+done,size-done);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break;
        done+=(gsize)n;
    }
    gboolean ok=done==size && fsync(fd)==0;
    if (close(fd)) ok=FALSE;
    wipe(data,size); g_free(data);
    if (ok) return TRUE;
    unlink(path); /* Only the fresh file created by this invocation. */
failed:
    g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Could not save a new private template; existing files are never overwritten");
    return FALSE;
}

int main(int argc, char **argv)
{
    gboolean list=argc==2 && !strcmp(argv[1],"--list");
    gboolean enroll=argc==3 && !strcmp(argv[1],"enroll");
    if (!list && (argc!=3 || (!enroll && strcmp(argv[1],"verify")) || !g_path_is_absolute(argv[2]))) {
        g_printerr("Usage: %s --list | enroll|verify /absolute/private/template-path\n",argv[0]); return 2;
    }
    umask(077);
    g_autoptr(GError) error=NULL;
    g_autoptr(FpPrint) print=NULL;
    if (!list && !enroll && !(print=load_print(argv[2],&error))) {
        g_printerr("%s\n",error->message); return 1;
    }
    g_autoptr(FpContext) context=fp_context_new();
    GPtrArray *devices=fp_context_get_devices(context);
    FpDevice *device=NULL;
    for (guint i=0;i<devices->len;i++) {
        FpDevice *d=g_ptr_array_index(devices,i);
        if (!strcmp(fp_device_get_driver(d),"tudor_native_lab")) {
            if (device) { g_printerr("Expected exactly one lab sensor.\n"); return 1; }
            device=d;
        }
    }
    if (list) {
        printf("{\"driver\":\"tudor_native_lab\",\"devices_found\":%u,\"system_authentication_supported\":false}\n",device ? 1u : 0u);
        return device ? 0 : 1;
    }
    if (!device) { g_printerr("No tudor_native_lab sensor found.\n"); return 1; }
    g_autoptr(GCancellable) cancel=g_cancellable_new();
    guint sigint=g_unix_signal_add(SIGINT,cancel_signal,cancel);
    guint sigterm=g_unix_signal_add(SIGTERM,cancel_signal,cancel);
    gboolean ok=FALSE,matched=FALSE;
    g_autoptr(FpPrint) result=NULL;
    if (!fp_device_open_sync(device,cancel,&error)) goto close;
    if (enroll) {
        print=fp_print_new(device); g_object_ref_sink(print);
        result=fp_device_enroll_sync(device,print,cancel,progress,cancel,&error);
        ok=result!=NULL;
    } else {
        g_printerr("Verification: use the enrolled finger or a deliberately different test finger.\n");
        if (!g_strcmp0(g_getenv("TUDOR_NATIVE_AUTOMATIC_CONTACT"),"1") || prompt(cancel,FALSE))
            ok=fp_device_verify_sync(device,print,cancel,NULL,NULL,&matched,NULL,&error);
    }
close:
    if (fp_device_is_open(device)) {
        GError *close_error=NULL;
        if (!fp_device_close_sync(device,NULL,&close_error)) {
            g_printerr("Close failed: %s\n",close_error->message); g_clear_error(&close_error); ok=FALSE;
        } else g_printerr("libfprint: device closed.\n");
    }
    if (g_cancellable_is_cancelled(cancel)) ok=FALSE;
    if (ok && enroll) ok=save_print(result,argv[2],&error);
    if (error) g_printerr("libfprint lab: %s\n",error->message);
    if (ok && enroll) {
        g_printerr("Private ten-scan template saved: %s\n",argv[2]);
        puts("{\"libfprint_enrollment_succeeded\":true,\"accepted_scans\":10,\"system_authentication_supported\":false}");
    } else if (ok) {
        printf("{\"libfprint_verification_completed\":true,\"experimental_candidate_match\":%s,\"system_authentication_supported\":false}\n",matched ? "true" : "false");
    } else if (error && error->domain==FP_DEVICE_RETRY) {
        puts("{\"libfprint_verification_completed\":false,\"retry\":true,\"system_authentication_supported\":false}");
    }
    g_source_remove(sigint); g_source_remove(sigterm);
    return ok ? 0 : 1;
}
