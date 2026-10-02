/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "capture_core.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1]<<8; }

static int64_t now_ms(const struct tudor_capture_ops *ops, void *context)
{
    if (ops->monotonic_ms) return ops->monotonic_ms(context);
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)) return -1;
    return (int64_t)now.tv_sec*1000 + now.tv_nsec/1000000;
}

static int command(struct tudor_tls *tls, const uint8_t *request, size_t size,
                   uint8_t *response, size_t capacity, const char *label)
{
    int n=tudor_tls_command(tls,request,size,response,capacity);
    if (n<0) { fprintf(stderr,"%s: encrypted response rejected.\n",label); return -1; }
    int status=tudor_status(response,(size_t)n);
    if (status) { fprintf(stderr,"%s rejected: status=%d (0x%04x) length=%d\n",label,status,(unsigned)status,n); return -1; }
    return n;
}

static int event_clear(struct tudor_tls *tls)
{
    uint8_t request[37]={0x86}, response[128];
    request[33]=4;
    int n=command(tls,request,sizeof(request),response,sizeof(response),"EVENT_CONFIG(clear)");
    OPENSSL_cleanse(response,sizeof(response));
    if (n!=66) { fprintf(stderr,"Unexpected EVENT_CONFIG length=%d\n",n); return -1; }
    return 0;
}

static int contact_config(struct tudor_tls *tls, unsigned mask, unsigned *sequence)
{
    uint8_t request[37]={0x86}, response[66];
    for (unsigned i=0;i<8;i++) request[1+4*i]=(uint8_t)mask;
    int n=command(tls,request,sizeof(request),response,sizeof(response),"EVENT_CONFIG(contact)");
    if (n!=66) { fprintf(stderr,"Contact configuration rejected: response bytes=%d (expected 66).\n",n); return -1; }
    *sequence=le16(response+64);
    return 0;
}

/* Reverse-engineered event format: six-byte reply header followed by 12-byte
 * entries. Modern pending count excludes returned entries; legacy includes them.
 * Only explicit 0x405..0x407 rejection permits the documented legacy fallback. */
static int contact_read(struct tudor_tls *tls, unsigned *sequence, int *legacy,
                        int *present, unsigned *pending, int *removed)
{
    *removed=0;
    uint8_t request[]={0x87,(uint8_t)*sequence,(uint8_t)(*sequence>>8),32,0,1,0,0,0};
    uint8_t response[390];
    int n=tudor_tls_command(tls,request,*legacy ? 5 : sizeof(request),response,sizeof(response));
    if (n<2) return -1;
    unsigned status=le16(response);
    if (!*legacy && status>=0x405 && status<=0x407) {
        *legacy=1;
        n=tudor_tls_command(tls,request,5,response,sizeof(response));
        if (n<2) return -1;
        status=le16(response);
    }
    if (status || n<6) {
        fprintf(stderr,"EVENT_READ rejected: status=0x%04x bytes=%d legacy=%d.\n",status,n,*legacy);
        return -1;
    }
    unsigned count=le16(response+2), left=le16(response+4);
    if (count>32 || (unsigned)n!=6+12*count || (*legacy && left<count)) {
        fprintf(stderr,"EVENT_READ layout rejected: bytes=%d events=%u pending=%u legacy=%d.\n",n,count,left,*legacy);
        return -1;
    }
    *pending=*legacy ? left-count : left;
    if (*pending>1024 || (!count && *pending)) return -1;
    for (unsigned i=0;i<count;i++) {
        unsigned type=response[6+12*i];
        if (type==1) *present=1;
        else if (type==2) { *present=0; *removed=1; }
    }
    *sequence=(*sequence+count)&0xffff;
    return 0;
}

