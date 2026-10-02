/* SPDX-License-Identifier: MIT */
#include "tls.h"
#include <openssl/core_names.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void be16(uint8_t *p, size_t n) { p[0] = (uint8_t)(n >> 8); p[1] = (uint8_t)n; }
static void be24(uint8_t *p, size_t n) { p[0] = (uint8_t)(n >> 16); be16(p + 1, n); }
static size_t get16(const uint8_t *p) { return (size_t)p[0] << 8 | p[1]; }
static size_t get24(const uint8_t *p) { return (size_t)p[0] << 16 | get16(p + 1); }
static void be64(uint8_t *p, uint64_t n)
{
    for (unsigned i = 0; i < 8; i++) p[7 - i] = (uint8_t)(n >> (8 * i));
}

int tudor_tls_prf(const uint8_t *secret, size_t secret_size, const char *label,
                   const uint8_t *seed, size_t seed_size, uint8_t *out, size_t size)
{
    uint8_t combined[256];
    if (!secret || !secret_size || !label || !seed || !out || !size ||
        strlen(label) > sizeof(combined) || seed_size > sizeof(combined) - strlen(label)) return -1;
    size_t label_size = strlen(label);
    memcpy(combined, label, label_size); memcpy(combined + label_size, seed, seed_size);
    char digest[] = "SHA384";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, (void *)secret, secret_size),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SEED, combined, label_size + seed_size),
        OSSL_PARAM_construct_end()
    };
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "TLS1-PRF", NULL);
    EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    int ok = ctx && EVP_KDF_derive(ctx, out, size, parameters) == 1;
    EVP_KDF_CTX_free(ctx); EVP_KDF_free(kdf);
    OPENSSL_cleanse(combined, sizeof(combined));
    return ok ? 0 : -1;
}

int tudor_tls_seal(const uint8_t key[32], const uint8_t salt[4], uint64_t *sequence,
                    uint8_t type, const uint8_t *data, size_t size, uint8_t *out, size_t capacity)
{
    if (!key || !salt || !sequence || !data || !out || size > TUDOR_TLS_LIMIT - 29 ||
        capacity < size + 29 || *sequence == UINT64_MAX || type < 21 || type > 23) return -1;
    uint8_t nonce[12], aad[13];
    out[0] = type; out[1] = 3; out[2] = 3; be16(out + 3, size + 24);
    /* Unique explicit nonce per traffic key; this need not be random. */
    be64(out + 5, *sequence); memcpy(nonce, salt, 4); memcpy(nonce + 4, out + 5, 8);
    be64(aad, *sequence); memcpy(aad + 8, out, 3); be16(aad + 11, size);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, final = 0;
    int ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, NULL, &n, aad, sizeof(aad)) == 1 &&
        EVP_EncryptUpdate(ctx, out + 13, &n, data, (int)size) == 1 && (size_t)n == size &&
        EVP_EncryptFinal_ex(ctx, out + 13 + size, &final) == 1 && final == 0 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out + 13 + size) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) { OPENSSL_cleanse(out, size + 29); return -1; }
    (*sequence)++;
    return (int)(size + 29);
}

int tudor_tls_unseal(const uint8_t key[32], const uint8_t salt[4], uint64_t *sequence,
                      const uint8_t *record, size_t size, uint8_t *out, size_t capacity)
{
    if (!key || !salt || !sequence || !record || !out || size < 29 || size > TUDOR_TLS_LIMIT ||
        size - 29 > capacity || *sequence == UINT64_MAX || record[1] != 3 || record[2] != 3 ||
        get16(record + 3) != size - 5 || record[0] < 21 || record[0] > 23) return -1;
    size_t plain_size = size - 29;
    uint8_t nonce[12], aad[13], temporary[TUDOR_TLS_LIMIT];
    memcpy(nonce, salt, 4); memcpy(nonce + 4, record + 5, 8);
    be64(aad, *sequence); memcpy(aad + 8, record, 3); be16(aad + 11, plain_size);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, final = 0;
    int ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, key, nonce) == 1 &&
        EVP_DecryptUpdate(ctx, NULL, &n, aad, sizeof(aad)) == 1 &&
        EVP_DecryptUpdate(ctx, temporary, &n, record + 13, (int)plain_size) == 1 &&
        (size_t)n == plain_size &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)(record + 13 + plain_size)) == 1 &&
        EVP_DecryptFinal_ex(ctx, temporary + plain_size, &final) == 1 && final == 0;
    EVP_CIPHER_CTX_free(ctx);
    if (ok) { memcpy(out, temporary, plain_size); (*sequence)++; }
    OPENSSL_cleanse(temporary, sizeof(temporary));
    return ok ? (int)plain_size : -1;
}

