/* zigzag_rle.c - Zigzag scan and Run-Length Encoding for quantized DCT blocks
 *
 * Zigzag reorders 8x8 DCT coefficients from raster (row-major) to diagonal
 * order, grouping low-frequency coefficients first and high-frequency (zeros)
 * last. RLE then compresses runs of consecutive zeros.
 *
 * RLE format per block (after zigzag):
 *   [DC: int16_le]                          2 bytes, always present
 *   [zero_run: uint8][value: int16_le] ...  3 bytes per non-zero AC
 *   EOB: zero_run=0, value=0               remaining ACs are all zero
 *
 * Size per block: min 5 bytes (all ACs zero), max 191 bytes (no zeros)
 *
 * Reference: ITU-T T.81 sections F.1.2.2 (zigzag) and F.1.2.3 (RLE)
 */

#include "../include/internal.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *RLE_TAG = "jpeg_rle";
#define RLE_LOGI(...) ESP_LOGI(RLE_TAG, __VA_ARGS__)
#define RLE_LOGE(...) ESP_LOGE(RLE_TAG, __VA_ARGS__)
#define RLE_PROGRESS_STEP_BLOCKS 128
#define RLE_DECODE_TIMEOUT_TICKS pdMS_TO_TICKS(5000)
#define RLE_CHECK_TIMEOUT(where) do {                                           \
    if ((TickType_t)(xTaskGetTickCount() - t0) > RLE_DECODE_TIMEOUT_TICKS) {    \
        RLE_LOGE("one timeout em %s pos=%ld ac_idx=%d len=%ld",                 \
                 where, (long)pos, ac_idx, (long)in_len);                      \
        return -1;                                                              \
    }                                                                           \
} while (0)
#else
#define RLE_LOGI(...) ((void)0)
#define RLE_LOGE(...) ((void)0)
#define RLE_CHECK_TIMEOUT(where) ((void)0)
#endif

/* Standard JPEG zigzag scan order (ITU-T T.81, Figure A.6)
 * Maps zigzag position -> raster (row-major) position.
 * Usage: zigzag_output[z] = block[ZIGZAG_SCAN[z]]
 */
