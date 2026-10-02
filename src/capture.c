/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_core.h"
#include "frame_decode.h"
#include "image_quality.h"
#include "tls.h"
#include "usb.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/rand.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FRAME_WIDTH TUDOR_FRAME_WIDTH
#define FRAME_HEIGHT TUDOR_FRAME_HEIGHT
#define FRAME_BYTES TUDOR_FRAME_BYTES
struct capture_context { struct tudor_pairing_state state; const char *output_parent; int settled, memory, fd; };
static volatile sig_atomic_t cancelled;
static void cancel_capture(int signal_number) { (void)signal_number; cancelled = 1; }
static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static int confirm_settled_finger(void)
{
    fprintf(stderr, "Settled capture experiment: place your finger firmly and hold still.\n"
            "After one second, press Enter with your other hand (20-second limit; q cancels).\n");
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) return -1;
    while (!cancelled) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec-start.tv_sec >= 20) break;
        struct pollfd input = {.fd=STDIN_FILENO, .events=POLLIN};
        int ready = poll(&input, 1, 500);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (ready && !(input.revents & POLLIN))) break;
        if (!ready) continue;
        unsigned char answer;
        ssize_t n = read(STDIN_FILENO, &answer, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1 || answer != '\n') break;
        /* A short fixed delay is a timing experiment, not a quality guarantee. */
        struct timespec delay = {.tv_sec=0, .tv_nsec=300000000};
        while (!cancelled && nanosleep(&delay, &delay)) {
            if (errno != EINTR) return -1;
        }
        return cancelled ? -1 : 0;
    }
    fprintf(stderr, "Settled capture cancelled or confirmation timed out; no acquisition started.\n");
    return -1;
}

static int exchange(void *context, const uint8_t *request, size_t size, uint8_t *response, size_t capacity)
{
    struct capture_context *ctx=context;
    int n=tudor_exchange(ctx->fd,request,size,response,capacity);
    if (n<0) perror("Capture USB transfer");
    return n;
}

static int tls_status(void *context)
{
    return tudor_device_tls_status(((struct capture_context *)context)->fd);
}

static int interrupt(void *context, uint8_t response[8], unsigned timeout_ms)
{
    int n=tudor_device_interrupt(((struct capture_context *)context)->fd,response,timeout_ms);
    return n<0 && (errno==ETIMEDOUT || errno==EINTR) ? 0 : n;
}

static int is_cancelled(void *context) { (void)context; return cancelled!=0; }

static int prepare(void *context)
{
    struct capture_context *ctx=context;
    return ctx->settled && confirm_settled_finger() ? 1 : 0;
}

static void notify(void *context, enum tudor_capture_event event)
{
    struct capture_context *ctx=context;
    if (event==TUDOR_CAPTURE_READY)
        fprintf(stderr,"Frame-ready interrupt received. Reading one frame.\n");
    else fprintf(stderr,"%s\n",ctx->settled ?
        "Keep your finger in place while the frame is captured (up to 20 seconds)." :
        "Place one finger on the sensor now and hold it still (up to 20 seconds).");
}

static int write_private(int dir, const char *name, const void *data, size_t size)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    size_t offset = 0;
    int result = -1;
    while (offset < size) {
        ssize_t n = write(fd, p + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        offset += (size_t)n;
    }
    result = fsync(fd);
done:
    if (close(fd)) result = -1;
    return result;
}