static int transcript_add(struct tudor_tls *s, const uint8_t *data, size_t size)
{
    if (size > sizeof(s->transcript) - s->transcript_size) return -1;
    memcpy(s->transcript + s->transcript_size, data, size); s->transcript_size += size;
    return 0;
}

static int handshake(struct tudor_tls *s, uint8_t type, const uint8_t *body,
                       size_t size, uint8_t *out, size_t capacity)
{
    if (size > TUDOR_TLS_LIMIT - 4 || capacity < size + 4) return -1;
    out[0] = type; be24(out + 1, size); memcpy(out + 4, body, size);
    /* Sensor hashes SHA-256 and omits Finished messages even for SHA-384 suite. */
    if (type != 20 && transcript_add(s, out, size + 4)) return -1;
    return (int)(size + 4);
}

static int plain_record(uint8_t type, const uint8_t *body, size_t size, uint8_t *out, size_t capacity)
{
    if (size > TUDOR_TLS_LIMIT - 5 || capacity < size + 5) return -1;
    out[0] = type; out[1] = 3; out[2] = 3; be16(out + 3, size); memcpy(out + 5, body, size);
    return (int)(size + 5);
}

static int exchange_handshake(struct tudor_tls *s, const uint8_t *records, size_t size, uint8_t *response)
{
    uint8_t request[TUDOR_TLS_LIMIT] = {0x44, 0, 0, 0};
    if (size > sizeof(request) - 4) return -1;
    memcpy(request + 4, records, size);
    s->attempted = 1;
    return s->exchange(s->context, request, size + 4, response, TUDOR_TLS_LIMIT);
}

static int record_size(const uint8_t *p, size_t remaining)
{
    if (remaining < 5 || p[1] != 3 || p[2] != 3 || get16(p + 3) > remaining - 5) return -1;
    return (int)(5 + get16(p + 3));
}

static int server_flight(struct tudor_tls *s, const uint8_t *response, size_t size)
{
    uint8_t messages[TUDOR_TLS_LIMIT];
    size_t total = 0;
    /* Reassemble handshake messages across records in this USB response. */
    for (size_t offset = 0; offset < size;) {
        int n = record_size(response + offset, size - offset);
        if (n < 0) return -1;
        const uint8_t *r = response + offset;
        if (r[0] == 21 && n == 7) {
            fprintf(stderr, "Sensor TLS alert: level=%u description=%u\n", r[5], r[6]); return -1;
        }
        if (r[0] != 22 || (size_t)n - 5 > sizeof(messages) - total) return -1;
        memcpy(messages + total, r + 5, (size_t)n - 5); total += (size_t)n - 5; offset += (size_t)n;
    }
    unsigned phase = 0;
    for (size_t offset = 0; offset < total;) {
        if (total - offset < 4) return -1;
        const uint8_t *m = messages + offset, *body = m + 4;
        size_t n = get24(m + 1);
        if (n > total - offset - 4) return -1;
        fprintf(stderr, "TLS server handshake: type=%u bytes=%zu\n", m[0], n);
        if (phase == 0 && m[0] == 2) {
            if (n < 38) { fprintf(stderr, "TLS: truncated ServerHello.\n"); return -1; }
            fprintf(stderr, "TLS ServerHello: version=0x%04zx session_id_bytes=%u\n", get16(body), body[34]);
            /* Related native Tudor implementations document 0x0383 in the hello
             * body. Records remain 0x0303. Keep the original bytes in the signed
             * transcript; accept only this specific variant, not arbitrary TLS versions. */
            if ((get16(body) != 0x0303 && get16(body) != 0x0383) || body[34] > 32) return -1;
            size_t cursor = 35 + body[34];
            if (n < cursor + 3) return -1;
            fprintf(stderr, "TLS ServerHello: cipher=0x%04zx compression=0x%02x trailing_bytes=%zu\n",
                    get16(body + cursor), body[cursor + 2], n - cursor - 3);
            if (get16(body + cursor) != 0xc02e || body[cursor + 2] != 0) return -1;
            memcpy(s->server_random, body + 2, 32); cursor += 3;
            /* Tudor omits the outer extensions length, unlike standard TLS. */
            while (cursor < n) {
                if (n - cursor < 4 || get16(body + cursor + 2) > n - cursor - 4) return -1;
                cursor += 4 + get16(body + cursor + 2);
            }
        } else if (phase == 1 && m[0] == 13) {
            if (n < 4 || body[0] == 0 || n != (size_t)body[0] + 3 ||
                !memchr(body + 1, 64, body[0])) return -1;
        } else if (phase == 2 && m[0] == 14) {
            if (n != 0) return -1;
        } else return -1;
        if (transcript_add(s, m, n + 4)) return -1;
        phase++; offset += n + 4;
    }
    return phase == 3 ? 0 : -1;
}

