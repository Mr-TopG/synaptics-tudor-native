/* SPDX-License-Identifier: MIT */
#ifndef TUDOR_STATE_H
#define TUDOR_STATE_H
#include "certificate.h"
struct tudor_pairing_state {
    EVP_PKEY *identity, *sensor;
    uint8_t host_cert[400], sensor_cert[400];
};
int tudor_state_load(const char *directory, const char *authority_file, struct tudor_pairing_state *state);
void tudor_state_clear(struct tudor_pairing_state *state);
#endif
