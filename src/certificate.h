/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_CERTIFICATE_H
#define TUDOR_CERTIFICATE_H
#include <openssl/evp.h>
#include <stdint.h>
#include <stddef.h>
#define TUDOR_CERT_SIZE 400
EVP_PKEY *tudor_key_generate(void);
EVP_PKEY *tudor_public_from_xy(const uint8_t x[68], const uint8_t y[68]);
EVP_PKEY *tudor_certificate_public(const uint8_t *certificate, size_t size);
int tudor_certificate_create(EVP_PKEY *identity, EVP_PKEY *signer,
                             uint8_t type, uint8_t out[TUDOR_CERT_SIZE]);
int tudor_certificate_verify(const uint8_t *certificate, size_t size, EVP_PKEY *signer);
int tudor_certificate_matches(const uint8_t *certificate, size_t size, EVP_PKEY *identity);
#endif
