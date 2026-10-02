/* SPDX-License-Identifier: MIT */
#include "state.h"
#include "tls.h"
#include "usb.h"
#include <stdio.h>

static int usb_exchange(void *context, const uint8_t *request, size_t size,
                         uint8_t *response, size_t capacity)
{
    int n = tudor_exchange(*(int *)context, request, size, response, capacity);
    if (n < 0) perror("TLS USB transfer");
    return n;
}

static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }

static int session_action(int fd, const struct tudor_version *version, void *context)
{
    struct tudor_pairing_state *state = context;
    if (version->major != 10 || version->minor != 1 || version->product != 0x41 ||
        version->provision != 3 || !version->advanced_security || !version->key_flag) {
        fprintf(stderr, "Session refused: sensor does not match saved 10.1/0x41/key-flag profile.\n");
        return 1;
    }
    struct tudor_tls tls;
    int result = 1;
    if (tudor_tls_open(&tls, state->identity, state->sensor, state->host_cert, usb_exchange, &fd)) goto done;
    if (tudor_device_tls_status(fd) != 1) {
        fprintf(stderr, "TLS proof verified, but sensor did not report an active session.\n"); goto done;
    }
    uint8_t response[256], command = 1;
    int n = tudor_tls_command(&tls, &command, 1, response, sizeof(response));
    struct tudor_version secure_version;
    if (n < 0 || tudor_parse_version(response, (size_t)n, &secure_version) ||
        version->major != secure_version.major || version->minor != secure_version.minor ||
        version->build != secure_version.build || version->product != secure_version.product ||
        version->provision != secure_version.provision ||
        version->key_flag != secure_version.key_flag || version->advanced_security != secure_version.advanced_security) {
        fprintf(stderr, "Encrypted GET_VERSION failed or disagrees with initial probe.\n"); goto done;
    }
    fprintf(stderr, "TLS: encrypted GET_VERSION verified. Reading frame dimensions.\n");
    const uint8_t dimensions_command[] = {0x82,0,0,0,0,0,0,2,7};
    uint8_t dimensions[256];
    n = tudor_tls_command(&tls, dimensions_command, sizeof(dimensions_command), dimensions, sizeof(dimensions));
    if (n < 0) goto done;
    int status = tudor_status(dimensions, (size_t)n);
    if (status == -1 || (status == 0 && n != 34)) {
        fprintf(stderr, "Unexpected FRAME_STATE_GET response length=%d.\n", n); goto done;
    }
    int dimensions_ok = status == 0;
    if (!dimensions_ok) fprintf(stderr, "FRAME_STATE_GET returned status 0x%04x; TLS communication succeeded.\n", (unsigned)status);
    if (tudor_tls_close(&tls)) {
        fprintf(stderr, "Could not verify sensor close_notify.\n"); goto done;
    }
    if (tudor_device_tls_status(fd) != 0) {
        fprintf(stderr, "Close notification verified, but sensor did not report idle.\n"); goto done;
    }
    printf("{\n  \"tls_established_and_verified\": true,\n"
           "  \"encrypted_firmware\": \"%u.%u.%u\",\n"
           "  \"session_closed\": true,\n  \"authentication_supported\": false,\n"
           "  \"frame_state_status\": %d,\n  \"frame_dimensions\": ",
           secure_version.major, secure_version.minor, secure_version.build, status);
    if (dimensions_ok) {
        printf("{\"pixel_bits\":%u,\"width\":%u,\"frame_header_size\":%u,"
               "\"x_offset\":%u,\"x_size\":%u,\"height\":%u,\"column_header_size\":%u,"
               "\"y_offset\":%u,\"y_size\":%u}", le16(dimensions + 14), le16(dimensions + 16),
               le16(dimensions + 18), le16(dimensions + 20), le16(dimensions + 22),
               le16(dimensions + 24), le16(dimensions + 26), le16(dimensions + 28), le16(dimensions + 30));
    } else printf("null");
    puts("\n}"); result = 0;
done:
    if (result) {
        tudor_tls_abort(&tls);
        fprintf(stderr, "TLS experiment stopped; sensor TLS status now=%d (0 idle, 1 active, -1 unreadable).\n",
                tudor_device_tls_status(fd));
        fprintf(stderr, "Saved pairing files are unchanged. No reset or re-pair was attempted.\n");
    }
    tudor_tls_clear(&tls);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s EXISTING_STATE_DIRECTORY SENSOR_10_1_KF_TSK\n", argv[0]); return 2;
    }
    struct tudor_pairing_state state;
    if (tudor_state_load(argv[1], argv[2], &state)) return 1;
    int result = tudor_device_run(session_action, &state);
    tudor_state_clear(&state);
    return result;
}
