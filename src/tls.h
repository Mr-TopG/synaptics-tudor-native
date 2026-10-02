/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_TLS_H
#define TUDOR_TLS_H
#include "certificate.h"
/* Bounded Tudor response capacity: the observed 104x86x16-bit frame is 17888
 * bytes before command/record framing. Tudor can exceed standard TLS record sizes. */
#define TUDOR_TLS_LIMIT 32768
/* Fully authenticated response did not fit the caller's buffer. No plaintext
 * is copied; traffic sequences remain synchronized so cleanup is possible. */
#define TUDOR_TLS_RESPONSE_TOO_LARGE (-2)
typedef int (*tudor_tls_exchange)(void *, const uint8_t *, size_t, uint8_t *, size_t);
struct tudor_tls {
    tudor_tls_exchange exchange;
    void *context;
    uint8_t client_random[32], server_random[32], master[48];
    uint8_t tx_key[32], rx_key[32], tx_salt[4], rx_salt[4];
    uint64_t tx_sequence, rx_sequence;
    uint8_t transcript[4096];
    size_t transcript_size;
    int keys_ready, established, attempted, failed;
};
int tudor_tls_prf(const uint8_t *secret, size_t secret_size, const char *label,
                   const uint8_t *seed, size_t seed_size, uint8_t *out, size_t size);
int tudor_tls_seal(const uint8_t key[32], const uint8_t salt[4], uint64_t *sequence,
                    uint8_t type, const uint8_t *data, size_t size, uint8_t *out, size_t capacity);
int tudor_tls_unseal(const uint8_t key[32], const uint8_t salt[4], uint64_t *sequence,
                      const uint8_t *record, size_t size, uint8_t *out, size_t capacity);
int tudor_tls_open(struct tudor_tls *s, EVP_PKEY *identity, EVP_PKEY *sensor,
                    const uint8_t host_cert[400], tudor_tls_exchange exchange, void *context);
int tudor_tls_command(struct tudor_tls *s, const uint8_t *command, size_t size,
                       uint8_t *response, size_t capacity);
int tudor_tls_close(struct tudor_tls *s);
/* Best-effort fatal alert after failure. Never sends an application command. */
void tudor_tls_abort(struct tudor_tls *s);
void tudor_tls_clear(struct tudor_tls *s);
#endif
