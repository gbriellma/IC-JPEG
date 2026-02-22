/* codec.c - Main compression/decompression pipeline */

#include "../include/jpeg_codec.h"
#include "../include/internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

jpeg_error_t jpeg_compress(const jpeg_image_t *image, 
                          const jpeg_params_t *params,
                          jpeg_compressed_t **compressed) {
    if (!image || !params || !compressed) return JPEG_ERROR_NULL_POINTER;
    if (image->width <= 0 || image->height <= 0) return JPEG_ERROR_INVALID_DIMENSIONS;
    
    *compressed = (jpeg_compressed_t*)calloc(1, sizeof(jpeg_compressed_t));
    if (!*compressed) return JPEG_ERROR_ALLOCATION_FAILED;
    
    jpeg_compressed_t *comp = *compressed;
    comp->width = image->width;
    comp->height = image->height;
    comp->quality_factor = params->quality_factor;
    comp->dct_method = params->dct_method;
    
    /* Prepare scaled quantization tables and reciprocals */
    int32_t quant_luma[64], quant_chroma[64];
    uint32_t recip_luma[64], recip_chroma[64];
    scale_quant_table(Q50_LUMA, params->quality_factor, quant_luma);
    scale_quant_table(Q50_CHROMA, params->quality_factor, quant_chroma);
    compute_reciprocal_table(quant_luma, recip_luma);
    compute_reciprocal_table(quant_chroma, recip_chroma);
    
    int32_t blocks_x = (image->width + 7) / 8;
    int32_t blocks_y = (image->height + 7) / 8;
    comp->num_blocks_y = blocks_x * blocks_y;
    comp->num_blocks_chroma = comp->num_blocks_y;
    
    size_t block_size = comp->num_blocks_y * 64;
    comp->y_coeffs = (int32_t*)CODEC_CALLOC(block_size, sizeof(int32_t));
    comp->y_quantized = (int32_t*)CODEC_CALLOC(block_size, sizeof(int32_t));
    comp->cb_coeffs = (int32_t*)CODEC_CALLOC(block_size, sizeof(int32_t));
    comp->cb_quantized = (int32_t*)CODEC_CALLOC(block_size, sizeof(int32_t));
    comp->cr_coeffs = (int32_t*)CODEC_CALLOC(block_size, sizeof(int32_t));
    comp->cr_quantized = (int32_t*)CODEC_CALLOC(block_size, sizeof(int32_t));
    
    if (!comp->y_coeffs || !comp->y_quantized || 
        !comp->cb_coeffs || !comp->cb_quantized ||
        !comp->cr_coeffs || !comp->cr_quantized) {
        jpeg_free_compressed(comp);
        return JPEG_ERROR_ALLOCATION_FAILED;
    }
    
    int total_pixels = image->width * image->height;
    int32_t *y_ch = (int32_t*)CODEC_CALLOC(total_pixels, sizeof(int32_t));
    int32_t *cb_ch = (int32_t*)CODEC_CALLOC(total_pixels, sizeof(int32_t));
    int32_t *cr_ch = (int32_t*)CODEC_CALLOC(total_pixels, sizeof(int32_t));
    
    if (!y_ch || !cb_ch || !cr_ch) {
        free(y_ch); free(cb_ch); free(cr_ch);
        jpeg_free_compressed(comp);
        return JPEG_ERROR_ALLOCATION_FAILED;
    }
    
    /* Color space conversion - use batch functions for performance */
    if (image->colorspace == JPEG_COLORSPACE_RGB) {
        rgb_to_ycbcr_batch(image->data, y_ch, cb_ch, cr_ch, total_pixels);
    } else {
        /* Grayscale - direct batch copy */
        const uint8_t *src = image->data;
        int32_t *dst_y = y_ch;
        int32_t *dst_cb = cb_ch;
        int32_t *dst_cr = cr_ch;
        int n = total_pixels;
        while (n--) {
            *dst_y++ = (int32_t)(*src++) - 128;
            *dst_cb++ = 0;
            *dst_cr++ = 0;
        }
    }
    
    /* Extract 8x8 blocks from each channel */
    int32_t *y_blks, *cb_blks, *cr_blks, num_blks;
    extract_blocks(y_ch, image->width, image->height, &y_blks, &num_blks);
    extract_blocks(cb_ch, image->width, image->height, &cb_blks, &num_blks);
    extract_blocks(cr_ch, image->width, image->height, &cr_blks, &num_blks);
    
    /* Select DCT function */
    void (*dct_func)(const int32_t*, int32_t*);
    if (params->dct_method == JPEG_DCT_MATRIX) dct_func = dct_matrix_2d;
    else if (params->dct_method == JPEG_DCT_APPROX) dct_func = dct_approx_2d;
    else if (params->dct_method == JPEG_DCT_IDENTITY) dct_func = dct_identity_2d;
    else dct_func = dct_loeffler_2d;
    
    /* Process all blocks - DCT + quantization */
    int32_t *y_in = y_blks, *cb_in = cb_blks, *cr_in = cr_blks;
    int32_t *y_dct = comp->y_coeffs, *cb_dct = comp->cb_coeffs, *cr_dct = comp->cr_coeffs;
    int32_t *y_q = comp->y_quantized, *cb_q = comp->cb_quantized, *cr_q = comp->cr_quantized;
    
    for (int b = 0; b < num_blks; b++) {
        dct_func(y_in, y_dct);
        dct_func(cb_in, cb_dct);
        dct_func(cr_in, cr_dct);
        
        if (params->skip_quantization) {
            memcpy(y_q, y_dct, 64 * sizeof(int32_t));
            memcpy(cb_q, cb_dct, 64 * sizeof(int32_t));
            memcpy(cr_q, cr_dct, 64 * sizeof(int32_t));
        } else {
            /* Fast quantization with reciprocal multiplication */
            quantize_fast(y_dct, quant_luma, recip_luma, y_q);
            quantize_fast(cb_dct, quant_chroma, recip_chroma, cb_q);
            quantize_fast(cr_dct, quant_chroma, recip_chroma, cr_q);
        }
        
        y_in += 64; cb_in += 64; cr_in += 64;
        y_dct += 64; cb_dct += 64; cr_dct += 64;
        y_q += 64; cb_q += 64; cr_q += 64;
    }
    
    free(y_ch); free(cb_ch); free(cr_ch);
    free(y_blks); free(cb_blks); free(cr_blks);
    return JPEG_SUCCESS;
}