const int32_t ZIGZAG_SCAN[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ========== Zigzag scan / unscan ========== */

void jpeg_zigzag_scan(const int32_t block[64], int32_t output[64]) {
    for (int i = 0; i < 64; i++)
        output[i] = block[ZIGZAG_SCAN[i]];
}

void jpeg_zigzag_unscan(const int32_t zigzag[64], int32_t output[64]) {
    for (int i = 0; i < 64; i++)
        output[ZIGZAG_SCAN[i]] = zigzag[i];
}

static inline void store_i32_safe(int32_t *dst, int32_t value) {
#ifdef ESP_PLATFORM
    if ((((uintptr_t)dst) & 0x3u) != 0u) {
        RLE_LOGE("store_i32_safe: dst desalinhado=%p", (void *)dst);
        abort();
    }
    *((volatile int32_t *)dst) = value;
#else
    *dst = value;
#endif
}

static void jpeg_zigzag_unscan_store_trace(const int32_t zigzag[64],
                                           int32_t *output,
                                           int trace) {
#ifndef ESP_PLATFORM
    (void)trace;
#endif

    for (int i = 0; i < 64; i++) {
        int idx = ZIGZAG_SCAN[i];
        int32_t *dst = &output[idx];

#ifdef ESP_PLATFORM
        if (trace && i < 4) {
            RLE_LOGI("store begin i=%d idx=%d dst=%p val=%ld",
                     i, idx, (void *)dst, (long)zigzag[i]);
        }
#endif

        store_i32_safe(dst, zigzag[i]);

#ifdef ESP_PLATFORM
        if (trace && i < 4) {
            int32_t readback;

            RLE_LOGI("store write ok i=%d idx=%d dst=%p", i, idx, (void *)dst);
            readback = *dst;
            RLE_LOGI("store ok i=%d idx=%d dst=%p readback=%ld",
                     i, idx, (void *)dst, (long)readback);
        }
#endif
    }
}

/* ========== RLE encode / decode (single block) ========== */

static int32_t rle_encode_one(const int32_t zz[64], uint8_t *out, int32_t max_len) {
    int32_t pos = 0;

    /* DC coefficient - always present */
    if (pos + 2 > max_len) return -1;
    /* int32→int16 safe for 8-bit input: worst case J7 DC = 4×64×127 = 32512 < INT16_MAX */
    int16_t dc = (int16_t)zz[0];
    out[pos++] = (uint8_t)(dc & 0xFF);
    out[pos++] = (uint8_t)((dc >> 8) & 0xFF);

    /* Find last non-zero AC to avoid encoding trailing zeros */
    int last_nz = 0;
    for (int i = 63; i >= 1; i--) {
        if (zz[i] != 0) { last_nz = i; break; }
    }

    /* All ACs zero - emit EOB only */
    if (last_nz == 0) {
        if (pos + 3 > max_len) return -1;
        out[pos++] = 0; out[pos++] = 0; out[pos++] = 0;
        return pos;
    }

    /* Encode ACs up to last non-zero */
    int zero_run = 0;
    for (int i = 1; i <= last_nz; i++) {
        if (zz[i] == 0) {
            zero_run++;
        } else {
            if (pos + 3 > max_len) return -1;
            out[pos++] = (uint8_t)zero_run;
            /* same int32→int16 bounds hold for AC coefficients */
            int16_t val = (int16_t)zz[i];
            out[pos++] = (uint8_t)(val & 0xFF);
            out[pos++] = (uint8_t)((val >> 8) & 0xFF);
            zero_run = 0;
        }
    }

    /* EOB if trailing zeros remain */
    if (last_nz < 63) {
        if (pos + 3 > max_len) return -1;
        out[pos++] = 0; out[pos++] = 0; out[pos++] = 0;
    }

    return pos;
}

static int32_t rle_decode_one(const uint8_t *in, int32_t in_len, int32_t zz[64],
                              int trace
#ifdef ESP_PLATFORM
                              , TickType_t t0
#endif
) {
    int32_t pos = 0;
    int ac_idx = 1;

#ifndef ESP_PLATFORM
    (void)trace;
#endif

    if (!in || !zz) return -1;
#ifdef ESP_PLATFORM
    if (trace) {
        RLE_LOGI("one entry in=%p len=%ld zz=%p",
                 (const void *)in, (long)in_len, (void *)zz);
    }
#endif

    if (in_len < 5) {
        RLE_LOGE("one input muito pequeno len=%ld", (long)in_len);
        return -1;
    }

#ifdef ESP_PLATFORM
    if (trace) RLE_LOGI("one before head read");
#endif
    uint8_t h0 = in[0];
    uint8_t h1 = in[1];
    uint8_t h2 = in[2];
    uint8_t h3 = in[3];
    uint8_t h4 = in[4];
#ifdef ESP_PLATFORM
    if (trace) {
        RLE_LOGI("one head ok %02X %02X %02X %02X %02X",
                 h0, h1, h2, h3, h4);
    }
#else
    (void)h0;
    (void)h1;
    (void)h2;
    (void)h3;
    (void)h4;
#endif

    for (int i = 0; i < 64; i++) zz[i] = 0;
#ifdef ESP_PLATFORM
    if (trace) RLE_LOGI("one clear ok");
#endif

    /* DC coefficient */
    if (pos + 2 > in_len) return -1;
    zz[0] = (int16_t)(in[pos] | (in[pos + 1] << 8));
    pos += 2;
#ifdef ESP_PLATFORM
    if (trace) RLE_LOGI("one dc ok dc=%ld pos=%ld", (long)zz[0], (long)pos);
#endif

    /* AC coefficients */
    while (ac_idx < 64) {
        RLE_CHECK_TIMEOUT("ac_loop");
        if (pos + 3 > in_len) return -1;
        uint8_t zero_run = in[pos];
        int16_t val = (int16_t)(in[pos + 1] | (in[pos + 2] << 8));
        pos += 3;
#ifdef ESP_PLATFORM
        if (trace && ((ac_idx == 1) || ((ac_idx & 15) == 0))) {
            RLE_LOGI("one ac ac_idx=%d zr=%u val=%ld pos=%ld",
                     ac_idx, (unsigned)zero_run, (long)val, (long)pos);
        }
#endif

        /* EOB - remaining ACs stay zero */
        if (zero_run == 0 && val == 0) {
#ifdef ESP_PLATFORM
            if (trace) RLE_LOGI("one eob ac_idx=%d pos=%ld", ac_idx, (long)pos);
#endif
            break;
        }

        ac_idx += zero_run;
        if (ac_idx >= 64) return -1;  /* invalid stream */
        zz[ac_idx] = val;
        ac_idx++;
    }

#ifdef ESP_PLATFORM
    if (trace) RLE_LOGI("one done pos=%ld", (long)pos);
#endif
    return pos;
}

/* ========== Full-channel RLE encode / decode ========== */

int32_t jpeg_rle_encode(const int32_t *quantized, int32_t num_blocks,
                        uint8_t *output, int32_t max_len) {
    int32_t total = 0;
    int32_t zz[64];

    for (int b = 0; b < num_blocks; b++) {
        jpeg_zigzag_scan(quantized + b * 64, zz);
        int32_t written = rle_encode_one(zz, output + total, max_len - total);
        if (written < 0) return -1;
        total += written;
    }

    return total;
}

int32_t jpeg_rle_decode(const uint8_t *input, int32_t input_len,
                        int32_t *quantized, int32_t num_blocks) {
    int32_t total = 0;
    int32_t zz[64];
#ifdef ESP_PLATFORM
    TickType_t t0 = xTaskGetTickCount();
#endif

    if (!input || !quantized || input_len < 0 || num_blocks < 0) return -1;

    for (int b = 0; b < num_blocks; b++) {
#ifdef ESP_PLATFORM
        if ((b % RLE_PROGRESS_STEP_BLOCKS) == 0) {
            RLE_LOGI("decode bloco=%d/%d consumed=%ld rem=%ld dst=%p",
                     b, num_blocks, (long)total,
                     (long)(input_len - total),
                     (void *)(quantized + b * 64));
        }
        if (b == 0) {
            RLE_LOGI("decode primeiro bloco pre-call input=%p len=%ld zz=%p dst=%p",
                     (const void *)(input + total),
                     (long)(input_len - total),
                     (void *)zz,
                     (void *)(quantized + b * 64));
        }
#endif
        int32_t consumed = rle_decode_one(input + total, input_len - total, zz,
                                          b == 0
#ifdef ESP_PLATFORM
                                          , t0
#endif
        );
#ifdef ESP_PLATFORM
        if (b == 0) {
            RLE_LOGI("decode primeiro bloco post-call consumed=%ld",
                     (long)consumed);
        }
#endif
        if (consumed <= 0) {
            RLE_LOGE("decode invalido bloco=%d consumed=%ld total=%ld input_len=%ld",
                     b, (long)consumed, (long)total, (long)input_len);
            return -1;
        }
#ifdef ESP_PLATFORM
        if (b == 0) {
            RLE_LOGI("decode primeiro bloco parsed consumed=%ld total=%ld dc=%ld store=%p",
                     (long)consumed, (long)total, (long)zz[0],
                     (void *)(quantized + b * 64));
        }
        if (b == 0) {
            RLE_LOGI("decode primeiro bloco pre-store dst=%p zz=%p dc=%ld",
                     (void *)(quantized + b * 64), (void *)zz,
                     (long)zz[0]);
        }
#endif
        jpeg_zigzag_unscan_store_trace(zz, quantized + b * 64, b == 0);
#ifdef ESP_PLATFORM
        if (b == 0) {
            RLE_LOGI("decode primeiro bloco stored consumed=%ld total=%ld dc=%ld",
                     (long)consumed, (long)total, (long)zz[0]);
        }
#endif
        total += consumed;
        CODEC_YIELD_EVERY(b, 64);
#ifdef ESP_PLATFORM
        if ((TickType_t)(xTaskGetTickCount() - t0) > RLE_DECODE_TIMEOUT_TICKS) {
            RLE_LOGE("decode timeout bloco=%d/%d consumed=%ld input_len=%ld",
                     b + 1, num_blocks, (long)total, (long)input_len);
            return -1;
        }
#endif
    }

    RLE_LOGI("decode fim blocks=%d consumed=%ld", num_blocks, (long)total);
    return total;
}

/* ========== Statistics ========== */

float jpeg_count_zeros_pct(const int32_t *quantized, int32_t num_blocks) {
    int32_t total = num_blocks * 64;
    int32_t zeros = 0;

    for (int i = 0; i < total; i++) {
        if (quantized[i] == 0) zeros++;
    }

    return (total > 0) ? (float)zeros * 100.0f / (float)total : 0.0f;
}