static int contact_wait(struct tudor_tls *tls,const struct tudor_capture_ops *ops,
                        void *context,int need_clear,int *legacy)
{
    unsigned sequence=0,pending=0,reads=0;
    int present=-1,notified_present=0;
    int64_t start=now_ms(ops,context),last=start,stable=-1;
    if (start<0) return TUDOR_CAPTURE_IO;
    if (ops->notify) ops->notify(context,need_clear ? TUDOR_CAPTURE_NEED_LIFT : TUDOR_CAPTURE_NEED_TOUCH);
    if (ops->cancelled(context)) return TUDOR_CAPTURE_CANCELLED;
    if (contact_config(tls,need_clear ? 4u : 6u,&sequence)) return TUDOR_CAPTURE_PROTOCOL;
    for (;;) {
        if (ops->cancelled(context)) return TUDOR_CAPTURE_CANCELLED;
        int64_t now=now_ms(ops,context);
        if (now<last) return TUDOR_CAPTURE_IO;
        if (now-start>=20000) return TUDOR_CAPTURE_TIMEOUT;
        last=now;
        /* Bound malformed/storming backlogs as well as wall-clock duration. */
        if (++reads>256) return TUDOR_CAPTURE_PROTOCOL;
        int old=present;
        int removed=0;
        if (contact_read(tls,&sequence,legacy,&present,&pending,&removed)) return TUDOR_CAPTURE_PROTOCOL;
        if (ops->cancelled(context)) return TUDOR_CAPTURE_CANCELLED;
        now=now_ms(ops,context);
        if (now<last) return TUDOR_CAPTURE_IO;
        if (now-start>=20000) return TUDOR_CAPTURE_TIMEOUT;
        last=now;
        if (need_clear && present==0 && !pending) return TUDOR_CAPTURE_OK;
        if (!need_clear) {
            if (present!=1) {
                stable=-1;
                if (notified_present && ops->notify) ops->notify(context,TUDOR_CAPTURE_NEED_TOUCH);
                notified_present=0;
            } else {
                if (old!=1 || stable<0 || removed) stable=now;
                if (!notified_present && ops->notify) ops->notify(context,TUDOR_CAPTURE_SETTLING);
                notified_present=1;
                if (!pending && now-stable>=1000) return TUDOR_CAPTURE_OK;
            }
        }
        if (pending) continue;
        unsigned timeout=(unsigned)(20000-(now-start));
        if (!need_clear && stable>=0 && now-stable<1000 && timeout>(unsigned)(1000-(now-stable)))
            timeout=(unsigned)(1000-(now-stable));
        if (timeout>250) timeout=250;
        uint8_t event[8];
        int n=ops->interrupt(context,event,timeout);
        if (ops->cancelled(context)) return TUDOR_CAPTURE_CANCELLED;
        if (n && (n<7 || n>8)) return TUDOR_CAPTURE_IO;
        /* Poll EVENT_READ after each bounded wait, including timeouts. This
         * avoids assuming interrupt sequence bits describe current contact. */
    }
}

void tudor_capture_result_clear(struct tudor_capture_result *result)
{
    if (result) OPENSSL_cleanse(result,sizeof(*result));
}

