/* SPDX-License-Identifier: MIT */
/* In-memory capture against an independent Python encrypted peer, no USB. */
#define _POSIX_C_SOURCE 200809L
#include "capture_core.h"
#include "frame_decode.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct context { const char *mode; unsigned status_calls, ready_transfers;
    int cancelled, ready, contact_phase; int64_t clock; };
static int is_mode(struct context *c, const char *mode) { return !strcmp(c->mode,mode); }

static int transfer(void *buffer, size_t size, int writing)
{
    uint8_t *p=buffer;
    while (size) {
        ssize_t n=writing ? write(1,p,size) : read(0,p,size);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return -1;
        p+=n; size-=(size_t)n;
    }
    return 0;
}

static int exchange(void *opaque, const uint8_t *request, size_t size,
                    uint8_t *response, size_t capacity)
{
    struct context *c=opaque;
    if (size>65535) return -1;
    uint8_t header[]={0,0,(uint8_t)(size>>8),(uint8_t)size};
    if (transfer(header,4,1) || transfer((void *)request,size,1) || transfer(header,4,0)) return -1;
    size_t incoming=(size_t)header[0]<<24 | (size_t)header[1]<<16 | (size_t)header[2]<<8 | header[3];
    if (incoming>capacity || transfer(response,incoming,0)) return -1;
    if (c->ready) {
        c->ready_transfers++;
        if ((is_mode(c,"cancel-frame") && c->ready_transfers==1) ||
            (is_mode(c,"cancel-cleanup") && c->ready_transfers==2) ||
            (is_mode(c,"cancel-close") && c->ready_transfers==4)) c->cancelled=1;
    }
    return (int)incoming;
}

static int tls_status(void *opaque)
{
    struct context *c=opaque;
    c->status_calls++;
    if (is_mode(c,"busy")) return 1;
    if (c->status_calls==2) {
        if (is_mode(c,"cancel-handshake")) c->cancelled=1;
        return 1;
    }
    if (c->status_calls==3 && is_mode(c,"idle-failure")) return 1;
    return 0;
}
static int cancelled(void *opaque) { return ((struct context *)opaque)->cancelled; }
static int prepare(void *opaque)
{
    struct context *c=opaque;
    return is_mode(c,"cancel-prepare") ? 1 : is_mode(c,"prepare-error") ? -1 : 0;
}
static void notify(void *opaque, enum tudor_capture_event event)
{
    struct context *c=opaque;
    if (event==TUDOR_CAPTURE_NEED_LIFT) c->contact_phase=1;
    if (event==TUDOR_CAPTURE_NEED_TOUCH) c->contact_phase=2;
    if (event==TUDOR_CAPTURE_SETTLING) c->contact_phase=3;
    if ((event==TUDOR_CAPTURE_NEED_LIFT && is_mode(c,"contact-cancel-clear")) ||
        (event==TUDOR_CAPTURE_NEED_TOUCH && is_mode(c,"contact-cancel-touch")) ||
        (event==TUDOR_CAPTURE_SETTLING && is_mode(c,"contact-cancel-settle"))) c->cancelled=1;
    if (event==TUDOR_CAPTURE_WAITING) {
        if (!strncmp(c->mode,"contact-",8))
            assert(c->clock>= (is_mode(c,"contact-bounce") ? 2250 : 2000));
        c->contact_phase=4;
    }
    if (event==TUDOR_CAPTURE_WAITING && is_mode(c,"cancel-acquire")) c->cancelled=1;
    if (event==TUDOR_CAPTURE_READY) {
        c->ready=1;
        if (is_mode(c,"cancel-ready")) c->cancelled=1;
    }
}
static int interrupt(void *opaque, uint8_t response[8], unsigned timeout_ms)
{
    struct context *c=opaque;
    assert(timeout_ms>0 && timeout_ms<=250);
    if (!strncmp(c->mode,"contact-",8) && c->contact_phase<4) {
        c->clock+=timeout_ms;
        if (is_mode(c,"contact-late")) c->clock+=20001;
        if (is_mode(c,"contact-short-interrupt")) return 6;
        return 0;
    }
    if (is_mode(c,"timeout")) { c->clock+=timeout_ms; return 0; }
    if (is_mode(c,"cancel-wait")) { c->cancelled=1; return 0; }
    if (is_mode(c,"short-interrupt")) return 5;
    if (is_mode(c,"long-interrupt")) return 9;
    if (is_mode(c,"late-interrupt")) c->clock+=20001;
    memset(response,0,8); response[5]=1; return 8;
}
static int64_t clock_ms(void *opaque)
{
    struct context *c=opaque;
    if (is_mode(c,"clock-error")) return -1;
    if (is_mode(c,"clock-backwards")) return c->clock--;
    if (is_mode(c,"contact-clock-backwards") && c->contact_phase==3) return --c->clock;
    return c->clock;
}

int main(int argc, char **argv)
{
    if (argc!=4) return 2;
    struct context ctx={.mode=argv[1],.clock=1000};
    ctx.cancelled=is_mode(&ctx,"cancel-before");
    struct tudor_pairing_state state;
    if (tudor_state_load(argv[2],argv[3],&state)) return 2;
    struct tudor_version version={.major=10,.minor=1,.product=65,.provision=3,
        .advanced_security=1,.key_flag=1};
    struct tudor_capture_ops ops={.exchange=exchange,.tls_status=tls_status,
        .interrupt=interrupt,.cancelled=cancelled,.prepare=prepare,.notify=notify,.monotonic_ms=clock_ms};
    ops.automatic_contact=!strncmp(ctx.mode,"contact-",8);
    if (is_mode(&ctx,"invalid-callback")) ops.interrupt=NULL;
    if (is_mode(&ctx,"wrong-version")) version.product=64;
    struct tudor_capture_result result;
    memset(&result,0xa5,sizeof(result));
    int status=tudor_capture_run(&state,&version,&ops,&ctx,&result);
    struct tudor_gray_frame gray;
    if (status==0) {
        assert(result.session_closed && result.response_size==17898);
        struct tudor_frame frame;
        assert(!tudor_parse_frame(result.response,result.response_size,&frame));
        for (unsigned i=0;i<TUDOR_GRAY_BYTES;i++) {
            unsigned value=(unsigned)frame.pixels[i*2] | (unsigned)frame.pixels[i*2+1]<<8;
            assert(value==i);
        }
        assert(!tudor_frame_decode(result.response,result.response_size,&gray));
        assert(gray.contrast_low==89 && gray.contrast_high==8854);
        /* Independent expected row-major conversion from synthetic column ramp. */
        for (int y=0;y<86;y++) for (int x=0;x<104;x++) {
            int value=x*86+y;
            int expected=value<=89 ? 0 : value>=8854 ? 255 : (value-89)*255/(8854-89);
            assert(gray.pixels[y*104+x]==expected);
        }
        tudor_gray_frame_clear(&gray);
    } else {
        assert(result.response_size==0);
        for (size_t i=0;i<sizeof(result.response);i++) assert(result.response[i]==0);
    }
    fprintf(stderr,"CORE_RESULT status=%d closed=%d bytes=%zu\n",status,result.session_closed,result.response_size);
    tudor_capture_result_clear(&result);
    for (size_t i=0;i<sizeof(result);i++) assert(((unsigned char *)&result)[i]==0);
    tudor_state_clear(&state);
    return 0;
}
