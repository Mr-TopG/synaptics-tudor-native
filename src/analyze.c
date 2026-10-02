/* SPDX-License-Identifier: MIT */
/* Offline research only: no USB or pairing keys. Explicit --preview emits a
 * provisional BMP to redirected stdout; default mode prints statistics only. */
#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include "frame_decode.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIDTH TUDOR_FRAME_WIDTH
#define HEIGHT TUDOR_FRAME_HEIGHT
#define SAMPLES (WIDTH * HEIGHT)
#define PIXEL_BYTES (SAMPLES * 2)
#define MAX_RESPONSE (PIXEL_BYTES + 10)

static void wipe(void *buffer, size_t size)
{
    volatile unsigned char *p = buffer;
    while (size--) *p++ = 0;
}

static unsigned sample(const unsigned char *p, int big_endian)
{
    return big_endian ? (unsigned)p[0] * 256 + p[1] : (unsigned)p[1] * 256 + p[0];
}

static void put_le32(unsigned char *p, unsigned value)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8*i));
}

static int preview(const unsigned char *response, int size)
{
    struct tudor_frame frame;
    if (size < 0 || tudor_parse_frame(response, (size_t)size, &frame)) {
        fprintf(stderr, "Preview refused: requires complete frame flags and a 17888-byte length field at offset 8 in a 17898-byte response.\n");
        return 1;
    }
    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "Preview output is binary; use scripts/preview-capture.sh to save it privately.\n");
        return 1;
    }
    struct tudor_gray_frame decoded;
    if (tudor_frame_decode(response,(size_t)size,&decoded)) {
        fprintf(stderr,"Preview refused: percentile range has no contrast or decoding failed.\n");
        return 1;
    }
    int low=decoded.contrast_low, high=decoded.contrast_high, result=1;
    enum { SCALE = 4, IMAGE_WIDTH = WIDTH * 2 * SCALE,
           IMAGE_HEIGHT = HEIGHT * SCALE, STRIDE = IMAGE_WIDTH * 3 };
    unsigned char header[54] = {0}, row[STRIDE];
    header[0] = 'B'; header[1] = 'M';
    put_le32(header + 2, 54 + STRIDE * IMAGE_HEIGHT);
    put_le32(header + 10, 54); put_le32(header + 14, 40);
    put_le32(header + 18, IMAGE_WIDTH); put_le32(header + 22, IMAGE_HEIGHT);
    header[26] = 1; header[28] = 24;
    put_le32(header + 34, STRIDE * IMAGE_HEIGHT);
    fprintf(stderr, "Header length candidate confirmed: offset=8 value=17888; pixels begin at byte 10 for this preview.\n"
        "Provisional signed-16 LE, 104x86 column-major preview, enlarged 4x. Left: normal contrast. Right: inverted.\n"
        "Contrast endpoints (1st/99th percentiles): %d / %d. No calibration, matching, or image-quality claim.\n", low, high);
    if (fwrite(header, 1, sizeof(header), stdout) != sizeof(header)) goto rows_done;
    for (int y = IMAGE_HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < IMAGE_WIDTH; x++) {
            int sx = (x / SCALE) % WIDTH, sy = y / SCALE;
            int gray = decoded.pixels[sy * WIDTH + sx];
            if (x >= WIDTH*SCALE) gray = 255-gray;
            row[x*3] = row[x*3+1] = row[x*3+2] = (unsigned char)gray;
        }
        if (fwrite(row, 1, sizeof(row), stdout) != sizeof(row)) goto rows_done;
    }
    result = fflush(stdout) ? 1 : 0;
rows_done:
    wipe(row, sizeof(row));
    tudor_gray_frame_clear(&decoded);
    return result;
}

static int read_response(const char *path, unsigned char *data)
{
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int file = -1, result = -1;
    struct stat st;
    if (dir < 0 || fstat(dir, &st) || st.st_uid != geteuid() || (st.st_mode & 077)) goto done;
    file = openat(dir, "frame-response.bin", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file < 0 || fstat(file, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 077) || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > MAX_RESPONSE ||
        (st.st_size != PIXEL_BYTES + 8 && st.st_size != MAX_RESPONSE)) goto done;
    size_t size = st.st_size == MAX_RESPONSE ? MAX_RESPONSE : PIXEL_BYTES + 8;
    size_t used = 0;
    while (used < size) {
        size_t remaining = size - used;
        if (remaining > MAX_RESPONSE) goto done;
        ssize_t n = read(file, data + used, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || (size_t)n > size - used) goto done;
        used += (size_t)n;
    }
    unsigned char extra;
    ssize_t n;
    do { n = read(file, &extra, 1); } while (n < 0 && errno == EINTR);
    if (n != 0 || tudor_status(data, size)) goto done;
    result = (int)size;
done:
    if (file >= 0) close(file);
    if (dir >= 0) close(dir);
    return result;
}

/* Column-major coordinates follow the reference stride, not a validated image
 * reconstruction. Constant columns/rows are useful for spotting fixed offsets. */