jpeg_error_t jpeg_decompress(const jpeg_compressed_t *compressed,
                            jpeg_image_t **image) {
    if (!compressed || !image) return JPEG_ERROR_NULL_POINTER;
    
    *image = (jpeg_image_t*)calloc(1, sizeof(jpeg_image_t));
    if (!*image) return JPEG_ERROR_ALLOCATION_FAILED;
    
    jpeg_image_t *img = *image;
    img->width = compressed->width;
    img->height = compressed->height;
    img->colorspace = JPEG_COLORSPACE_RGB;
    img->data = (uint8_t*)CODEC_CALLOC(img->width * img->height * 3, sizeof(uint8_t));
    
    if (!img->data) {
        free(img);
        return JPEG_ERROR_ALLOCATION_FAILED;
    }
    
    int32_t quant_luma[64], quant_chroma[64];
    scale_quant_table(Q50_LUMA, compressed->quality_factor, quant_luma);
    scale_quant_table(Q50_CHROMA, compressed->quality_factor, quant_chroma);
    
    /* Select IDCT function */
    void (*idct_func)(const int32_t*, int32_t*);
    if (compressed->dct_method == JPEG_DCT_MATRIX) idct_func = idct_matrix_2d;
    else if (compressed->dct_method == JPEG_DCT_APPROX) idct_func = idct_approx_2d;
    else if (compressed->dct_method == JPEG_DCT_IDENTITY) idct_func = idct_identity_2d;
    else idct_func = idct_loeffler_2d;
    
    int32_t num_blks = compressed->num_blocks_y;
    int32_t *y_blks = (int32_t*)CODEC_CALLOC(num_blks * 64, sizeof(int32_t));
    int32_t *cb_blks = (int32_t*)CODEC_CALLOC(num_blks * 64, sizeof(int32_t));
    int32_t *cr_blks = (int32_t*)CODEC_CALLOC(num_blks * 64, sizeof(int32_t));
    
    if (!y_blks || !cb_blks || !cr_blks) {
        free(y_blks); free(cb_blks); free(cr_blks);
        jpeg_free_image(img);
        return JPEG_ERROR_ALLOCATION_FAILED;
    }
    
    /* Dequantization + IDCT for all blocks */
    if (compressed->dct_method == JPEG_DCT_IDENTITY) {
        memcpy(y_blks, compressed->y_quantized, num_blks * 64 * sizeof(int32_t));
        memcpy(cb_blks, compressed->cb_quantized, num_blks * 64 * sizeof(int32_t));
        memcpy(cr_blks, compressed->cr_quantized, num_blks * 64 * sizeof(int32_t));
    } else {
        const int32_t *y_q = compressed->y_quantized;
        const int32_t *cb_q = compressed->cb_quantized;
        const int32_t *cr_q = compressed->cr_quantized;
        int32_t *y_out = y_blks, *cb_out = cb_blks, *cr_out = cr_blks;
        
        for (int b = 0; b < num_blks; b++) {
            int32_t y_dct[64], cb_dct[64], cr_dct[64];
            dequantize(y_q, quant_luma, y_dct);
            dequantize(cb_q, quant_chroma, cb_dct);
            dequantize(cr_q, quant_chroma, cr_dct);
            
            idct_func(y_dct, y_out);
            idct_func(cb_dct, cb_out);
            idct_func(cr_dct, cr_out);
            
            y_q += 64; cb_q += 64; cr_q += 64;
            y_out += 64; cb_out += 64; cr_out += 64;
        }
    }
    
    /* Reconstruct image channels from blocks */
    int total_pixels = img->width * img->height;
    int32_t *y_ch = (int32_t*)CODEC_CALLOC(total_pixels, sizeof(int32_t));
    int32_t *cb_ch = (int32_t*)CODEC_CALLOC(total_pixels, sizeof(int32_t));
    int32_t *cr_ch = (int32_t*)CODEC_CALLOC(total_pixels, sizeof(int32_t));
    
    if (!y_ch || !cb_ch || !cr_ch) {
        free(y_ch); free(cb_ch); free(cr_ch);
        free(y_blks); free(cb_blks); free(cr_blks);
        jpeg_free_image(img);
        *image = NULL;
        return JPEG_ERROR_ALLOCATION_FAILED;
    }
    
    reconstruct_channel(y_blks, num_blks, img->width, img->height, y_ch);
    reconstruct_channel(cb_blks, num_blks, img->width, img->height, cb_ch);
    reconstruct_channel(cr_blks, num_blks, img->width, img->height, cr_ch);
    
    /* Convert YCbCr back to RGB - batch conversion */
    ycbcr_to_rgb_batch(y_ch, cb_ch, cr_ch, img->data, total_pixels);
    
    free(y_ch); free(cb_ch); free(cr_ch);
    free(y_blks); free(cb_blks); free(cr_blks);
    return JPEG_SUCCESS;
}

void jpeg_free_compressed(jpeg_compressed_t *compressed) {
    if (compressed) {
        free(compressed->y_coeffs); free(compressed->y_quantized);
        free(compressed->cb_coeffs); free(compressed->cb_quantized);
        free(compressed->cr_coeffs); free(compressed->cr_quantized);
        free(compressed);
    }
}

void jpeg_free_image(jpeg_image_t *image) {
    if (image) {
        free(image->data);
        free(image);
    }
}

const char* jpeg_version(void) {
    static char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d",
             JPEG_VERSION_MAJOR, JPEG_VERSION_MINOR, JPEG_VERSION_PATCH);
    return version;
}

const char* jpeg_error_string(jpeg_error_t error) {
    switch (error) {
        case JPEG_SUCCESS: return "Success";
        case JPEG_ERROR_NULL_POINTER: return "Null pointer";
        case JPEG_ERROR_INVALID_DIMENSIONS: return "Invalid dimensions";
        case JPEG_ERROR_ALLOCATION_FAILED: return "Allocation failed";
        case JPEG_ERROR_INVALID_METHOD: return "Invalid DCT method";
        default: return "Unknown error";
    }
}