/**
 * @file jpeg_codec.h
 * @brief Canonical JPEG-like codec API
 * @version 2.1.0
 *
 * Arithmetic regime:
 *  - public samples, coefficients, quantization, RLE and approximate DCTs use int32
 *  - Loeffler and Matrix DCTs use float internally and round back to int32
 *  - SCALE_CONST = 8192 (2^13) is kept for quantization/norm correction
 *  - flags-based params
 *  - block-based pipeline
 *  - grayscale fast path
 *  - 4:4:4 / 4:2:2 / 4:2:0 support
 *  - versioned frame header for experimental transport
 */

#ifndef JPEG_CODEC_H
#define JPEG_CODEC_H

#include <stdint.h>

#define JPEG_VERSION_MAJOR 2
#define JPEG_VERSION_MINOR 0
#define JPEG_VERSION_PATCH 0

/* ======================== Error codes ======================== */
typedef enum {
    JPEG_SUCCESS = 0,
    JPEG_ERROR_NULL_POINTER       = -1,
    JPEG_ERROR_INVALID_DIMENSIONS = -2,
    JPEG_ERROR_ALLOCATION_FAILED  = -3,
    JPEG_ERROR_INVALID_METHOD     = -4,
    JPEG_ERROR_INVALID_COLORSPACE = -5,
    JPEG_ERROR_INVALID_FRAME      = -6,
    JPEG_ERROR_BUFFER_TOO_SMALL   = -7,
    JPEG_ERROR_INVALID_SUBSAMPLING = -8,
    JPEG_ERROR_INVALID_QUALITY     = -9
} jpeg_error_t;

/* ======================== DCT methods ======================== */
typedef enum {
    JPEG_DCT_LOEFFLER    = 0,  /* Loeffler 1989 float DCT, 11 multiplications */
    JPEG_DCT_MATRIX      = 1,  /* Orthonormal float matrix DCT reference */
    JPEG_DCT_RDCT        = 2,  /* RDCT -- Cintra & Bayer, IEEE SPL 2011 */
    JPEG_DCT_SILVEIRA_J3 = 3,  /* Silveira et al. 2022, Table I, j=3 */
    JPEG_DCT_SILVEIRA_J7 = 4,  /* Silveira et al. 2022, Table I, j=7 */
    JPEG_DCT_IDENTITY    = 5   /* No transform (validation only) */
} jpeg_dct_method_t;

/* ======================== Color spaces ======================== */
typedef enum {
    JPEG_COLORSPACE_RGB       = 0,
    JPEG_COLORSPACE_GRAYSCALE = 1
} jpeg_colorspace_t;

/* ======================== Chroma subsampling ======================== */
typedef enum {
    JPEG_SUBSAMP_444 = 0,   /* No subsampling (default) */
    JPEG_SUBSAMP_422 = 1,   /* Horizontal 2:1 */
    JPEG_SUBSAMP_420 = 2    /* Both dimensions 2:1 */
} jpeg_subsampling_t;

/* ======================== Flags ======================== */
#define JPEG_FLAG_SKIP_QUANTIZATION  (1U << 0)
#define JPEG_FLAG_KEEP_COEFFS        (1U << 1)

/* ======================== Image structure ======================== */
typedef struct {
    int32_t width;
    int32_t height;
    jpeg_colorspace_t colorspace;
    uint8_t *data;  /* RGB: width*height*3, Grayscale: width*height */
} jpeg_image_t;

/* ======================== Compression parameters ======================== */
typedef struct {
    float quality_factor;           /* k > 0.0; experimental range: 0.1–0.8; max safe: 32.0 */
    jpeg_dct_method_t dct_method;
    jpeg_subsampling_t subsampling; /* chroma subsampling mode */
    uint32_t flags;                 /* JPEG_FLAG_* bitmask */
} jpeg_params_t;

/* ======================== Compressed data ======================== */
typedef struct {
    int32_t width;
    int32_t height;
    float quality_factor;
    jpeg_dct_method_t dct_method;
    jpeg_colorspace_t colorspace;
    jpeg_subsampling_t subsampling;
    uint32_t flags;                 /* JPEG_FLAG_* (persisted from params) */

    int32_t num_blocks_y;           /* luma block count */
    int32_t num_blocks_chroma;      /* chroma block count (0 for grayscale) */

    int32_t *y_coeffs;              /* DCT coefficients (only if KEEP_COEFFS) */
    int32_t *y_quantized;
    int32_t *cb_coeffs;             /* NULL for grayscale */
    int32_t *cb_quantized;          /* NULL for grayscale */
    int32_t *cr_coeffs;             /* NULL for grayscale */
    int32_t *cr_quantized;          /* NULL for grayscale */

    uint32_t dct_kernel_us;         /* ESP-only profiling: accumulated forward DCT time */
    uint32_t idct_kernel_us;        /* output written by jpeg_decompress (not const) */
    uint32_t dct_kernel_calls;      /* Number of 8x8 forward transform calls */
    uint32_t idct_kernel_calls;     /* output written by jpeg_decompress (not const) */
} jpeg_compressed_t;

