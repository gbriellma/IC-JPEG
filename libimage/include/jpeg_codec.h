/**
 * @file jpeg_codec.h
 * @brief JPEG Codec API - Multi-DCT Implementation
 * @version 1.0.0
 */

#ifndef JPEG_CODEC_H
#define JPEG_CODEC_H

#include <stdint.h>

#define JPEG_VERSION_MAJOR 1
#define JPEG_VERSION_MINOR 0
#define JPEG_VERSION_PATCH 0

/* Error codes */
typedef enum {
    JPEG_SUCCESS = 0,
    JPEG_ERROR_NULL_POINTER = -1,
    JPEG_ERROR_INVALID_DIMENSIONS = -2,
    JPEG_ERROR_ALLOCATION_FAILED = -3,
    JPEG_ERROR_INVALID_METHOD = -4
} jpeg_error_t;

/* DCT methods */
typedef enum {
    JPEG_DCT_LOEFFLER = 0,  /* 11 multiplications (fast) */
    JPEG_DCT_MATRIX = 1,    /* 64 multiplications (reference) */
    JPEG_DCT_APPROX = 2,    /* Cintra-Bayer 2011 (approximate) */
    JPEG_DCT_IDENTITY = 3   /* No transform (validation only) */
} jpeg_dct_method_t;

/* Color spaces */
typedef enum {
    JPEG_COLORSPACE_RGB = 0,
    JPEG_COLORSPACE_GRAYSCALE = 1
} jpeg_colorspace_t;

/* Image structure */
typedef struct {
    int32_t width;
    int32_t height;
    jpeg_colorspace_t colorspace;
    uint8_t *data;  /* RGB: width*height*3, Grayscale: width*height */
} jpeg_image_t;

/* Compression parameters */
typedef struct {
    float quality_factor;           /* 1.0=high, 8.0=low */
    jpeg_dct_method_t dct_method;
    int32_t use_standard_tables;    /* 1=Q50, 0=custom */
    int32_t skip_quantization;      /* 1=skip, 0=apply */
} jpeg_params_t;

/* Compressed data */
typedef struct {
    int32_t width;
    int32_t height;
    float quality_factor;
    jpeg_dct_method_t dct_method;
    
    int32_t num_blocks_y;
    int32_t num_blocks_chroma;
    
    int32_t *y_coeffs;      /* DCT coefficients */
    int32_t *y_quantized;   /* Quantized coefficients */
    int32_t *cb_coeffs;
    int32_t *cb_quantized;
    int32_t *cr_coeffs;
    int32_t *cr_quantized;
} jpeg_compressed_t;

/* API Functions */
jpeg_error_t jpeg_compress(const jpeg_image_t *image, 
                          const jpeg_params_t *params,
                          jpeg_compressed_t **compressed);

jpeg_error_t jpeg_decompress(const jpeg_compressed_t *compressed,
                            jpeg_image_t **image);

void jpeg_free_compressed(jpeg_compressed_t *compressed);
void jpeg_free_image(jpeg_image_t *image);

const char* jpeg_version(void);
const char* jpeg_error_string(jpeg_error_t error);

#endif /* JPEG_CODEC_H */