static int finished_data(struct tudor_tls *s, const char *label, uint8_t out[12])
{
    uint8_t hash[32]; unsigned size = 0;
    if (EVP_Digest(s->transcript, s->transcript_size, hash, &size, EVP_sha256(), NULL) != 1 || size != 32) return -1;
    return tudor_tls_prf(s->master, sizeof(s->master), label, hash, sizeof(hash), out, 12);
}

static int derive_keys(struct tudor_tls *s, EVP_PKEY *ephemeral, EVP_PKEY *sensor)
{
    uint8_t premaster[32], randoms[64], block[72];
    size_t size = sizeof(premaster);
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(ephemeral, NULL);
    int ok = ctx && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, sensor) == 1 &&
        EVP_PKEY_derive(ctx, premaster, &size) == 1 && size == 32;
    EVP_PKEY_CTX_free(ctx);
    memcpy(randoms, s->client_random, 32); memcpy(randoms + 32, s->server_random, 32);
    /* Tudor uses client||server for BOTH PRFs (standard TLS swaps key expansion). */
    if (ok) ok = tudor_tls_prf(premaster, size, "master secret", randoms, 64, s->master, 48) == 0 &&
        tudor_tls_prf(s->master, 48, "key expansion", randoms, 64, block, sizeof(block)) == 0;
    if (ok) {
        memcpy(s->tx_key, block, 32); memcpy(s->rx_key, block + 32, 32);
        memcpy(s->tx_salt, block + 64, 4); memcpy(s->rx_salt, block + 68, 4);
    }
    OPENSSL_cleanse(premaster, sizeof(premaster)); OPENSSL_cleanse(block, sizeof(block));
    return ok ? 0 : -1;
}

