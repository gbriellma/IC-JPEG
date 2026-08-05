/* codec.c -- JPEG compression/decompression pipeline (int32, SCALE=2^13)
 *
 * v2: block-based pipeline, grayscale fast path, subsampling, enum validation.
 */

#include "../include/jpeg_codec.h"
#include "../include/internal.h"
#include <stdlib.h>
#include <string.h>

static void codec_profile_add(uint32_t *elapsed_us, uint32_t *calls,
                              int64_t t0, int64_t t1)
{
    uint64_t sum;

    if (!elapsed_us || !calls) return;
    if (t1 > t0) {
        sum = (uint64_t)*elapsed_us + (uint64_t)(t1 - t0);
        *elapsed_us = (sum > UINT32_MAX) ? UINT32_MAX : (uint32_t)sum;
    }
    if (*calls < UINT32_MAX) {
        (*calls)++;
    }
}

static void codec_profile_dct_call(void (*dct_func)(const int32_t*, int32_t*),
                                   const int32_t *input, int32_t *output,
                                   jpeg_compressed_t *profile)
{
    int64_t t0 = codec_time_us();
    dct_func(input, output);
    codec_profile_add(&profile->dct_kernel_us, &profile->dct_kernel_calls,
                      t0, codec_time_us());
}

static void codec_profile_idct_call(void (*idct_func)(const int32_t*, int32_t*),
                                    const int32_t *input, int32_t *output,
                                    jpeg_compressed_t *profile)
{
    int64_t t0 = codec_time_us();
    idct_func(input, output);
    codec_profile_add(&profile->idct_kernel_us, &profile->idct_kernel_calls,
                      t0, codec_time_us());
}

