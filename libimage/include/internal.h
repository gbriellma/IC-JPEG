/**
 * @file internal.h
 * @brief Internal implementation details for JPEG codec
 * 
 * This header contains all internal constants, function declarations,
 * and utility macros used by the JPEG compression/decompression pipeline.
 * Designed for embedded systems with fixed-point arithmetic.
 */

#ifndef INTERNAL_H
#define INTERNAL_H

#include <stdint.h>
#include <stdlib.h>

/* Use PSRAM for large allocations on ESP32 */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define CODEC_CALLOC(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
/* Yield every N blocks to feed the task watchdog during long codec runs */
#define CODEC_YIELD_EVERY(b, n) do { if (((b) & ((n)-1)) == 0) vTaskDelay(1); } while(0)
#else
#define CODEC_CALLOC(n, sz) calloc((n), (sz))
#define CODEC_YIELD_EVERY(b, n) ((void)0)
#endif

/* ========== Fixed-point arithmetic constants ========== */
#define SCALE_CONST 1048576  /* Base scale for fixed-point math (2^20) */

/* Trigonometric constants for Loeffler DCT (scaled by 2^20) */
#define C1  1028428  /* cos(pi/16)   * 2^20 */
#define S1   204567  /* sin(pi/16)   * 2^20 */
#define C3   871859  /* cos(3*pi/16) * 2^20 */
#define S3   582558  /* sin(3*pi/16) * 2^20 */
#define C6   401273  /* cos(6*pi/16) * 2^20 */
#define S6   968758  /* sin(6*pi/16) * 2^20 */
#define SQRT_2 1482910  /* sqrt(2) * 2^20 */
#define SQRT_8 2965821  /* sqrt(8) * 2^20 */

/* ========== Quantization tables (JPEG standard Q=50) ========== */
extern const int32_t Q50_LUMA[64];    /* Luminance quantization table */
extern const int32_t Q50_CHROMA[64];  /* Chrominance quantization table */
extern const int32_t ZIGZAG_ORDER[64]; /* Zigzag scan order for entropy coding */

/* ========== DCT Functions - Loeffler Algorithm ========== */
/* Fast DCT with only 11 multiplications per 1D transform */
void dct_loeffler_1d(const int32_t *input, int32_t *output);
void idct_loeffler_1d(const int32_t *input, int32_t *output);
void dct_loeffler_2d(const int32_t *input, int32_t *output);
void idct_loeffler_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - Matrix Multiplication ========== */
/* Reference implementation using direct cosine matrix */
void dct_matrix_1d(const int32_t *input, int32_t *output);
void idct_matrix_1d(const int32_t *input, int32_t *output);
void dct_matrix_2d(const int32_t *input, int32_t *output);
void idct_matrix_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - Cintra-Bayer Approximation ========== */
/* Multiplierless DCT using only additions (IEEE SPL 2011) */
void dct_approx_1d(const int32_t *input, int32_t *output);
void idct_approx_1d(const int32_t *input, int32_t *output);
void dct_approx_2d(const int32_t *input, int32_t *output);
void idct_approx_2d(const int32_t *input, int32_t *output);

/* ========== DCT Functions - Identity Transform ========== */
/* No-op transform for pipeline validation */
void dct_identity_2d(const int32_t *input, int32_t *output);
void idct_identity_2d(const int32_t *input, int32_t *output);

/* ========== Quantization Functions ========== */
void quantize(const int32_t dct_block[64], const int32_t quant_table[64], 
              int32_t output[64]);
void dequantize(const int32_t quant_block[64], const int32_t quant_table[64],
                int32_t output[64]);
void scale_quant_table(const int32_t base_table[64], float k, 
                       int32_t output[64]);

/* Fast quantization with reciprocal multiplication (no division) */
void compute_reciprocal_table(const int32_t quant_table[64], uint32_t recip_table[64]);
void quantize_fast(const int32_t dct_block[64], const int32_t quant_table[64],
                   const uint32_t recip_table[64], int32_t output[64]);

/* Apply norm correction for Cintra-Bayer approximate DCT.
 * Scales the quantization table by ||T_row_i|| * ||T_row_j|| so that
 * the multiplierless forward transform and Loeffler/Matrix see
 * equivalent quantization levels. */
void apply_approx_norm_correction(int32_t quant_table[64]);

/* ========== Color Space Conversion (ITU-R BT.601) ========== */
/* Single pixel conversion (legacy, for compatibility) */
void rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b, 
                  int32_t *y, int32_t *cb, int32_t *cr);
void ycbcr_to_rgb(int32_t y, int32_t cb, int32_t cr,
                  uint8_t *r, uint8_t *g, uint8_t *b);

/* Batch conversion (optimized - use these in codec) */
void rgb_to_ycbcr_batch(const uint8_t *rgb, int32_t *y, int32_t *cb, int32_t *cr, int n);
void ycbcr_to_rgb_batch(const int32_t *y, const int32_t *cb, const int32_t *cr, 
                        uint8_t *rgb, int n);

/* RGB565 to RGB888 conversion (for embedded cameras like OV7670) */
void convert_rgb565_to_rgb888(const uint16_t *buffer_565,
                              uint8_t *buffer_out_rgb,
                              int width, int height);

/* ========== Utility Functions ========== */

/* Clamp value to uint8 range [0, 255] */
static inline uint8_t clamp_uint8(int32_t value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/* Block extraction and reconstruction */
int extract_blocks(const int32_t *channel, int32_t width, int32_t height,
                   int32_t **blocks, int32_t *num_blocks);
int reconstruct_channel(const int32_t *blocks, int32_t num_blocks,
                        int32_t width, int32_t height, int32_t *channel);

#endif /* INTERNAL_H */