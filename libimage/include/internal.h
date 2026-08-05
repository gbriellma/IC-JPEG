/**
 * @file internal.h
 * @brief Internal implementation details for JPEG codec
 *
 * Loeffler and Matrix DCTs use float arithmetic internally (single-precision).
 * Approximate DCTs (RDCT, Silveira) remain pure int32. SCALE_CONST is kept
 * for quantization and approx-DCT norm correction. The public API (int32_t
 * in/out) is the same for all methods.
 */

#ifndef INTERNAL_H
#define INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "jpeg_codec.h"

/* Use PSRAM for large allocations on ESP32 */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define CODEC_CALLOC(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CODEC_CALLOC_SMALL(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_8BIT)
#define CODEC_MALLOC(bytes) heap_caps_malloc((bytes), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CODEC_MALLOC_SMALL(bytes) heap_caps_malloc((bytes), MALLOC_CAP_8BIT)
#define CODEC_FREE(p) heap_caps_free((p))
#define CODEC_YIELD_EVERY(b, n) do { if ((((b) + 1) & ((n)-1)) == 0) taskYIELD(); } while(0)
static inline int64_t codec_time_us(void) { return esp_timer_get_time(); }
#else
#define CODEC_CALLOC(n, sz) calloc((n), (sz))
#define CODEC_CALLOC_SMALL(n, sz) calloc((n), (sz))
#define CODEC_MALLOC(bytes) malloc((bytes))
#define CODEC_MALLOC_SMALL(bytes) malloc((bytes))
#define CODEC_FREE(p) free((p))
#define CODEC_YIELD_EVERY(b, n) ((void)0)
static inline int64_t codec_time_us(void) { return 0; }
#endif

static inline void *codec_alloc_zero_bytes(size_t bytes)
{
    void *p = CODEC_MALLOC(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

static inline void *codec_alloc_zero_small_bytes(size_t bytes)
{
    void *p = CODEC_MALLOC_SMALL(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

/* ========== Fixed-point arithmetic constants (SCALE = 2^13) ========== */
#define SCALE_BITS   13
#define SCALE_CONST  8192   /* 2^13 -- used by quantization and approx-DCT norm */

/* Approx-DCT norm factors: row norms squared = {8,6,4,6,8,6,4,6}
 * IDCT scale factor per row = LCM(8,6,4) / norm^2
 *   row 0,4 -> 24/8 = 3    row 1,3,5,7 -> 24/6 = 4    row 2,6 -> 24/4 = 6 */
#define APPROX_IDCT_SCALE {3, 4, 6, 4, 3, 4, 6, 4}

/* ========== Quantization tables (JPEG standard Q=50) ========== */
extern const int32_t Q50_LUMA[64];
extern const int32_t Q50_CHROMA[64];
/* ZIGZAG_ORDER[raster_pos] = zigzag_pos  - raster→zigzag mapping (Python/reference) */
extern const int32_t ZIGZAG_ORDER[64];
/* ZIGZAG_SCAN[zigzag_pos]  = raster_pos  - zigzag→raster mapping (RLE pipeline) */
extern const int32_t ZIGZAG_SCAN[64];

/* ========== DCT Functions - Loeffler Algorithm ========== */
void dct_loeffler_1d(const int32_t *input, int32_t *output);
void idct_loeffler_1d(const int32_t *input, int32_t *output);
void dct_loeffler_2d(const int32_t *input, int32_t *output);
void idct_loeffler_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - Matrix Multiplication ========== */
void dct_matrix_1d(const int32_t *input, int32_t *output);
void idct_matrix_1d(const int32_t *input, int32_t *output);
void dct_matrix_2d(const int32_t *input, int32_t *output);
void idct_matrix_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - RDCT (Cintra-Bayer 2011) ========== */
void dct_rdct_2d(const int32_t *input, int32_t *output);
void idct_rdct_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - Silveira et al. 2022 ========== */
void dct_silveira_j3_2d(const int32_t *input, int32_t *output);
void idct_silveira_j3_2d(const int32_t *input, int32_t *output);
void dct_silveira_j7_2d(const int32_t *input, int32_t *output);
void idct_silveira_j7_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - Identity Transform ========== */
void dct_identity_2d(const int32_t *input, int32_t *output);
void idct_identity_2d(const int32_t *input, int32_t *output);

/* ========== Quantization Functions ========== */
void quantize(const int32_t dct_block[64], const int32_t quant_table[64],
              int32_t output[64]);
void dequantize(const int32_t quant_block[64], const int32_t quant_table[64],
                int32_t output[64]);
void scale_quant_table(const int32_t base_table[64], float k,
                       int32_t output[64]);

void compute_reciprocal_table(const int32_t quant_table[64], uint32_t recip_table[64]);
void quantize_fast(const int32_t dct_block[64], const int32_t quant_table[64],
                   const uint32_t recip_table[64], int32_t output[64]);

void apply_approx_norm_correction(int32_t quant_table[64]);
void apply_class_norm_correction(int32_t *quant_table, int j);

/* ========== Color Space Conversion (ITU-R BT.601) ========== */
void rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b,
                  int32_t *y, int32_t *cb, int32_t *cr);
void ycbcr_to_rgb(int32_t y, int32_t cb, int32_t cr,
                  uint8_t *r, uint8_t *g, uint8_t *b);

void rgb_to_ycbcr_batch(const uint8_t *rgb, int32_t *y, int32_t *cb, int32_t *cr, int n);
void ycbcr_to_rgb_batch(const int32_t *y, const int32_t *cb, const int32_t *cr,
                        uint8_t *rgb, int n);

void convert_rgb565_to_rgb888(const uint16_t *buffer_565,
                              uint8_t *buffer_out_rgb,
                              int width, int height);

/* ========== Utility Functions ========== */

static inline uint8_t clamp_uint8(int32_t value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/* Validate enum ranges */
static inline int jpeg_valid_dct_method(int m) {
    return m >= JPEG_DCT_LOEFFLER && m <= JPEG_DCT_IDENTITY;
}
static inline int jpeg_method_to_class_j(int m) {
    switch (m) {
        case JPEG_DCT_SILVEIRA_J3: return 3;
        case JPEG_DCT_SILVEIRA_J7: return 7;
        default: return -1;
    }
}
static inline int jpeg_valid_colorspace(int c) {
    return c == JPEG_COLORSPACE_RGB || c == JPEG_COLORSPACE_GRAYSCALE;
}
static inline int jpeg_valid_subsampling(int s) {
    return s >= JPEG_SUBSAMP_444 && s <= JPEG_SUBSAMP_420;
}
static inline int jpeg_valid_quality_factor(float q) {
    /* Upper bound 32.0: above ~7481 the two-step norm correction in
     * apply_approx_norm_correction overflows int32. 32 gives 4x headroom
     * over the documented max (8.0=low quality). */
    return isfinite(q) && q > 0.0f && q <= 32.0f;
}

static inline int32_t codec_ceil_div_pos(int32_t v, int32_t d) {
    return v / d + ((v % d) != 0);
}

static inline int codec_checked_mul_i32(int32_t a, int32_t b, int32_t *out) {
    if (!out || a < 0 || b < 0) return 0;
    if (a != 0 && b > INT32_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static inline int codec_checked_mul_size(size_t a, size_t b, size_t *out) {
    if (!out) return 0;
    if (a != 0u && b > SIZE_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static inline int codec_checked_mul3_size(size_t a, size_t b, size_t c, size_t *out) {
    size_t ab;
    return codec_checked_mul_size(a, b, &ab) && codec_checked_mul_size(ab, c, out);
}

static inline int32_t codec_div_round_symm(int32_t num, int32_t den) {
    if (num >= 0) return (num + den / 2) / den;
    return (num - den / 2) / den;
}

static inline int codec_compute_block_geometry(int32_t width, int32_t height,
                                               jpeg_colorspace_t colorspace,
                                               jpeg_subsampling_t subsampling,
                                               int32_t *bx_out, int32_t *by_out,
                                               int32_t *num_luma_out, int32_t *num_chroma_out) {
    int32_t bx, by, num_luma, num_chroma;

    if (!jpeg_valid_colorspace(colorspace) || !jpeg_valid_subsampling(subsampling)) return 0;
    if (width <= 0 || height <= 0) return 0;

    bx = codec_ceil_div_pos(width, 8);
    by = codec_ceil_div_pos(height, 8);
    if (!codec_checked_mul_i32(bx, by, &num_luma)) return 0;

    if (colorspace == JPEG_COLORSPACE_GRAYSCALE) {
        num_chroma = 0;
        subsampling = JPEG_SUBSAMP_444;
    } else if (subsampling == JPEG_SUBSAMP_420) {
        int32_t mcx = codec_ceil_div_pos(width, 16);
        int32_t mcy = codec_ceil_div_pos(height, 16);
        if (!codec_checked_mul_i32(mcx, mcy, &num_chroma)) return 0;
    } else if (subsampling == JPEG_SUBSAMP_422) {
        int32_t mcx = codec_ceil_div_pos(width, 16);
        if (!codec_checked_mul_i32(mcx, by, &num_chroma)) return 0;
    } else {
        num_chroma = num_luma;
    }

    if (bx_out) *bx_out = bx;
    if (by_out) *by_out = by;
    if (num_luma_out) *num_luma_out = num_luma;
    if (num_chroma_out) *num_chroma_out = num_chroma;
    return 1;
}

/* ========== Zigzag + RLE ========== */
void jpeg_zigzag_scan(const int32_t block[64], int32_t output[64]);
void jpeg_zigzag_unscan(const int32_t zigzag[64], int32_t output[64]);

int32_t jpeg_rle_encode(const int32_t *quantized, int32_t num_blocks,
                        uint8_t *output, int32_t max_len);
int32_t jpeg_rle_decode(const uint8_t *input, int32_t input_len,
                        int32_t *quantized, int32_t num_blocks);
float jpeg_count_zeros_pct(const int32_t *quantized, int32_t num_blocks);

/* ========== Block extraction and reconstruction (legacy, for compatibility) ========== */
int extract_blocks(const int32_t *channel, int32_t width, int32_t height,
                   int32_t **blocks, int32_t *num_blocks);
int reconstruct_channel(const int32_t *blocks, int32_t num_blocks,
                        int32_t width, int32_t height, int32_t *channel);

/* ========== Block-based pixel extraction (v2 pipeline) ========== */

void extract_luma_block_rgb(const uint8_t *rgb, int32_t width, int32_t height,
                            int32_t bx, int32_t by, int32_t block[64]);

void extract_luma_block_gray(const uint8_t *gray, int32_t width, int32_t height,
                             int32_t bx, int32_t by, int32_t block[64]);

void extract_chroma_blocks_444(const uint8_t *rgb, int32_t width, int32_t height,
                               int32_t bx, int32_t by,
                               int32_t cb_block[64], int32_t cr_block[64]);

void extract_chroma_blocks_420(const uint8_t *rgb, int32_t width, int32_t height,
                               int32_t mcx, int32_t mcy,
                               int32_t cb_block[64], int32_t cr_block[64]);

void extract_chroma_blocks_422(const uint8_t *rgb, int32_t width, int32_t height,
                               int32_t mcx, int32_t mcy,
                               int32_t cb_block[64], int32_t cr_block[64]);

void write_block_to_rgb(uint8_t *rgb, int32_t width, int32_t height,
                        int32_t bx, int32_t by,
                        const int32_t y_block[64],
                        const int32_t cb_block[64],
                        const int32_t cr_block[64]);

void write_block_to_gray(uint8_t *gray, int32_t width, int32_t height,
                         int32_t bx, int32_t by,
                         const int32_t y_block[64]);

void upsample_chroma_420(const int32_t chroma_8x8[64],
                         int32_t out_tl[64], int32_t out_tr[64],
                         int32_t out_bl[64], int32_t out_br[64]);

void upsample_chroma_422(const int32_t chroma_8x8[64],
                         int32_t out_left[64], int32_t out_right[64]);

#endif /* INTERNAL_H */