jpeg_error_t jpeg_compress(const jpeg_image_t *image,
                           const jpeg_params_t *params,
                           jpeg_compressed_t **compressed)
{
    int32_t w, h, bx, by, num_luma, num_chroma;
    int is_gray, skip_q, keep_c;
    jpeg_subsampling_t subsamp;
    size_t luma_elems, chroma_elems, luma_bytes, chroma_bytes;

    if (!image || !params || !compressed) return JPEG_ERROR_NULL_POINTER;
    if (!image->data) return JPEG_ERROR_NULL_POINTER;
    if (image->width <= 0 || image->height <= 0) return JPEG_ERROR_INVALID_DIMENSIONS;
    if (!jpeg_valid_dct_method(params->dct_method)) return JPEG_ERROR_INVALID_METHOD;
    if (!jpeg_valid_colorspace(image->colorspace)) return JPEG_ERROR_INVALID_COLORSPACE;
    if (!jpeg_valid_subsampling(params->subsampling)) return JPEG_ERROR_INVALID_SUBSAMPLING;
    if (!jpeg_valid_quality_factor(params->quality_factor)) return JPEG_ERROR_INVALID_QUALITY;

    w = image->width;
    h = image->height;
    is_gray = (image->colorspace == JPEG_COLORSPACE_GRAYSCALE);
    skip_q  = (params->flags & JPEG_FLAG_SKIP_QUANTIZATION) ||
              (params->dct_method == JPEG_DCT_IDENTITY);
    keep_c  = (params->flags & JPEG_FLAG_KEEP_COEFFS) != 0;
    /* grayscale has no chroma; subsampling param is silently ignored → 444 */
    subsamp = is_gray ? JPEG_SUBSAMP_444 : params->subsampling;

    if (!codec_compute_block_geometry(w, h, image->colorspace, subsamp,
                                      &bx, &by, &num_luma, &num_chroma)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }
    if (!codec_checked_mul_size((size_t)num_luma, 64u, &luma_elems)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }
    if (!codec_checked_mul_size((size_t)num_chroma, 64u, &chroma_elems)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }
    if (!codec_checked_mul_size(luma_elems, sizeof(int32_t), &luma_bytes)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }
    if (!codec_checked_mul_size(chroma_elems, sizeof(int32_t), &chroma_bytes)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }

    *compressed = (jpeg_compressed_t*)codec_alloc_zero_small_bytes(sizeof(jpeg_compressed_t));
    if (!*compressed) return JPEG_ERROR_ALLOCATION_FAILED;

    jpeg_compressed_t *comp = *compressed;
    comp->width          = w;
    comp->height         = h;
    comp->quality_factor = params->quality_factor;
    comp->dct_method     = params->dct_method;
    comp->colorspace     = image->colorspace;
    comp->subsampling    = subsamp;
    comp->flags          = params->flags;
    comp->num_blocks_y      = num_luma;
    comp->num_blocks_chroma = num_chroma;
    comp->dct_kernel_us = 0u;
    comp->idct_kernel_us = 0u;
    comp->dct_kernel_calls = 0u;
    comp->idct_kernel_calls = 0u;

    comp->y_quantized = (int32_t*)codec_alloc_zero_bytes(luma_bytes);
    if (!comp->y_quantized) { jpeg_free_compressed(comp); *compressed = NULL; return JPEG_ERROR_ALLOCATION_FAILED; }

    if (keep_c) {
        comp->y_coeffs = (int32_t*)codec_alloc_zero_bytes(luma_bytes);
        if (!comp->y_coeffs) { jpeg_free_compressed(comp); *compressed = NULL; return JPEG_ERROR_ALLOCATION_FAILED; }
    }

    if (!is_gray) {
        comp->cb_quantized = (int32_t*)codec_alloc_zero_bytes(chroma_bytes);
        comp->cr_quantized = (int32_t*)codec_alloc_zero_bytes(chroma_bytes);
        if (!comp->cb_quantized || !comp->cr_quantized) {
            jpeg_free_compressed(comp); *compressed = NULL; return JPEG_ERROR_ALLOCATION_FAILED;
        }
        if (keep_c) {
            comp->cb_coeffs = (int32_t*)codec_alloc_zero_bytes(chroma_bytes);
            comp->cr_coeffs = (int32_t*)codec_alloc_zero_bytes(chroma_bytes);
            if (!comp->cb_coeffs || !comp->cr_coeffs) {
                jpeg_free_compressed(comp); *compressed = NULL; return JPEG_ERROR_ALLOCATION_FAILED;
            }
        }
    }

    /* Quantization tables are prepared once per frame, before the block loop.
     * For approximate DCTs, diagonal norm factors are absorbed here so the
     * RDCT/Silveira transform kernels remain multiplier-free. */
    int32_t quant_luma[64], quant_chroma[64];
    uint32_t recip_luma[64], recip_chroma[64];
    int class_j = jpeg_method_to_class_j(params->dct_method);
    scale_quant_table(Q50_LUMA, params->quality_factor, quant_luma);
    scale_quant_table(Q50_CHROMA, params->quality_factor, quant_chroma);

    if (params->dct_method == JPEG_DCT_RDCT) {
        apply_approx_norm_correction(quant_luma);
        apply_approx_norm_correction(quant_chroma);
    } else if (class_j == 3 || class_j == 7) {
        apply_class_norm_correction(quant_luma, class_j);
        apply_class_norm_correction(quant_chroma, class_j);
    }

    compute_reciprocal_table(quant_luma, recip_luma);
    compute_reciprocal_table(quant_chroma, recip_chroma);

    void (*dct_func)(const int32_t*, int32_t*);
    switch (params->dct_method) {
        case JPEG_DCT_MATRIX:      dct_func = dct_matrix_2d;       break;
        case JPEG_DCT_RDCT:        dct_func = dct_rdct_2d;         break;
        case JPEG_DCT_SILVEIRA_J3: dct_func = dct_silveira_j3_2d;  break;
        case JPEG_DCT_SILVEIRA_J7: dct_func = dct_silveira_j7_2d;  break;
        case JPEG_DCT_IDENTITY:    dct_func = dct_identity_2d;     break;
        default:                   dct_func = dct_loeffler_2d;     break;
    }

    /* Process luma blocks */
    for (int32_t j = 0; j < by; j++) {
        for (int32_t i = 0; i < bx; i++) {
            CODEC_YIELD_EVERY(j * bx + i, 64);
            int32_t block[64], dct_out[64];
            int32_t b = j * bx + i;

            if (is_gray)
                extract_luma_block_gray(image->data, w, h, i, j, block);
            else
                extract_luma_block_rgb(image->data, w, h, i, j, block);

            codec_profile_dct_call(dct_func, block, dct_out, comp);
            if (keep_c) memcpy(&comp->y_coeffs[b * 64], dct_out, 64 * sizeof(int32_t));
            if (skip_q) memcpy(&comp->y_quantized[b * 64], dct_out, 64 * sizeof(int32_t));
            else        quantize_fast(dct_out, quant_luma, recip_luma, &comp->y_quantized[b * 64]);
        }
    }

    if (is_gray) return JPEG_SUCCESS;

    /* Process chroma blocks */
    if (subsamp == JPEG_SUBSAMP_444) {
        for (int32_t j = 0; j < by; j++) {
            for (int32_t i = 0; i < bx; i++) {
                CODEC_YIELD_EVERY(j * bx + i, 64);
                int32_t cb_blk[64], cr_blk[64], cb_dct[64], cr_dct[64];
                int32_t b = j * bx + i;

                extract_chroma_blocks_444(image->data, w, h, i, j, cb_blk, cr_blk);
                codec_profile_dct_call(dct_func, cb_blk, cb_dct, comp);
                codec_profile_dct_call(dct_func, cr_blk, cr_dct, comp);

                if (keep_c) {
                    memcpy(&comp->cb_coeffs[b * 64], cb_dct, 64 * sizeof(int32_t));
                    memcpy(&comp->cr_coeffs[b * 64], cr_dct, 64 * sizeof(int32_t));
                }
                if (skip_q) {
                    memcpy(&comp->cb_quantized[b * 64], cb_dct, 64 * sizeof(int32_t));
                    memcpy(&comp->cr_quantized[b * 64], cr_dct, 64 * sizeof(int32_t));
                } else {
                    quantize_fast(cb_dct, quant_chroma, recip_chroma, &comp->cb_quantized[b * 64]);
                    quantize_fast(cr_dct, quant_chroma, recip_chroma, &comp->cr_quantized[b * 64]);
                }
            }
        }
    } else if (subsamp == JPEG_SUBSAMP_420) {
        int32_t mcx = codec_ceil_div_pos(w, 16), mcy = codec_ceil_div_pos(h, 16);
        for (int32_t j = 0; j < mcy; j++) {
            for (int32_t i = 0; i < mcx; i++) {
                CODEC_YIELD_EVERY(j * mcx + i, 256);
                int32_t cb_blk[64], cr_blk[64], cb_dct[64], cr_dct[64];
                int32_t b = j * mcx + i;

                extract_chroma_blocks_420(image->data, w, h, i, j, cb_blk, cr_blk);
                codec_profile_dct_call(dct_func, cb_blk, cb_dct, comp);
                codec_profile_dct_call(dct_func, cr_blk, cr_dct, comp);

                if (keep_c) {
                    memcpy(&comp->cb_coeffs[b * 64], cb_dct, 64 * sizeof(int32_t));
                    memcpy(&comp->cr_coeffs[b * 64], cr_dct, 64 * sizeof(int32_t));
                }
                if (skip_q) {
                    memcpy(&comp->cb_quantized[b * 64], cb_dct, 64 * sizeof(int32_t));
                    memcpy(&comp->cr_quantized[b * 64], cr_dct, 64 * sizeof(int32_t));
                } else {
                    quantize_fast(cb_dct, quant_chroma, recip_chroma, &comp->cb_quantized[b * 64]);
                    quantize_fast(cr_dct, quant_chroma, recip_chroma, &comp->cr_quantized[b * 64]);
                }
            }
        }
    } else { /* 4:2:2 */
        int32_t mcx = codec_ceil_div_pos(w, 16);
        for (int32_t j = 0; j < by; j++) {
            for (int32_t i = 0; i < mcx; i++) {
                CODEC_YIELD_EVERY(j * mcx + i, 256);
                int32_t cb_blk[64], cr_blk[64], cb_dct[64], cr_dct[64];
                int32_t b = j * mcx + i;

                extract_chroma_blocks_422(image->data, w, h, i, j, cb_blk, cr_blk);
                codec_profile_dct_call(dct_func, cb_blk, cb_dct, comp);
                codec_profile_dct_call(dct_func, cr_blk, cr_dct, comp);

                if (keep_c) {
                    memcpy(&comp->cb_coeffs[b * 64], cb_dct, 64 * sizeof(int32_t));
                    memcpy(&comp->cr_coeffs[b * 64], cr_dct, 64 * sizeof(int32_t));
                }
                if (skip_q) {
                    memcpy(&comp->cb_quantized[b * 64], cb_dct, 64 * sizeof(int32_t));
                    memcpy(&comp->cr_quantized[b * 64], cr_dct, 64 * sizeof(int32_t));
                } else {
                    quantize_fast(cb_dct, quant_chroma, recip_chroma, &comp->cb_quantized[b * 64]);
                    quantize_fast(cr_dct, quant_chroma, recip_chroma, &comp->cr_quantized[b * 64]);
                }
            }
        }
    }

    return JPEG_SUCCESS;
}

