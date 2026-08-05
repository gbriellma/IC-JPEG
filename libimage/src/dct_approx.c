/* dct_approx.c -- RDCT / Cintra-Bayer 2011 approximate DCT (pure int32)
 *
 * The forward transform uses additions only -- independent of SCALE.
 * The inverse transform uses small integer norm factors expressed as
 * additions, preserving the multiplier-free arithmetic profile of the
 * approximation kernels.
 *
 * apply_approx_norm_correction() uses two-step division to stay in int32:
 *   Instead of q * (n_i * n_j) / 1048576, we compute:
 *     step1 = q * n_i / 1024  (with rounding)
 *     step2 = step1 * n_j / 1024  (with rounding)
 *   Max intermediate: 968 * 2896 = 2,803,328 -> fits int32.
 */

#include <stdint.h>
#include "../include/internal.h"

static inline int32_t x2_i32(int32_t x) { return x + x; }
static inline int32_t x3_i32(int32_t x) { return x + x + x; }
static inline int32_t x4_i32(int32_t x)
{
    int32_t x2 = x2_i32(x);
    return x2 + x2;
}
static inline int32_t x6_i32(int32_t x) { return x3_i32(x2_i32(x)); }

/* -- Forward 1D (additions only) --------------------------------------- */
static void dct_1d_stride(const int32_t *src, int s, int32_t *dst)
{
    const int32_t *p = src;
    int32_t x0 = *p; p += s;
    int32_t x1 = *p; p += s;
    int32_t x2 = *p; p += s;
    int32_t x3 = *p; p += s;
    int32_t x4 = *p; p += s;
    int32_t x5 = *p; p += s;
    int32_t x6 = *p; p += s;
    int32_t x7 = *p;

    dst[0] =  x0+x1+x2+x3+x4+x5+x6+x7;
    dst[1] =  x0+x1+x2      -x5-x6-x7;
    dst[2] =  x0      -x3-x4      +x7;
    dst[3] =  x0   -x2-x3+x4+x5   -x7;
    dst[4] =  x0-x1-x2+x3+x4-x5-x6+x7;
    dst[5] =  x0-x1   +x3-x4   +x6-x7;
    dst[6] =    -x1+x2      +x5-x6;
    dst[7] =    -x1+x2-x3+x4-x5+x6;
}

/* -- Inverse 1D -------------------------------------------------------- */
static void idct_1d_stride(const int32_t *src, int32_t *dst, int s)
{
    int32_t a0 = x3_i32(src[0]);
    int32_t a1 = x4_i32(src[1]);
    int32_t a2 = x6_i32(src[2]);
    int32_t a3 = x4_i32(src[3]);
    int32_t a4 = x3_i32(src[4]);
    int32_t a5 = x4_i32(src[5]);
    int32_t a6 = x6_i32(src[6]);
    int32_t a7 = x4_i32(src[7]);

    int32_t *p = dst;
    *p = codec_div_round_symm(a0+a1+a2+a3+a4+a5, 24); p += s;
    *p = codec_div_round_symm(a0+a1-a4-a5-a6-a7, 24); p += s;
    *p = codec_div_round_symm(a0+a1-a3-a4+a6+a7, 24); p += s;
    *p = codec_div_round_symm(a0-a2-a3+a4+a5-a7, 24); p += s;
    *p = codec_div_round_symm(a0-a2+a3+a4-a5+a7, 24); p += s;
    *p = codec_div_round_symm(a0-a1+a3-a4+a6-a7, 24); p += s;
    *p = codec_div_round_symm(a0-a1-a4+a5-a6+a7, 24); p += s;
    *p = codec_div_round_symm(a0-a1+a2-a3+a4-a5, 24);
}

/* apply_approx_norm_correction() is defined in quantization.c */

/* -- Public API --------------------------------------------------------- */

void dct_rdct_2d(const int32_t *in, int32_t *out)
{
    int32_t temp[64];

    const int32_t *row_in = in;
    int32_t *row_tmp = temp;
    for (int y = 0; y < 8; y++, row_in += 8, row_tmp += 8)
        dct_1d_stride(row_in, 1, row_tmp);

    int32_t *out_row = out;
    for (int x = 0; x < 8; x++, out_row += 8)
        dct_1d_stride(temp + x, 8, out_row);

    for (int y = 0; y < 8; y++) {
        int32_t *row = out + (y << 3);
        for (int x = y+1; x < 8; x++) {
            int32_t *a = row + x;
            int32_t *b = out + (x << 3) + y;
            int32_t t = *a;
            *a = *b;
            *b = t;
        }
    }
}

void idct_rdct_2d(const int32_t *in, int32_t *out)
{
    int32_t temp[64], col[8];

    for (int x = 0; x < 8; x++) {
        const int32_t *p = in + x;
        for (int y = 0; y < 8; y++, p += 8) col[y] = *p;
        idct_1d_stride(col, temp + x, 8);
    }

    int32_t *row_out = out;
    const int32_t *row_tmp = temp;
    for (int y = 0; y < 8; y++, row_tmp += 8, row_out += 8)
        idct_1d_stride(row_tmp, row_out, 1);
}
