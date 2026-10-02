/* SPDX-License-Identifier: MIT */
#include "certificate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    EVP_PKEY *identity = tudor_key_generate(), *signer = tudor_key_generate(),
             *other = tudor_key_generate();
    assert(identity && signer && other);
    uint8_t cert[TUDOR_CERT_SIZE], bad[TUDOR_CERT_SIZE];
    assert(tudor_certificate_create(identity, signer, 0, cert) == 0);
    assert(tudor_certificate_verify(cert, sizeof(cert), signer) == 0);
    assert(tudor_certificate_matches(cert, sizeof(cert), identity) == 0);
    assert(tudor_certificate_matches(cert, sizeof(cert), other) != 0);
    assert(tudor_certificate_verify(cert, sizeof(cert), other) != 0);
    for (size_t n = 0; n < sizeof(cert); n++)
        assert(tudor_certificate_verify(cert, n, signer) != 0);
    assert(tudor_certificate_verify(NULL, sizeof(cert), signer) != 0);
    assert(tudor_certificate_verify(cert, sizeof(cert) + 1, signer) != 0);
    memcpy(bad, cert, sizeof(bad)); bad[141] ^= 1;
    assert(tudor_certificate_verify(bad, sizeof(bad), signer) != 0);
    memcpy(bad, cert, sizeof(bad)); bad[144] ^= 1;
    assert(tudor_certificate_verify(bad, sizeof(bad), signer) != 0);
    memcpy(bad, cert, sizeof(bad)); bad[143] = 2;
    assert(tudor_certificate_verify(bad, sizeof(bad), signer) != 0);
    memcpy(bad, cert, sizeof(bad)); bad[36] = 1; /* oversized P-256 coordinate */
    assert(tudor_certificate_public(bad, sizeof(bad)) == NULL);
    memcpy(bad, cert, sizeof(bad)); memset(bad + 4, 0, 136); /* point off curve */
    assert(tudor_certificate_public(bad, sizeof(bad)) == NULL);
    memcpy(bad, cert, sizeof(bad)); bad[2] = 24; /* wrong curve */
    assert(tudor_certificate_public(bad, sizeof(bad)) == NULL);
    EVP_PKEY_free(identity); EVP_PKEY_free(signer); EVP_PKEY_free(other);
    puts("Certificate tests passed (generated test identities, tamper and bounds checks).");
    return 0;
}
