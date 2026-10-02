/* SPDX-License-Identifier: MIT */
#include "certificate.h"
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <string.h>

static int is_p256(EVP_PKEY *key)
{
    char name[80];
    size_t n;
    return key && EVP_PKEY_is_a(key, "EC") &&
        EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME,
                                       name, sizeof(name), &n) == 1 &&
        !strcmp(name, "prime256v1");
}

EVP_PKEY *tudor_key_generate(void)
{
    return EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
}

EVP_PKEY *tudor_public_from_xy(const uint8_t x[68], const uint8_t y[68])
{
    if (!x || !y) return NULL;
    for (size_t i = 32; i < 68; i++) if (x[i] || y[i]) return NULL;
    uint8_t point[65] = {4};
    for (size_t i = 0; i < 32; i++) {
        point[1 + i] = x[31 - i]; point[33 + i] = y[31 - i];
    }
    char group[] = "prime256v1";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, group, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)),
        OSSL_PARAM_construct_end()
    };
    EVP_PKEY *key = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (!ctx || EVP_PKEY_fromdata_init(ctx) != 1 ||
        EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) != 1) {
        EVP_PKEY_free(key); key = NULL;
    }
    EVP_PKEY_CTX_free(ctx);
    if (key) {
        ctx = EVP_PKEY_CTX_new(key, NULL);
        if (!ctx || EVP_PKEY_public_check(ctx) != 1) { EVP_PKEY_free(key); key = NULL; }
        EVP_PKEY_CTX_free(ctx);
    }
    return key;
}

EVP_PKEY *tudor_certificate_public(const uint8_t *cert, size_t size)
{
    if (!cert || size != TUDOR_CERT_SIZE || cert[0] != 0x3f || cert[1] != 0x5f ||
        cert[2] != 23 || cert[3] != 0) return NULL;
    unsigned sig_size = (unsigned)cert[142] | (unsigned)cert[143] << 8;
    if (!sig_size || sig_size > 256) return NULL;
    return tudor_public_from_xy(cert + 4, cert + 72);
}

int tudor_certificate_create(EVP_PKEY *identity, EVP_PKEY *signer,
                             uint8_t type, uint8_t out[TUDOR_CERT_SIZE])
{
    if (!out || !is_p256(identity) || !is_p256(signer)) return -1;
    uint8_t point[65];
    size_t n = 0;
    if (EVP_PKEY_get_octet_string_param(identity, OSSL_PKEY_PARAM_PUB_KEY,
                                         point, sizeof(point), &n) != 1 ||
        n != 65 || point[0] != 4) return -1;
    memset(out, 0, TUDOR_CERT_SIZE);
    out[0] = 0x3f; out[1] = 0x5f; out[2] = 23; out[141] = type;
    for (size_t i = 0; i < 32; i++) { out[4 + i] = point[32 - i]; out[72 + i] = point[64 - i]; }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    n = 256;
    int ok = ctx && EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, signer) == 1 &&
        EVP_DigestSign(ctx, out + 144, &n, out, 142) == 1 && n <= 256;
    EVP_MD_CTX_free(ctx);
    if (!ok) { memset(out, 0, TUDOR_CERT_SIZE); return -1; }
    out[142] = (uint8_t)n; out[143] = (uint8_t)(n >> 8);
    return 0;
}

int tudor_certificate_verify(const uint8_t *cert, size_t size, EVP_PKEY *signer)
{
    EVP_PKEY *subject = tudor_certificate_public(cert, size);
    if (!subject || !is_p256(signer)) { EVP_PKEY_free(subject); return -1; }
    EVP_PKEY_free(subject);
    size_t n = (size_t)cert[142] | (size_t)cert[143] << 8;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    int ok = ctx && EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, signer) == 1 &&
        EVP_DigestVerify(ctx, cert + 144, n, cert, 142) == 1;
    EVP_MD_CTX_free(ctx);
    return ok ? 0 : -1;
}

int tudor_certificate_matches(const uint8_t *cert, size_t size, EVP_PKEY *identity)
{
    EVP_PKEY *subject = tudor_certificate_public(cert, size);
    int ok = subject && identity && EVP_PKEY_eq(subject, identity) == 1;
    EVP_PKEY_free(subject);
    return ok ? 0 : -1;
}