int tudor_tls_open(struct tudor_tls *s, EVP_PKEY *identity, EVP_PKEY *sensor,
                    const uint8_t host_cert[400], tudor_tls_exchange exchange, void *context)
{
    if (!s) return -1;
    memset(s, 0, sizeof(*s)); s->exchange = exchange; s->context = context;
    if (!identity || !sensor || !host_cert || !exchange ||
        tudor_certificate_matches(host_cert, 400, identity)) return -1;
    uint8_t body[1024], messages[2048], records[TUDOR_TLS_LIMIT], response[TUDOR_TLS_LIMIT];
    EVP_PKEY *ephemeral = NULL;
    int result = -1, n;
    uint32_t now = (uint32_t)time(NULL);
    s->client_random[0] = (uint8_t)(now >> 24); s->client_random[1] = (uint8_t)(now >> 16);
    s->client_random[2] = (uint8_t)(now >> 8); s->client_random[3] = (uint8_t)now;
    if (RAND_bytes(s->client_random + 4, 28) != 1) goto done;
    body[0] = 3; body[1] = 3; memcpy(body + 2, s->client_random, 32);
    body[34] = 7; memset(body + 35, 0, 7);
    const uint8_t options[] = {0,2,0xc0,0x2e, 0, 0,10,0,4,0,2,0,23, 0,11,0,2,1,0};
    memcpy(body + 42, options, sizeof(options));
    n = handshake(s, 1, body, 42 + sizeof(options), messages, sizeof(messages));
    if (n < 0 || (n = plain_record(22, messages, (size_t)n, records, sizeof(records))) < 0) goto done;
    fprintf(stderr, "TLS: sending ClientHello (P-256 / AES-256-GCM / SHA-384).\n");
    n = exchange_handshake(s, records, (size_t)n, response);
    if (n <= 0 || n > TUDOR_TLS_LIMIT || server_flight(s, response, (size_t)n)) {
        fprintf(stderr, "TLS: first server flight rejected.\n"); goto done;
    }
    /* Nonstandard certificate wrapper: duplicated length and two unused bytes. */
    be24(body, 400); be24(body + 3, 400); body[6] = body[7] = 0;
    memcpy(body + 8, host_cert, 400);
    n = handshake(s, 11, body, 408, messages, sizeof(messages));
    if (n < 0) goto done;
    size_t used = (size_t)n;
    ephemeral = tudor_key_generate();
    size_t point_size = 65;
    if (!ephemeral || EVP_PKEY_get_octet_string_param(ephemeral, OSSL_PKEY_PARAM_PUB_KEY,
        body, 65, &point_size) != 1 || point_size != 65 || body[0] != 4) goto done;
    n = handshake(s, 16, body, 65, messages + used, sizeof(messages) - used);
    if (n < 0) goto done;
    used += (size_t)n;
    EVP_MD_CTX *sign = EVP_MD_CTX_new();
    size_t signature_size = sizeof(body);
    int signed_ok = sign && EVP_DigestSignInit(sign, NULL, EVP_sha256(), NULL, identity) == 1 &&
        EVP_DigestSign(sign, body, &signature_size, s->transcript, s->transcript_size) == 1;
    EVP_MD_CTX_free(sign);
    if (!signed_ok) goto done;
    n = handshake(s, 15, body, signature_size, messages + used, sizeof(messages) - used);
    if (n < 0) goto done;
    used += (size_t)n;
    n = plain_record(22, messages, used, records, sizeof(records));
    if (n < 0 || derive_keys(s, ephemeral, sensor)) goto done;
    used = (size_t)n;
    const uint8_t ccs[] = {20,3,3,0,1,1};
    if (used + sizeof(ccs) > sizeof(records)) goto done;
    memcpy(records + used, ccs, sizeof(ccs)); used += sizeof(ccs);
    if (finished_data(s, "client finished", body)) goto done;
    n = handshake(s, 20, body, 12, messages, sizeof(messages));
    if (n < 0) goto done;
    n = tudor_tls_seal(s->tx_key, s->tx_salt, &s->tx_sequence, 22, messages, (size_t)n,
                        records + used, sizeof(records) - used);
    if (n < 0) goto done;
    used += (size_t)n; s->keys_ready = 1;
    fprintf(stderr, "TLS: sending host certificate, key exchange, and Finished proof.\n");
    n = exchange_handshake(s, records, used, response);
    if (n <= 0 || n > TUDOR_TLS_LIMIT) goto done;
    size_t size = (size_t)n, offset = 0, finished_size = 0;
    int saw_ccs = 0;
    while (offset < size) {
        const uint8_t *r = response + offset;
        n = record_size(r, size - offset);
        if (n < 0) goto done;
        if (!saw_ccs) {
            if ((size_t)n != sizeof(ccs) || memcmp(r, ccs, sizeof(ccs))) goto done;
            saw_ccs = 1;
        } else {
            if (r[0] != 22 || finished_size >= 16) goto done;
            int bytes = tudor_tls_unseal(s->rx_key, s->rx_salt, &s->rx_sequence, r, (size_t)n,
                                         messages + finished_size, 16 - finished_size);
            if (bytes < 0) goto done;
            finished_size += (size_t)bytes;
        }
        offset += (size_t)n;
    }
    if (!saw_ccs || finished_size != 16 || messages[0] != 20 || get24(messages + 1) != 12 ||
        finished_data(s, "server finished", body) || CRYPTO_memcmp(body, messages + 4, 12)) {
        fprintf(stderr, "TLS: server Finished proof rejected.\n"); goto done;
    }
    s->established = 1; result = 0;
    fprintf(stderr, "TLS: sensor Finished proof verified.\n");
done:
    EVP_PKEY_free(ephemeral);
    OPENSSL_cleanse(body, sizeof(body));
    if (result) s->failed = 1;
    return result;
}

