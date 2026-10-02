/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "state.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int private_read(int directory, const char *name, uint8_t *out, size_t capacity)
{
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    int result = -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_nlink != 1 || st.st_size <= 0 ||
        (uintmax_t)st.st_size > capacity) goto done;
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, out + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        used += (size_t)n;
    }
    uint8_t extra;
    if (read(fd, &extra, 1) != 0) goto done;
    result = (int)used;
done:
    close(fd);
    return result;
}

int tudor_state_load(const char *directory, const char *authority_file, struct tudor_pairing_state *state)
{
    if (!directory || !authority_file || !state) return -1;
    memset(state, 0, sizeof(*state));
    int fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    uint8_t data[4096];
    EVP_PKEY *authority = NULL;
    int result = -1;
    if (fd < 0 || fstat(fd, &st) || st.st_uid != geteuid() || (st.st_mode & 077)) goto done;
    if (private_read(fd, "complete", data, sizeof(data)) <= 0 ||
        private_read(fd, "host.cert", state->host_cert, 400) != 400 ||
        private_read(fd, "sensor.cert", state->sensor_cert, 400) != 400) goto done;
    int n = private_read(fd, "host-key.pem", data, sizeof(data));
    if (n <= 0) goto done;
    BIO *bio = BIO_new_mem_buf(data, n);
    if (!bio) goto done;
    state->identity = PEM_read_bio_PrivateKey(bio, NULL, NULL, (void *)"");
    BIO_free(bio); OPENSSL_cleanse(data, sizeof(data));
    EVP_PKEY_CTX *check = state->identity ? EVP_PKEY_CTX_new(state->identity, NULL) : NULL;
    int valid = check && EVP_PKEY_pairwise_check(check) == 1;
    EVP_PKEY_CTX_free(check);
    if (!valid || tudor_certificate_matches(state->host_cert, 400, state->identity)) goto done;
    /* Public reference authority, not a private per-host file. */
    FILE *f = fopen(authority_file, "rb");
    if (!f) goto done;
    size_t got = fread(data, 1, 257, f);
    int error = ferror(f); fclose(f);
    if (error || got != 256) goto done;
    authority = tudor_public_from_xy(data, data + 68);
    if (!authority || tudor_certificate_verify(state->sensor_cert, 400, authority)) goto done;
    state->sensor = tudor_certificate_public(state->sensor_cert, 400);
    if (!state->sensor) goto done;
    result = 0;
done:
    if (fd >= 0) close(fd);
    EVP_PKEY_free(authority); OPENSSL_cleanse(data, sizeof(data));
    if (result) {
        tudor_state_clear(state);
        fprintf(stderr, "Cannot load verified pairing state: check private ownership/modes, complete marker, and certificates.\n");
    }
    return result;
}

void tudor_state_clear(struct tudor_pairing_state *state)
{
    if (!state) return;
    EVP_PKEY_free(state->identity); EVP_PKEY_free(state->sensor);
    OPENSSL_cleanse(state, sizeof(*state));
}