static void adjacent(const double *values, size_t stride, double *mad, double *correlation, int *defined)
{
    double sum_a = 0, sum_b = 0, aa = 0, bb = 0, ab = 0, difference = 0;
    unsigned count = 0;
    for (size_t i = 0; i < SAMPLES; i++) {
        if (stride == 1 ? i % HEIGHT == HEIGHT - 1 : i + stride >= SAMPLES) continue;
        double a = values[i], b = values[i + stride];
        sum_a += a; sum_b += b; aa += a*a; bb += b*b; ab += a*b;
        difference += fabs(a-b); count++;
    }
    double var_a = aa - sum_a * sum_a / count;
    double var_b = bb - sum_b * sum_b / count;
    *mad = difference / count;
    *defined = var_a > 0 && var_b > 0;
    *correlation = *defined ? (ab - sum_a * sum_b / count) / sqrt(var_a * var_b) : 0;
    if (*correlation > 1) *correlation = 1;
    if (*correlation < -1) *correlation = -1;
}

static void report_candidate(const unsigned char *response, size_t size, size_t offset, int big_endian, int signed_values)
{
    double values[SAMPLES], sum = 0, squares = 0, minimum = 65535, maximum = -32768;
    unsigned zeroes = 0, full = 0, unique = 0, bit_or = 0, bit_and = 65535;
    unsigned char seen[65536] = {0};
    for (size_t i = 0; i < SAMPLES; i++) {
        unsigned raw = sample(response + offset + i*2, big_endian);
        double value = signed_values && raw >= 32768 ? (double)raw - 65536 : (double)raw;
        values[i] = value; sum += value; squares += value * value;
        if (value < minimum) minimum = value;
        if (value > maximum) maximum = value;
        if (!raw) zeroes++;
        if (raw == 65535) full++;
        if (!seen[raw]) { seen[raw] = 1; unique++; }
        bit_or |= raw; bit_and &= raw;
    }
    double mean = sum / SAMPLES, variance = squares / SAMPLES - mean * mean;
    if (variance < 0) variance = 0;
    double vertical_mad, vertical_corr, horizontal_mad, horizontal_corr;
    int vertical_defined, horizontal_defined;
    adjacent(values, 1, &vertical_mad, &vertical_corr, &vertical_defined);
    adjacent(values, HEIGHT, &horizontal_mad, &horizontal_corr, &horizontal_defined);
    printf("{\"pixel_offset_candidate\":%zu,\"trailing_bytes_candidate\":%zu,\"encoding_candidate\":\"%s16%s\","
           "\"min\":%.0f,\"max\":%.0f,\"mean\":%.3f,\"stddev\":%.3f,\"distinct_samples\":%u,"
           "\"zero_samples\":%u,\"all_ones_samples\":%u,\"raw_bits_or\":%u,\"raw_bits_and\":%u,"
           "\"within_column_mean_abs_delta\":%.3f,\"across_columns_mean_abs_delta\":%.3f,"
           "\"within_column_correlation\":", offset, size-offset-PIXEL_BYTES,
           signed_values ? "s" : "u", big_endian ? "be" : "le", minimum, maximum, mean,
           sqrt(variance), unique, zeroes, full, bit_or, bit_and, vertical_mad, horizontal_mad);
    if (vertical_defined) printf("%.6f", vertical_corr); else printf("null");
    printf(",\"across_columns_correlation\":");
    if (horizontal_defined) printf("%.6f", horizontal_corr); else printf("null");
    printf("}");
    wipe(values, sizeof(values)); wipe(seen, sizeof(seen));
}

int main(int argc, char **argv)
{
    int want_preview = argc == 3 && !strcmp(argv[1], "--preview");
    if (argc != 2 && !want_preview) {
        fprintf(stderr, "Usage: %s [--preview] PRIVATE_CAPTURE_DIRECTORY\n", argv[0]); return 2;
    }
    unsigned char response[MAX_RESPONSE];
    int n = read_response(argv[want_preview ? 2 : 1], response);
    if (n < 0) {
        fprintf(stderr, "Cannot read private frame-response.bin: check ownership, modes, regular-file type, size, and command status.\n");
        wipe(response, sizeof(response)); return 1;
    }
    if (want_preview) {
        int result = preview(response, n);
        wipe(response, sizeof(response)); return result;
    }
    fprintf(stderr, "Offline summary only: no USB access, pairing access, image export, or file writes.\n"
        "Candidate statistics cannot prove the extra-byte location, sample encoding, or fingerprint quality.\n");
    printf("{\"analysis_version\":1,\"response_bytes\":%d,\"width_assumed\":104,\"height_assumed\":86,"
           "\"storage_order_assumed\":\"column-major\",\"layout_verified\":false,"
           "\"reference_flags_candidate\":%u,\"reference_index_candidate\":%u,"
           "\"offset8_length_candidate\":%u,\"offset8_length_matches_pixels\":%s,\"candidates\":[\n",
           n, sample(response+2, 0), sample(response+6, 0), sample(response+8, 0),
           n == MAX_RESPONSE && sample(response+8, 0) == PIXEL_BYTES ? "true" : "false");
    int first = 1;
    for (size_t offset = 8; offset + PIXEL_BYTES <= (size_t)n; offset += 2) {
        for (int big = 0; big <= 1; big++) for (int sign = 0; sign <= 1; sign++) {
            if (!first) printf(",\n");
            first = 0;
            report_candidate(response, (size_t)n, offset, big, sign);
        }
    }
    puts("\n]}"); wipe(response, sizeof(response));
    return ferror(stdout) ? 1 : 0;
}