/* ======================== Zigzag + RLE ======================== */
int32_t jpeg_rle_encode(const int32_t *quantized, int32_t num_blocks,
                        uint8_t *output, int32_t max_len);
int32_t jpeg_rle_decode(const uint8_t *input, int32_t input_len,
                        int32_t *quantized, int32_t num_blocks);
void jpeg_zigzag_scan(const int32_t block[64], int32_t output[64]);
void jpeg_zigzag_unscan(const int32_t zigzag[64], int32_t output[64]);
float jpeg_count_zeros_pct(const int32_t *quantized, int32_t num_blocks);

/* ======================== Codec API ======================== */
jpeg_error_t jpeg_compress(const jpeg_image_t *image,
                           const jpeg_params_t *params,
                           jpeg_compressed_t **compressed);

/* NOTE: compressed is not const - idct_kernel_us/calls are written as profiling outputs. */
jpeg_error_t jpeg_decompress(jpeg_compressed_t *compressed,
                             jpeg_image_t **image);

jpeg_error_t jpeg_decompress_into(jpeg_compressed_t *compressed,
                                  jpeg_image_t *image);

void jpeg_free_compressed(jpeg_compressed_t *compressed);
void jpeg_free_image(jpeg_image_t *image);

const char* jpeg_version(void);
const char* jpeg_error_string(jpeg_error_t error);

/* ======================== Minimal frame transport ========================
 *
 * Experimental transport frame v2:
 *   [0xA7][version=2][len_be32][payload...]
 *
 * The decoder also accepts legacy v1 frames:
 *   [0xA5][len_be16][payload...]
 *
 * The payload is the concatenation:
 *   [Y_RLE...][Cb_RLE...][Cr_RLE...]
 *
 * Decode context (method, quality factor, colorspace, subsampling, flags)
 * must be supplied out-of-band by the caller.
 */

#define JPEG_FRAME_MAGIC_V1         0xA5u
#define JPEG_FRAME_MAGIC_V2         0xA7u
#define JPEG_FRAME_MAGIC            JPEG_FRAME_MAGIC_V2
#define JPEG_FRAME_VERSION          2u
#define JPEG_FRAME_V1_HEADER_SIZE   3
#define JPEG_FRAME_HEADER_SIZE      6
#define JPEG_FRAME_V1_MAX_PAYLOAD_BYTES 0xFFFFu
#define JPEG_FRAME_MAX_PAYLOAD_BYTES    (512u * 1024u)
#define JPEG_FRAME_MAX_TOTAL_BYTES      (JPEG_FRAME_HEADER_SIZE + JPEG_FRAME_MAX_PAYLOAD_BYTES)

/* Backward-compatible aliases for older callers. */
#define JPEG_FRAME_MAX_PAYLOAD_LEN   JPEG_FRAME_MAX_PAYLOAD_BYTES
#define JPEG_FRAME_MAX_TOTAL_SIZE    JPEG_FRAME_MAX_TOTAL_BYTES

int32_t jpeg_frame_encode(const jpeg_compressed_t *comp,
                          uint8_t *out, int32_t max_len);

int32_t jpeg_frame_decode(const uint8_t *in, int32_t in_len,
                          jpeg_compressed_t *comp);

int32_t jpeg_frame_payload_encode(const jpeg_compressed_t *comp,
                                  uint8_t *out, int32_t max_len);

int32_t jpeg_frame_payload_decode(const uint8_t *payload, int32_t payload_len,
                                  jpeg_compressed_t *comp);

jpeg_compressed_t *jpeg_frame_alloc_compressed(int32_t width, int32_t height,
                                               jpeg_subsampling_t subsampling,
                                               jpeg_colorspace_t colorspace);

/* ======================== COBS framing ========================
 *
 * Consistent Overhead Byte Stuffing: eliminates 0x00 from payload,
 * allowing 0x00 as unambiguous frame delimiter on UART.
 *
 * These helpers operate only on the COBS body.
 * Adding/removing 0x00 delimiters on the wire is the caller's responsibility.
 *
 * COBS overhead: max 1 byte per 254 input bytes (~0.4%).
 */
int32_t jpeg_cobs_encode(const uint8_t *input, int32_t input_len,
                         uint8_t *output, int32_t max_len);
int32_t jpeg_cobs_decode(const uint8_t *input, int32_t input_len,
                         uint8_t *output, int32_t max_len);

/* Max COBS overhead for a given input length */
#define JPEG_COBS_MAX_ENCODED_LEN(n) ((n) + ((n) / 254) + 1)

#endif /* JPEG_CODEC_H */