static int save_frame(const char *parent_path, const uint8_t *response, size_t response_size)
{
    struct tudor_frame parsed;
    if (tudor_parse_frame(response, response_size, &parsed)) return -1;
    const uint8_t *frame = parsed.pixels;
    /* Capture artifacts are separate from the pairing directory, with fresh names. */
    int parent = open(parent_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (parent < 0 || fstat(parent, &st) || st.st_uid != geteuid() || (st.st_mode & 022)) {
        if (parent >= 0) close(parent);
        fprintf(stderr, "Output parent must be owned by this user and not writable by group/others.\n"); return -1;
    }
    uint8_t random[8];
    if (RAND_bytes(random, sizeof(random)) != 1) { close(parent); return -1; }
    char name[64];
    snprintf(name, sizeof(name), "tudor-native-frame-%02x%02x%02x%02x%02x%02x%02x%02x",
             random[0],random[1],random[2],random[3],random[4],random[5],random[6],random[7]);
    if (mkdirat(parent, name, 0700)) { close(parent); return -1; }
    int dir = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int result = -1;
    if (dir < 0) { close(parent); return -1; }
    int minimum = 32767, maximum = -32768;
    unsigned zeroes = 0;
    int64_t sum = 0;
    for (size_t i = 0; i < FRAME_BYTES; i += 2) {
        unsigned raw = le16(frame + i);
        int value = raw >= 32768 ? (int)raw - 65536 : (int)raw;
        if (value < minimum) minimum = value;
        if (value > maximum) maximum = value;
        if (!value) zeroes++;
        sum += value;
    }
    char metadata[1024];
    int n = snprintf(metadata, sizeof(metadata),
        "{\"width\":104,\"height\":86,\"pixel_bits\":16,\"raw_bytes\":%d,"
        "\"response_bytes\":%zu,\"pixel_offset\":10,\"length_field_verified\":true,"
        "\"frame_flags\":%u,\"frame_index\":%u,\"sample_encoding_candidate\":\"s16le\","
        "\"s16le_min\":%d,\"s16le_max\":%d,\"s16le_mean\":%.3f,"
        "\"zero_samples\":%u,\"image_decoding_validated\":false}\n",
        FRAME_BYTES, response_size, (unsigned)parsed.flags, (unsigned)parsed.index,
        minimum, maximum, (double)sum / (FRAME_WIDTH * FRAME_HEIGHT), zeroes);
    if (n < 0 || (size_t)n >= sizeof(metadata)) goto done;
    if (write_private(dir, "frame-response.bin", response, response_size) ||
        write_private(dir, "frame.raw", frame, FRAME_BYTES) ||
        write_private(dir, "metadata.json", metadata, (size_t)n) || fsync(dir) || fsync(parent)) goto done;
    /* Parent is not echoed as JSON: it is a caller-supplied path and can contain quotes. */
    fprintf(stderr, "Private capture saved under %s/%s (authenticated response, raw pixels, and metadata).\n", parent_path, name);
    printf("{\"authenticated_frame_response_saved\":true,\"raw_frame_received\":true,\"session_closed\":true,\"authentication_supported\":false,"
           "\"capture_directory_name\":\"%s\",\"frame_summary\":%.*s}\n", name, n - 1, metadata);
    result = 0;
done:
    close(dir); close(parent);
    if (result) fprintf(stderr, "Could not save complete capture; any partial files remain private.\n");
    return result;
}

static int capture_action(int fd, const struct tudor_version *version, void *opaque)
{
    struct capture_context *ctx=opaque;
    ctx->fd=fd;
    const struct tudor_capture_ops ops={.exchange=exchange,.tls_status=tls_status,
        .interrupt=interrupt,.cancelled=is_cancelled,.prepare=prepare,.notify=notify};
    struct tudor_capture_result frame;
    int result=tudor_capture_run(&ctx->state,version,&ops,ctx,&frame);
    if (!result && ctx->memory) {
        struct tudor_gray_frame gray;
        struct tudor_image_quality quality;
        result=tudor_frame_decode(frame.response,frame.response_size,&gray);
        if (!result) result=tudor_image_assess(gray.pixels,sizeof(gray.pixels),&quality);
        if (!result) {
            printf("{\"image_decoded_in_memory\":true,\"image_files_written\":false,"
                   "\"session_closed\":true,\"authentication_supported\":false,"
                   "\"width\":104,\"height\":86,\"quality_flags\":%u,"
                   "\"contrast_low\":%d,\"contrast_high\":%d}\n",
                   quality.flags,gray.contrast_low,gray.contrast_high);
            if (fflush(stdout)) result=1;
        } else fprintf(stderr,"Memory capture decoding failed; no image exported.\n");
        tudor_gray_frame_clear(&gray);
    } else if (!result) result=save_frame(ctx->output_parent,frame.response,frame.response_size);
    tudor_capture_result_clear(&frame);
    if (result) fprintf(stderr,"Capture stopped. Sensor TLS status=%d; pairing files unchanged.\n",tudor_device_tls_status(fd));
    return result ? 1 : 0;
}

int main(int argc, char **argv)
{
    int settled=0, memory=0, first=1;
    while (first<argc && !strncmp(argv[first],"--",2)) {
        if (!strcmp(argv[first],"--settled") && !settled) settled=1;
        else if (!strcmp(argv[first],"--memory") && !memory) memory=1;
        else goto usage;
        first++;
    }
    if (argc-first!=(memory ? 2 : 3)) goto usage;
    umask(077);
    struct sigaction action = {.sa_handler = cancel_capture};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 1;
    struct capture_context ctx = {.output_parent = memory ? NULL : argv[first+2], .settled=settled,.memory=memory};
    if (tudor_state_load(argv[first], argv[first+1], &ctx.state)) return 1;
    int result = tudor_device_run(capture_action, &ctx);
    tudor_state_clear(&ctx.state);
    return result;
usage:
    fprintf(stderr,"Usage: %s [--settled] EXISTING_PAIRING_DIRECTORY SENSOR_10_1_KF_TSK OUTPUT_PARENT\n"
                   "       %s --memory [--settled] EXISTING_PAIRING_DIRECTORY SENSOR_10_1_KF_TSK\n",argv[0],argv[0]);
    return 2;
}
