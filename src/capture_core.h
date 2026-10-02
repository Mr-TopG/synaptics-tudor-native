/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_CAPTURE_CORE_H
#define TUDOR_CAPTURE_CORE_H
#include "state.h"
#include "tls.h"
#include "protocol.h"

enum tudor_capture_status {
    TUDOR_CAPTURE_OK=0, TUDOR_CAPTURE_CANCELLED=1, TUDOR_CAPTURE_TIMEOUT=2,
    TUDOR_CAPTURE_IO=3, TUDOR_CAPTURE_PROTOCOL=4, TUDOR_CAPTURE_CLEANUP=5,
    TUDOR_CAPTURE_INVALID=-1
};
enum tudor_capture_event { TUDOR_CAPTURE_WAITING, TUDOR_CAPTURE_READY,
    TUDOR_CAPTURE_NEED_LIFT, TUDOR_CAPTURE_NEED_TOUCH, TUDOR_CAPTURE_SETTLING };
struct tudor_capture_ops {
    tudor_tls_exchange exchange;
    int (*tls_status)(void *context); /* 0 idle, 1 active, negative error */
    /* Returns received byte count, 0 for timeout/interrupted wait, negative error.
     * Implementations must respect timeout_ms, including device disappearance. */
    int (*interrupt)(void *context, uint8_t response[8], unsigned timeout_ms);
    int (*cancelled)(void *context);
    /* Optional preparation before acquisition: 0 ready, positive cancelled,
     * negative error. Legacy CLI uses this for its settled-contact prompt. */
    int (*prepare)(void *context);
    void (*notify)(void *context, enum tudor_capture_event event);
    /* Optional clock override for tests. Negative means failure. */
    int64_t (*monotonic_ms)(void *context);
    /* Experimental sensor events: require clear sensor, fresh touch and one
     * second without a removal event before acquiring. Disabled by default. */
    int automatic_contact;
};
struct tudor_capture_result {
    int session_closed;
    size_t response_size;
    uint8_t response[TUDOR_FRAME_HEADER_SIZE + TUDOR_FRAME_BYTES];
};
/* Blocking, transport-independent capture for worker-thread adapters. The caller
 * owns a claimed, identity-checked device and validated pairing for the entire
 * call. Do not call on a libfprint/GLib main-loop thread. No USB discovery,
 * terminal input, pairing loads/writes, image files, reset or authentication.
 * Callbacks run only on the calling thread. Required callbacks: exchange,
 * tls_status, interrupt, cancelled. Each exchange/status callback MUST impose
 * finite I/O timeouts; cancellation is checked between protocol phases (a TLS
 * handshake can contain several exchanges). Never interrupt a partial record.
 * Cleanup ignores cancellation so FRAME_FINISH/TLS close can still run.
 * automatic_contact adds two bounded event phases (20s each): confirm removal,
 * then confirm a touch stable for 1s. Explicit legacy EVENT_READ fallback is
 * allowed; malformed replies, storms, cancellation and failed cleanup fail closed.
 * Interrupt waits are <=250ms with a 20s acquisition deadline. An image is
 * published only after successful cleanup, idle TLS status, and final cancel
 * check. Every failure clears image output; session_closed describes cleanup.
 * Inputs and output must not overlap. Serialize access to a physical device.
 */
int tudor_capture_run(const struct tudor_pairing_state *state,
                      const struct tudor_version *version,
                      const struct tudor_capture_ops *ops, void *context,
                      struct tudor_capture_result *output);
void tudor_capture_result_clear(struct tudor_capture_result *result);
#endif
