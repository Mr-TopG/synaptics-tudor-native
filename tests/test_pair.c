/* SPDX-License-Identifier: MIT */
#define main unused_pair_main
#include "../src/pair.c"
#undef main
#include <assert.h>

enum pair_scenario { ACCEPTED, BAD_STATUS, TRUNCATED, BAD_AUTHORITY, WRONG_HOST, TRANSFER_ERROR };
static enum pair_scenario scenario;
static struct pair_context *test_context;
static unsigned exchanges;

int tudor_device_run(tudor_device_action action, void *context)
{
    (void)action; (void)context;
    assert(!"Test must call pairing callback directly, never open USB");
    return 1;
}

int tudor_exchange(int fd, const uint8_t *request, size_t size, uint8_t *response, size_t capacity)
{
    assert(fd == 42 && ++exchanges == 1 && size == 401 && capacity >= 802 && request[0] == 0x93);
    assert(tudor_certificate_verify(request + 1, 400, test_context->signer) == 0);
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/host-key.pem", test_context->directory) > 0);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_uid == geteuid());
    EVP_PKEY *saved = load_signer(path);
    assert(saved && tudor_certificate_matches(request + 1, 400, saved) == 0);
    EVP_PKEY_free(saved);
    assert(snprintf(path, sizeof(path), "%s/request.cert", test_context->directory) > 0);
    FILE *file = fopen(path, "rb");
    uint8_t persisted[401];
    assert(file && fread(persisted, 1, sizeof(persisted), file) == 400);
    assert(memcmp(persisted, request + 1, 400) == 0);
    fclose(file);
    if (scenario == TRANSFER_ERROR) { errno = ETIMEDOUT; return -1; }
    response[0] = response[1] = 0;
    memcpy(response + 2, request + 1, 400);
    EVP_PKEY *sensor = tudor_key_generate(), *other = tudor_key_generate();
    assert(sensor && other);
    EVP_PKEY *authority = scenario == BAD_AUTHORITY ? other : test_context->sensor_authority;
    assert(tudor_certificate_create(sensor, authority, 1, response + 402) == 0);
    if (scenario == WRONG_HOST)
        assert(tudor_certificate_create(other, test_context->signer, 0, response + 2) == 0);
    EVP_PKEY_free(sensor); EVP_PKEY_free(other);
    if (scenario == BAD_STATUS) { response[0] = 3; response[1] = 4; }
    return scenario == TRUNCATED ? 700 : 802;
}

static void remove_test_state(const char *directory)
{
    const char *files[] = {"host-key.pem", "request.cert", "response.bin", "host.cert", "sensor.cert", "complete"};
    char path[512];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        assert(snprintf(path, sizeof(path), "%s/%s", directory, files[i]) > 0);
        assert(unlink(path) == 0 || errno == ENOENT);
    }
    assert(rmdir(directory) == 0);
}

int main(void)
{
    char parent[] = "/tmp/tudor-pair-test-XXXXXX";
    assert(mkdtemp(parent));
    struct pair_context ctx = {.signer = tudor_key_generate(), .sensor_authority = tudor_key_generate()};
    assert(ctx.signer && ctx.sensor_authority);
    test_context = &ctx;
    struct tudor_version version = {.major = 10, .minor = 1, .product = 0x41,
        .provision = 3, .advanced_security = 1, .key_flag = 1};
    FILE *capture = tmpfile();
    assert(capture);
    int out = dup(1), err = dup(2);
    assert(out >= 0 && err >= 0 && dup2(fileno(capture), 1) >= 0 && dup2(fileno(capture), 2) >= 0);
    char directory[512], path[600];
    assert(snprintf(directory, sizeof(directory), "%s/state", parent) > 0);
    ctx.directory = directory;
    for (scenario = ACCEPTED; scenario <= TRANSFER_ERROR; scenario++) {
        exchanges = 0;
        int result = pair_sensor(42, &version, &ctx);
        assert((result == 0) == (scenario == ACCEPTED) && exchanges == 1);
        assert(snprintf(path, sizeof(path), "%s/complete", directory) > 0);
        assert((access(path, F_OK) == 0) == (scenario == ACCEPTED));
        /* Existing state must block a second request, even after a rejected response. */
        exchanges = 0;
        assert(pair_sensor(42, &version, &ctx) != 0 && exchanges == 0);
        remove_test_state(directory);
    }
    version.key_flag = 0;
    exchanges = 0;
    assert(pair_sensor(42, &version, &ctx) != 0 && exchanges == 0 && access(directory, F_OK) != 0);
    assert(symlink(parent, directory) == 0);
    version.key_flag = 1;
    assert(pair_sensor(42, &version, &ctx) != 0 && exchanges == 0);
    assert(unlink(directory) == 0);
    /* A group-writable parent must be refused before any USB mutation. */
    assert(chmod(parent, 0770) == 0);
    assert(pair_sensor(42, &version, &ctx) != 0 && exchanges == 0);
    assert(chmod(parent, 0700) == 0);
    fflush(stdout); fflush(stderr);
    assert(dup2(out, 1) >= 0 && dup2(err, 2) >= 0);
    close(out); close(err); fclose(capture);
    assert(rmdir(parent) == 0);
    EVP_PKEY_free(ctx.signer); EVP_PKEY_free(ctx.sensor_authority);
    puts("Pairing transaction tests passed (key persistence, certificate rejection, no retries, path guards).");
    return 0;
}
