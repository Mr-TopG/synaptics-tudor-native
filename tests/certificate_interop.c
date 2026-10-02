/* SPDX-License-Identifier: MIT */
#include "certificate.h"
#include <openssl/pem.h>
#include <stdio.h>

/* Test-only bridge: reads a synthetic signing key and certificate from stdin,
 * verifies the certificate, then emits a native certificate for the same key. */
int main(void)
{
    EVP_PKEY *key = PEM_read_PrivateKey(stdin, NULL, NULL, (void *)"");
    uint8_t cert[400], native[400];
    if (!key || fread(cert, 1, sizeof(cert), stdin) != sizeof(cert) ||
        tudor_certificate_verify(cert, sizeof(cert), key) ||
        tudor_certificate_create(key, key, 0, native)) {
        EVP_PKEY_free(key); return 1;
    }
    int ok = fwrite(native, 1, sizeof(native), stdout) == sizeof(native);
    EVP_PKEY_free(key);
    return ok ? 0 : 1;
}
