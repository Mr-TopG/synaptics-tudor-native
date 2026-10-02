/* SPDX-License-Identifier: MIT */
#include "tls.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Authenticated oversize is a caller error, not lost TLS synchronization.
 * Exercise a hardware-sized response and an encrypted cleanup command. */
struct large_peer { uint64_t tx, rx; int phase, fragment, tamper; };
static int large_exchange(void *context, const uint8_t *request, size_t size, uint8_t *response, size_t capacity)
{
    struct large_peer *peer = context;
    const uint8_t key[32] = {1}, salt[4] = {2};
    uint8_t input[16], payload[17898];
    int n = tudor_tls_unseal(key, salt, &peer->rx, request, size, input, sizeof(input));
    assert(n == 1 && input[0] == (peer->phase == 0 ? 0x7f : 0x81));
    memset(payload, 0x71, sizeof(payload));
    if (peer->phase++) {
        payload[0] = payload[1] = 0;
        return tudor_tls_seal(key, salt, &peer->tx, 23, payload, 2, response, capacity);
    }
    size_t first = peer->fragment ? 10000 : sizeof(payload);
    n = tudor_tls_seal(key, salt, &peer->tx, 23, payload, first, response, capacity);
    assert(n > 0);
    if (peer->fragment) {
        int second = tudor_tls_seal(key, salt, &peer->tx, 23, payload + first,
                sizeof(payload) - first, response + n, capacity - (size_t)n);
        assert(second > 0); n += second;
    }
    if (peer->tamper) response[n - 1] ^= 1;
    return n;
}

static void test_capacity_cleanup(int fragment, int tamper)
{
    struct large_peer peer = {.fragment=fragment, .tamper=tamper};
    struct tudor_tls tls = {.exchange=large_exchange, .context=&peer, .established=1};
    tls.tx_key[0] = tls.rx_key[0] = 1;
    tls.tx_salt[0] = tls.rx_salt[0] = 2;
    uint8_t out[17896], guard[17896], command = 0x7f;
    memset(guard, 0xa5, sizeof(guard)); memcpy(out, guard, sizeof(out));
    int n = tudor_tls_command(&tls, &command, 1, out, sizeof(out));
    assert(!memcmp(out, guard, sizeof(out)));
    if (tamper) {
        assert(n == -1 && tls.failed);
        command = 0x81;
        assert(tudor_tls_command(&tls, &command, 1, out, sizeof(out)) == -1);
        assert(peer.phase == 1); /* no cleanup traffic in a compromised session */
    } else {
        assert(n == TUDOR_TLS_RESPONSE_TOO_LARGE && !tls.failed);
        command = 0x81;
        assert(tudor_tls_command(&tls, &command, 1, out, sizeof(out)) == 2);
        assert(out[0] == 0 && out[1] == 0 && !tls.failed && peer.phase == 2);
        assert(tls.rx_sequence == (fragment ? 3U : 2U));
    }
    tudor_tls_clear(&tls);
}

int main(void)
{
    const uint8_t key[32] = {1}, salt[4] = {2}, message[] = {1,2,3,4,5};
    uint8_t record[128], changed[128], out[128], guard[128];
    memset(guard, 0xa5, sizeof(guard)); memcpy(out, guard, sizeof(out));
    uint64_t tx = 0, rx = 0;
    int n = tudor_tls_seal(key, salt, &tx, 23, message, sizeof(message), record, sizeof(record));
    assert(n == 34 && tx == 1);
    assert(tudor_tls_unseal(key, salt, &rx, record, (size_t)n, out, sizeof(out)) == 5 && rx == 1);
    assert(!memcmp(out, message, sizeof(message)));
    memcpy(out, guard, sizeof(out));
    assert(tudor_tls_unseal(key, salt, &rx, record, (size_t)n, out, sizeof(out)) < 0 && rx == 1);
    assert(!memcmp(out, guard, sizeof(out))); /* replay never releases plaintext */
    for (int i = 0; i < n; i++) {
        memcpy(changed, record, (size_t)n); changed[i] ^= 1; rx = 0;
        assert(tudor_tls_unseal(key, salt, &rx, changed, (size_t)n, out, sizeof(out)) < 0);
        assert(rx == 0 && !memcmp(out, guard, sizeof(out)));
    }
    for (int i = 0; i < n; i++) {
        rx = 0;
        assert(tudor_tls_unseal(key, salt, &rx, record, (size_t)i, out, sizeof(out)) < 0);
        assert(rx == 0 && !memcmp(out, guard, sizeof(out)));
    }
    rx = 0;
    assert(tudor_tls_unseal(key, salt, &rx, record, (size_t)n, out, 4) < 0 && rx == 0);
    tx = UINT64_MAX;
    assert(tudor_tls_seal(key, salt, &tx, 23, message, sizeof(message), record, sizeof(record)) < 0);
    assert(tudor_tls_close(NULL) < 0);
    test_capacity_cleanup(0, 0);
    test_capacity_cleanup(1, 0);
    test_capacity_cleanup(1, 1);
    puts("TLS record tests passed (tampering, replay, truncation, bounds, sequence exhaustion).");
    return 0;
}