static jpeg_error_t jpeg_decompress_impl(jpeg_compressed_t *compressed,
                                         jpeg_image_t *img)
{
    int32_t w, h, bx, by, expected_luma, expected_chroma;
    int is_gray, skip_q;
    jpeg_subsampling_t subsamp;
    jpeg_compressed_t *profile;
    if (!compressed || !img) return JPEG_ERROR_NULL_POINTER;
    if (!compressed->y_quantized) return JPEG_ERROR_NULL_POINTER;
    if (compressed->width <= 0 || compressed->height <= 0) return JPEG_ERROR_INVALID_DIMENSIONS;
    if (!jpeg_valid_dct_method(compressed->dct_method)) return JPEG_ERROR_INVALID_METHOD;
    if (!jpeg_valid_colorspace(compressed->colorspace)) return JPEG_ERROR_INVALID_COLORSPACE;
    if (!jpeg_valid_subsampling(compressed->subsampling)) return JPEG_ERROR_INVALID_SUBSAMPLING;
    if (!jpeg_valid_quality_factor(compressed->quality_factor)) return JPEG_ERROR_INVALID_QUALITY;

    w = compressed->width;
    h = compressed->height;
    is_gray = (compressed->colorspace == JPEG_COLORSPACE_GRAYSCALE);
    skip_q  = (compressed->flags & JPEG_FLAG_SKIP_QUANTIZATION) ||
              (compressed->dct_method == JPEG_DCT_IDENTITY);
    subsamp = compressed->subsampling;

    if (!codec_compute_block_geometry(w, h, compressed->colorspace, subsamp,
                                      &bx, &by, &expected_luma, &expected_chroma)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }
    if (compressed->num_blocks_y != expected_luma ||
            compressed->num_blocks_chroma != expected_chroma) {
        return JPEG_ERROR_INVALID_FRAME;
    }

    if (!is_gray && (!compressed->cb_quantized || !compressed->cr_quantized))
        return JPEG_ERROR_NULL_POINTER;

    if (!img->data) return JPEG_ERROR_NULL_POINTER;
    profile = compressed;
    profile->idct_kernel_us = 0u;
    profile->idct_kernel_calls = 0u;
    img->width  = w;
    img->height = h;
    img->colorspace = is_gray ? JPEG_COLORSPACE_GRAYSCALE : JPEG_COLORSPACE_RGB;

    /* Same table preparation as encode. This belongs to dequant/setup, not
     * to the arithmetic cost of the approximate inverse DCT kernel. */
    int32_t quant_luma[64], quant_chroma[64];
    int class_j = jpeg_method_to_class_j(compressed->dct_method);
    scale_quant_table(Q50_LUMA, compressed->quality_factor, quant_luma);
    scale_quant_table(Q50_CHROMA, compressed->quality_factor, quant_chroma);

    if (compressed->dct_method == JPEG_DCT_RDCT) {
        apply_approx_norm_correction(quant_luma);
        apply_approx_norm_correction(quant_chroma);
    } else if (class_j == 3 || class_j == 7) {
        apply_class_norm_correction(quant_luma, class_j);
        apply_class_norm_correction(quant_chroma, class_j);
    }

    void (*idct_func)(const int32_t*, int32_t*);
    switch (compressed->dct_method) {
        case JPEG_DCT_MATRIX:      idct_func = idct_matrix_2d;       break;
        case JPEG_DCT_RDCT:        idct_func = idct_rdct_2d;         break;
        case JPEG_DCT_SILVEIRA_J3: idct_func = idct_silveira_j3_2d;  break;
        case JPEG_DCT_SILVEIRA_J7: idct_func = idct_silveira_j7_2d;  break;
        case JPEG_DCT_IDENTITY:    idct_func = idct_identity_2d;     break;
        default:                   idct_func = idct_loeffler_2d;     break;
    }

    if (is_gray) {
        for (int32_t j = 0; j < by; j++) {
            for (int32_t i = 0; i < bx; i++) {
                CODEC_YIELD_EVERY(j * bx + i, 256);
                int32_t b = j * bx + i;
                int32_t y_block[64];
                if (skip_q) {
                    codec_profile_idct_call(idct_func, &compressed->y_quantized[b * 64], y_block, profile);
                } else {
                    int32_t tmp[64];
                    dequantize(&compressed->y_quantized[b * 64], quant_luma, tmp);
                    codec_profile_idct_call(idct_func, tmp, y_block, profile);
                }
                write_block_to_gray(img->data, w, h, i, j, y_block);
            }
        }
        return JPEG_SUCCESS;
    }

    if (subsamp == JPEG_SUBSAMP_444) {
        for (int32_t j = 0; j < by; j++) {
            for (int32_t i = 0; i < bx; i++) {
                CODEC_YIELD_EVERY(j * bx + i, 64);
                int32_t b = j * bx + i;
                int32_t y_block[64], cb_block[64], cr_block[64];
                if (skip_q) {
                    codec_profile_idct_call(idct_func, &compressed->y_quantized[b * 64], y_block, profile);
                    codec_profile_idct_call(idct_func, &compressed->cb_quantized[b * 64], cb_block, profile);
                    codec_profile_idct_call(idct_func, &compressed->cr_quantized[b * 64], cr_block, profile);
                } else {
                    int32_t tmp[64];
                    dequantize(&compressed->y_quantized[b * 64], quant_luma, tmp);
                    codec_profile_idct_call(idct_func, tmp, y_block, profile);
                    dequantize(&compressed->cb_quantized[b * 64], quant_chroma, tmp);
                    codec_profile_idct_call(idct_func, tmp, cb_block, profile);
                    dequantize(&compressed->cr_quantized[b * 64], quant_chroma, tmp);
                    codec_profile_idct_call(idct_func, tmp, cr_block, profile);
                }
                write_block_to_rgb(img->data, w, h, i, j, y_block, cb_block, cr_block);
            }
        }
    } else if (subsamp == JPEG_SUBSAMP_420) {
        int32_t mcx = codec_ceil_div_pos(w, 16), mcy = codec_ceil_div_pos(h, 16);
        for (int32_t mj = 0; mj < mcy; mj++) {
            for (int32_t mi = 0; mi < mcx; mi++) {
                CODEC_YIELD_EVERY(mj * mcx + mi, 64);
                int32_t cidx = mj * mcx + mi;
                int32_t cb_raw[64], cr_raw[64];
                if (skip_q) {
                    codec_profile_idct_call(idct_func, &compressed->cb_quantized[cidx * 64], cb_raw, profile);
                    codec_profile_idct_call(idct_func, &compressed->cr_quantized[cidx * 64], cr_raw, profile);
                } else {
                    int32_t tmp[64];
                    dequantize(&compressed->cb_quantized[cidx * 64], quant_chroma, tmp);
                    codec_profile_idct_call(idct_func, tmp, cb_raw, profile);
                    dequantize(&compressed->cr_quantized[cidx * 64], quant_chroma, tmp);
                    codec_profile_idct_call(idct_func, tmp, cr_raw, profile);
                }
                int32_t cb_tl[64], cb_tr[64], cb_bl[64], cb_br[64];
                int32_t cr_tl[64], cr_tr[64], cr_bl[64], cr_br[64];
                upsample_chroma_420(cb_raw, cb_tl, cb_tr, cb_bl, cb_br);
                upsample_chroma_420(cr_raw, cr_tl, cr_tr, cr_bl, cr_br);

                int32_t lbx = mi * 2, lby = mj * 2;
                struct { int dx, dy; int32_t *cbu, *cru; } quad[4] = {
                    {0,0,cb_tl,cr_tl},{1,0,cb_tr,cr_tr},{0,1,cb_bl,cr_bl},{1,1,cb_br,cr_br}
                };
                for (int q = 0; q < 4; q++) {
                    int32_t li = lbx + quad[q].dx, lj = lby + quad[q].dy;
                    if (li >= bx || lj >= by) continue;
                    int32_t lb = lj * bx + li;
                    int32_t y_block[64];
                    if (skip_q) {
                        codec_profile_idct_call(idct_func, &compressed->y_quantized[lb * 64], y_block, profile);
                    } else {
                        int32_t tmp[64];
                        dequantize(&compressed->y_quantized[lb * 64], quant_luma, tmp);
                        codec_profile_idct_call(idct_func, tmp, y_block, profile);
                    }
                    write_block_to_rgb(img->data, w, h, li, lj, y_block, quad[q].cbu, quad[q].cru);
                }
            }
        }
    } else { /* 4:2:2 */
        int32_t mcx = codec_ceil_div_pos(w, 16);
        for (int32_t j = 0; j < by; j++) {
            for (int32_t mi = 0; mi < mcx; mi++) {
                CODEC_YIELD_EVERY(j * mcx + mi, 256);
                int32_t cidx = j * mcx + mi;
                int32_t cb_raw[64], cr_raw[64];
                if (skip_q) {
                    codec_profile_idct_call(idct_func, &compressed->cb_quantized[cidx * 64], cb_raw, profile);
                    codec_profile_idct_call(idct_func, &compressed->cr_quantized[cidx * 64], cr_raw, profile);
                } else {
                    int32_t tmp[64];
                    dequantize(&compressed->cb_quantized[cidx * 64], quant_chroma, tmp);
                    codec_profile_idct_call(idct_func, tmp, cb_raw, profile);
                    dequantize(&compressed->cr_quantized[cidx * 64], quant_chroma, tmp);
                    codec_profile_idct_call(idct_func, tmp, cr_raw, profile);
                }
                int32_t cb_l[64], cb_r[64], cr_l[64], cr_r[64];
                upsample_chroma_422(cb_raw, cb_l, cb_r);
                upsample_chroma_422(cr_raw, cr_l, cr_r);

                for (int side = 0; side < 2; side++) {
                    int32_t li = mi * 2 + side;
                    if (li >= bx) continue;
                    int32_t lb = j * bx + li;
                    int32_t y_block[64];
                    if (skip_q) {
                        codec_profile_idct_call(idct_func, &compressed->y_quantized[lb * 64], y_block, profile);
                    } else {
                        int32_t tmp[64];
                        dequantize(&compressed->y_quantized[lb * 64], quant_luma, tmp);
                        codec_profile_idct_call(idct_func, tmp, y_block, profile);
                    }
                    write_block_to_rgb(img->data, w, h, li, j, y_block,
                                       side == 0 ? cb_l : cb_r, side == 0 ? cr_l : cr_r);
                }
            }
        }
    }

    return JPEG_SUCCESS;
}