static int encrypted_exchange(struct tudor_tls *s, uint8_t type, const uint8_t *data,
                               size_t size, uint8_t expected_type, uint8_t *out, size_t capacity)
{
    if (!s || !s->established || s->failed || !out || !capacity) return -1;
    uint8_t record[TUDOR_TLS_LIMIT], response[TUDOR_TLS_LIMIT], plain[TUDOR_TLS_LIMIT];
    const char *failure = "request encryption";
    size_t total = 0, used = 0, offset = 0;
    int n = tudor_tls_seal(s->tx_key, s->tx_salt, &s->tx_sequence, type, data, size, record, sizeof(record));
    if (n < 0) goto failed;
    failure = "USB response length";
    n = s->exchange(s->context, record, (size_t)n, response, sizeof(response));
    if (n <= 0 || n > TUDOR_TLS_LIMIT) goto failed;
    total = (size_t)n;
    for (size_t off = 0; off < total;) {
        offset = off;
        failure = "record framing/version";
        n = record_size(response + off, total - off);
        if (n < 0) goto failed;
        failure = "unexpected record type";
        if (response[off] != expected_type) goto failed;
        failure = "record authentication/bounds";
        int bytes = tudor_tls_unseal(s->rx_key, s->rx_salt, &s->rx_sequence, response + off, (size_t)n,
                                     plain + used, sizeof(plain) - used);
        if (bytes < 0) goto failed;
        used += (size_t)bytes; off += (size_t)n;
    }
    if (used > capacity) {
        OPENSSL_cleanse(plain, sizeof(plain));
        fprintf(stderr, "TLS: authenticated response exceeds application capacity: bytes=%zu capacity=%zu; session usable for cleanup.\n", used, capacity);
        return TUDOR_TLS_RESPONSE_TOO_LARGE;
    }
    memcpy(out, plain, used); OPENSSL_cleanse(plain, sizeof(plain));
    return (int)used;
failed:
    s->failed = 1; OPENSSL_cleanse(plain, sizeof(plain));
    fprintf(stderr, "TLS exchange failure: stage=%s received=%zu offset=%zu authenticated_bytes=%zu capacity=%zu result=%d\n",
            failure, total, offset, used, capacity, n);
    if (total >= offset + 5)
        fprintf(stderr, "TLS record header: type=%u version=0x%02x%02x declared_bytes=%zu available_bytes=%zu\n",
                response[offset], response[offset+1], response[offset+2], get16(response+offset+3), total-offset-5);
    fprintf(stderr, "TLS: encrypted exchange failed validation; no response delivered.\n");
    return -1;
}

int tudor_tls_command(struct tudor_tls *s, const uint8_t *command, size_t size, uint8_t *response, size_t capacity)
{
    return encrypted_exchange(s, 23, command, size, 23, response, capacity);
}

int tudor_tls_close(struct tudor_tls *s)
{
    if (!s) return -1;
    const uint8_t alert[] = {1,0};
    uint8_t response[2];
    int n = encrypted_exchange(s, 21, alert, sizeof(alert), 21, response, sizeof(response));
    if (n != 2 || memcmp(response, alert, 2)) { s->failed = 1; return -1; }
    s->established = 0; s->attempted = 0;
    return 0;
}

void tudor_tls_abort(struct tudor_tls *s)
{
    if (!s || !s->exchange || !s->attempted) return;
    const uint8_t alert[] = {2,80}; /* fatal internal_error */
    uint8_t record[64], response[TUDOR_TLS_LIMIT];
    int n = s->keys_ready ? tudor_tls_seal(s->tx_key, s->tx_salt, &s->tx_sequence, 21,
        alert, sizeof(alert), record, sizeof(record)) : plain_record(21, alert, sizeof(alert), record, sizeof(record));
    if (n > 0) {
        if (s->established) (void)s->exchange(s->context, record, (size_t)n, response, sizeof(response));
        else (void)exchange_handshake(s, record, (size_t)n, response);
    }
    s->established = 0; s->attempted = 0; s->failed = 1;
}

void tudor_tls_clear(struct tudor_tls *s)
{
    if (s) OPENSSL_cleanse(s, sizeof(*s));
}
