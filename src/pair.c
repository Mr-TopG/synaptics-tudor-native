/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "certificate.h"
#include "usb.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct pair_context {
    const char *directory;
    EVP_PKEY *signer, *sensor_authority;
};

static int create_state_directory(const char *path)
{
    /* Anchor creation to a private/non-writable parent and sync that parent.
     * This keeps a power loss from losing the directory containing the new key. */
    if (!path || path[0] != '/') { errno = EINVAL; return -1; }
    char *copy = strdup(path);
    if (!copy) return -1;
    char *slash = strrchr(copy, '/'), *leaf = slash + 1;
    if (!*leaf || !strcmp(leaf, ".") || !strcmp(leaf, "..")) {
        free(copy); errno = EINVAL; return -1;
    }
    *slash = '\0';
    int parent = open(*copy ? copy : "/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int directory = -1;
    struct stat st;
    if (parent >= 0 && fstat(parent, &st) == 0 && st.st_uid == geteuid() &&
        !(st.st_mode & 022) && mkdirat(parent, leaf, 0700) == 0) {
        if (fsync(parent) == 0)
            directory = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    int saved_errno = errno;
    if (parent >= 0) close(parent);
    free(copy);
    errno = saved_errno;
    return directory;
}

/* Create-only files; never overwrite an existing host identity. */
static int save_file(int directory, const char *name, const void *data, size_t size)
{
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = write(fd, p + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        offset += (size_t)n;
    }
    int rc = fsync(fd);
    if (close(fd) < 0) rc = -1;
    if (fsync(directory) < 0) rc = -1;
    return rc;
}

static int save_identity(int directory, EVP_PKEY *key)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return -1;
    int rc = -1;
    if (PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) == 1) {
        char *data = NULL;
        long size = BIO_get_mem_data(bio, &data);
        if (size > 0) {
            rc = save_file(directory, "host-key.pem", data, (size_t)size);
            OPENSSL_cleanse(data, (size_t)size);
        }
    }
    BIO_free(bio);
    return rc;
}

static int pair_sensor(int fd, const struct tudor_version *version, void *opaque)
{
    struct pair_context *ctx = opaque;
    /* The supplied authority is specifically the 10.1 key-flag profile. */
    if (version->major != 10 || version->minor != 1 || version->product != 0x41 ||
        version->provision != 3 || !version->advanced_security || !version->key_flag) {
        fprintf(stderr, "Pairing refused: sensor does not match the researched 10.1/0x41/key-flag profile.\n");
        return 1;
    }
    EVP_PKEY *identity = tudor_key_generate();
    uint8_t request[1 + TUDOR_CERT_SIZE] = {0x93};
    if (!identity || tudor_certificate_create(identity, ctx->signer, 0, request + 1) ||
        tudor_certificate_verify(request + 1, TUDOR_CERT_SIZE, ctx->signer)) {
        fprintf(stderr, "Could not generate and self-check the host certificate. No pairing command sent.\n");
        EVP_PKEY_free(identity);
        return 1;
    }
    int result = 1, directory = -1;
    directory = create_state_directory(ctx->directory);
    struct stat st;
    if (directory < 0 || fstat(directory, &st) < 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
        fprintf(stderr, "Cannot create a new private pairing directory. Use an absolute path under\n"
                "a directory you own without group/other write access. Existing state is never reused.\n");
        goto done;
    }
    /* Durably save the private key and request BEFORE the only mutating command. */
    if (save_identity(directory, identity) ||
        save_file(directory, "request.cert", request + 1, TUDOR_CERT_SIZE)) {
        perror("Save pending host identity (pairing command not sent)");
        goto done;
    }
    fprintf(stderr, "Host identity saved. Sending one experimental PAIR (0x93) command.\n");
    uint8_t response[1024];
    int n = tudor_exchange(fd, request, sizeof(request), response, sizeof(response));
    if (n < 0) {
        perror("PAIR transfer (not retried; keep the pairing directory)");
        goto done;
    }
    /* Keep the response before validation so a rejected certificate can be diagnosed
     * without sending a second pairing request. Never print certificates or keys. */
    if (save_file(directory, "response.bin", response, (size_t)n)) {
        perror("Save pairing response");
        goto done;
    }
    int status = tudor_status(response, (size_t)n);
    if (status || n != 2 + 2 * TUDOR_CERT_SIZE) {
        fprintf(stderr, "PAIR failed: status=%d, response_bytes=%d (expected 802).\n", status, n);
        goto done;
    }
    const uint8_t *host = response + 2, *sensor = host + TUDOR_CERT_SIZE;
    if (tudor_certificate_matches(host, TUDOR_CERT_SIZE, identity)) {
        fprintf(stderr, "PAIR rejected: returned host certificate has a different or invalid public key.\n");
        goto done;
    }
    if (tudor_certificate_verify(sensor, TUDOR_CERT_SIZE, ctx->sensor_authority)) {
        fprintf(stderr, "PAIR rejected: sensor certificate does not verify against the supplied 10.1 authority.\n");
        goto done;
    }
    if (save_file(directory, "host.cert", host, TUDOR_CERT_SIZE) ||
        save_file(directory, "sensor.cert", sensor, TUDOR_CERT_SIZE)) {
        perror("Save verified pairing certificates");
        goto done;
    }
    const char complete[] = "Native pairing response verified. TLS not attempted by pairing tool. Authentication unavailable.\n";
    if (save_file(directory, "complete", complete, sizeof(complete) - 1)) {
        perror("Commit pairing result"); goto done;
    }
    puts("{\"pairing_response_verified\":true,\"sensor_certificate_verified\":true,"
         "\"tls_session_attempted\":false,\"authentication_supported\":false}");
    result = 0;
done:
    if (directory >= 0) close(directory);
    EVP_PKEY_free(identity);
    if (result) fprintf(stderr, "No erase, reset, storage-format, or firmware command was sent.\n");
    return result;
}

static EVP_PKEY *load_signer(const char *path)
{
    BIO *bio = BIO_new_file(path, "r");
    if (!bio) return NULL;
    /* No interactive PEM password prompt: the public research HS key is unencrypted. */
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, (void *)"");
    BIO_free(bio);
    return key;
}

static EVP_PKEY *load_sensor_authority(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    uint8_t data[257];
    size_t n = fread(data, 1, sizeof(data), file);
    int failed = ferror(file);
    fclose(file);
    if (failed || n != 256) return NULL;
    return tudor_public_from_xy(data, data + 68);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s NEW_STATE_DIRECTORY HS_KEY_PEM SENSOR_10_1_KF_TSK\n"
                "Experimental native pairing; changes host/sensor pairing state.\n"
                "Sends no erase/format commands; existing Windows pairing may be affected.\n"
                "Does not enable fingerprint login.\n", argv[0]);
        return 2;
    }
    umask(077);
    struct pair_context ctx = {.directory = argv[1],
        .signer = load_signer(argv[2]), .sensor_authority = load_sensor_authority(argv[3])};
    if (!ctx.signer || !ctx.sensor_authority) {
        fprintf(stderr, "Cannot load the P-256 reference signing key / 10.1-kf authority.\n");
        EVP_PKEY_free(ctx.signer); EVP_PKEY_free(ctx.sensor_authority);
        return 1;
    }
    int result = tudor_device_run(pair_sensor, &ctx);
    EVP_PKEY_free(ctx.signer); EVP_PKEY_free(ctx.sensor_authority);
    return result;
}