jpeg_error_t jpeg_decompress(jpeg_compressed_t *compressed,
                             jpeg_image_t **image)
{
    int is_gray;
    size_t out_bytes;
    jpeg_error_t err;

    if (!compressed || !image) return JPEG_ERROR_NULL_POINTER;
    if (!compressed->y_quantized) return JPEG_ERROR_NULL_POINTER;
    if (compressed->width <= 0 || compressed->height <= 0) return JPEG_ERROR_INVALID_DIMENSIONS;
    if (!jpeg_valid_colorspace(compressed->colorspace)) return JPEG_ERROR_INVALID_COLORSPACE;
    if (!codec_checked_mul3_size((size_t)compressed->width, (size_t)compressed->height,
                                 (size_t)((compressed->colorspace == JPEG_COLORSPACE_GRAYSCALE) ? 1 : 3),
                                 &out_bytes)) {
        return JPEG_ERROR_INVALID_DIMENSIONS;
    }

    *image = (jpeg_image_t*)codec_alloc_zero_small_bytes(sizeof(jpeg_image_t));
    if (!*image) return JPEG_ERROR_ALLOCATION_FAILED;

    is_gray = (compressed->colorspace == JPEG_COLORSPACE_GRAYSCALE);
    (*image)->width = compressed->width;
    (*image)->height = compressed->height;
    (*image)->colorspace = is_gray ? JPEG_COLORSPACE_GRAYSCALE : JPEG_COLORSPACE_RGB;
    (*image)->data = (uint8_t*)codec_alloc_zero_bytes(out_bytes);
    if (!(*image)->data) {
        CODEC_FREE(*image);
        *image = NULL;
        return JPEG_ERROR_ALLOCATION_FAILED;
    }

    err = jpeg_decompress_impl(compressed, *image);
    if (err != JPEG_SUCCESS) {
        jpeg_free_image(*image);
        *image = NULL;
    }
    return err;
}

jpeg_error_t jpeg_decompress_into(jpeg_compressed_t *compressed,
                                  jpeg_image_t *image)
{
    return jpeg_decompress_impl(compressed, image);
}