int tudor_capture_run(const struct tudor_pairing_state *state,
                      const struct tudor_version *version,
                      const struct tudor_capture_ops *ops, void *context,
                      struct tudor_capture_result *output)
{
    if (!output) return TUDOR_CAPTURE_INVALID;
    tudor_capture_result_clear(output);
    if (!state || !state->identity || !state->sensor || !version || !ops ||
        !ops->exchange || !ops->tls_status || !ops->interrupt || !ops->cancelled)
        return TUDOR_CAPTURE_INVALID;
    if (version->major!=10 || version->minor!=1 || version->product!=65 ||
        version->provision!=3 || !version->advanced_security || !version->key_flag)
        return TUDOR_CAPTURE_PROTOCOL;
    if (ops->cancelled(context)) return TUDOR_CAPTURE_CANCELLED;
    if (ops->tls_status(context)!=0) return TUDOR_CAPTURE_IO;
    if (ops->cancelled(context)) return TUDOR_CAPTURE_CANCELLED;
    struct tudor_tls tls={0};
    uint8_t response[TUDOR_TLS_LIMIT], frame[TUDOR_FRAME_HEADER_SIZE+TUDOR_FRAME_BYTES];
    size_t frame_size=0;
    int status=TUDOR_CAPTURE_PROTOCOL, acquisition_attempted=0, events_configured=0;
    if (tudor_tls_open(&tls,state->identity,state->sensor,state->host_cert,ops->exchange,context)) goto done;
    if (ops->tls_status(context)!=1) { status=TUDOR_CAPTURE_IO; goto cleanup; }
    if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
    const uint8_t dimensions_request[]={0x82,0,0,0,0,0,0,2,7};
    int n=command(&tls,dimensions_request,sizeof(dimensions_request),response,sizeof(response),"FRAME_STATE_GET");
    const unsigned expected[]={16,104,0,0,104,86,0,0,86};
    if (n!=34) goto cleanup;
    for (size_t i=0;i<9;i++) if (le16(response+14+2*i)!=expected[i]) {
        fprintf(stderr,"Capture refused: dimensions differ from the observed 104x86x16-bit layout.\n"); goto cleanup;
    }
    if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
    if (ops->automatic_contact) {
        int legacy=0;
        events_configured=1;
        status=contact_wait(&tls,ops,context,1,&legacy);
        if (status!=TUDOR_CAPTURE_OK) goto cleanup;
        status=contact_wait(&tls,ops,context,0,&legacy);
        if (status!=TUDOR_CAPTURE_OK) goto cleanup;
        status=TUDOR_CAPTURE_PROTOCOL;
    }
    if (ops->prepare) {
        int prepared=ops->prepare(context);
        if (prepared) { status=prepared>0 ? TUDOR_CAPTURE_CANCELLED : TUDOR_CAPTURE_IO; goto cleanup; }
    }
    if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
    events_configured=1;
    if (event_clear(&tls)) goto cleanup;
    if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
    const uint8_t acquire[]={0x80,0,0,0,0,1,0,0,0,1,0,0,8,1,1,1,0};
    acquisition_attempted=1;
    n=command(&tls,acquire,sizeof(acquire),response,sizeof(response),"FRAME_ACQ");
    if (n!=2) goto cleanup;
    if (ops->notify) ops->notify(context,TUDOR_CAPTURE_WAITING);
    int64_t start=now_ms(ops,context);
    if (start<0) { status=TUDOR_CAPTURE_IO; goto cleanup; }
    for (;;) {
        if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
        int64_t now=now_ms(ops,context);
        if (now<start) { status=TUDOR_CAPTURE_IO; goto cleanup; }
        if (now-start>=20000) { status=TUDOR_CAPTURE_TIMEOUT; goto cleanup; }
        unsigned remaining=(unsigned)(20000-(now-start));
        uint8_t event[8];
        n=ops->interrupt(context,event,remaining<250 ? remaining : 250);
        if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
        now=now_ms(ops,context);
        if (now<start) { status=TUDOR_CAPTURE_IO; goto cleanup; }
        if (now-start>=20000) { status=TUDOR_CAPTURE_TIMEOUT; goto cleanup; }
        if (!n) continue;
        if (n<6 || n>8) { status=TUDOR_CAPTURE_IO; goto cleanup; }
        if ((event[5]&7)!=0) break;
    }
    if (ops->notify) ops->notify(context,TUDOR_CAPTURE_READY);
    if (ops->cancelled(context)) { status=TUDOR_CAPTURE_CANCELLED; goto cleanup; }
    const uint8_t read_frame[]={0x7f,0,0,0,0,0xff,0xff,3,0};
    n=command(&tls,read_frame,sizeof(read_frame),response,sizeof(response),"FRAME_READ");
    if (n<8) goto cleanup;
    fprintf(stderr,"FRAME_READ: bytes=%d flags=0x%04x index=%u\n",n,le16(response+2),le16(response+6));
    struct tudor_frame parsed;
    if (tudor_parse_frame(response,(size_t)n,&parsed)) {
        fprintf(stderr,"Frame rejected: size/length mismatch, incomplete frame, or finger lifted; no frame saved.\n"); goto cleanup;
    }
    fprintf(stderr,"FRAME_READ length verified: 17888 pixel bytes at offset 10.\n");
    frame_size=(size_t)n; memcpy(frame,response,frame_size);
    status=TUDOR_CAPTURE_OK;
cleanup:
    if (acquisition_attempted && !tls.failed) {
        const uint8_t finish=0x81;
        if (command(&tls,&finish,1,response,sizeof(response),"FRAME_FINISH")!=2)
            status=TUDOR_CAPTURE_CLEANUP;
        else fprintf(stderr,"FRAME_FINISH acknowledged; capture stop command completed.\n");
    }
    if (events_configured && !tls.failed && event_clear(&tls)) status=TUDOR_CAPTURE_CLEANUP;
    if (!tls.failed && tudor_tls_close(&tls)==0 && ops->tls_status(context)==0) {
        output->session_closed=1;
        if (ops->cancelled(context) && status==TUDOR_CAPTURE_OK) status=TUDOR_CAPTURE_CANCELLED;
        if (status==TUDOR_CAPTURE_OK) {
            memcpy(output->response,frame,frame_size); output->response_size=frame_size;
        } else fprintf(stderr,"Capture ended without a frame; TLS session closed.\n");
    } else if (status==TUDOR_CAPTURE_OK) status=TUDOR_CAPTURE_CLEANUP;
done:
    if (tls.attempted) tudor_tls_abort(&tls);
    OPENSSL_cleanse(response,sizeof(response)); OPENSSL_cleanse(frame,sizeof(frame));
    tudor_tls_clear(&tls);
    return status;
}
