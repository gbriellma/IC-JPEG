/* dct_class.c -- Silveira et al. 2022 DCT-like transforms
 *
 * Implements the retained instances from Table I of:
 *   T. L. T. da Silveira, D. R. Canterle, D. F. G. Coelho,
 *   V. A. Coutinho, F. M. Bayer, R. J. Cintra,
 *   "A Class of Low-complexity DCT-like Transforms for Image and
 *    Video Coding", 2022.
 *
 * Retained instances:
 *   j=3: a = [1,0,0,1,1,0,0,1]
 *   j=7: a = [1,1/2,1/2,1,1,1/2,1/2,1]
 *
 * Forward kernels are specialized addition-only forms. For j=7 the
 * implementation computes 2*T(a) in the 1-D pass so that the 1/2
 * coefficients from the paper are represented exactly without lossy right
 * shifts. The rows are orthogonal but not unit-norm, so the inverse applies
 * D^-1 T^T with small integer row-scale factors expressed as additions and
 * one final symmetric rounded division.
 */

#include "../include/internal.h"

static inline int32_t x2_i32(int32_t x) { return x + x; }
static inline int32_t x4_i32(int32_t x)
{
    int32_t x2 = x2_i32(x);
    return x2 + x2;
}
static inline int32_t x8_i32(int32_t x)
{
    int32_t x4 = x4_i32(x);
    return x4 + x4;
}
static inline int32_t x9_i32(int32_t x) { return x8_i32(x) + x; }
static inline int32_t x16_i32(int32_t x)
{
    int32_t x8 = x8_i32(x);
    return x8 + x8;
}
static inline int32_t x18_i32(int32_t x) { return x16_i32(x) + x2_i32(x); }

static void transpose_8x8(int32_t *m)
{
    for (int y = 0; y < 8; y++) {
        int32_t *row = m + (y << 3);
        for (int x = y + 1; x < 8; x++) {
            int32_t *a = row + x;
            int32_t *b = m + (x << 3) + y;
            int32_t t = *a;
            *a = *b;
            *b = t;
        }
    }
}

static void dct_j3_1d_stride(const int32_t *src, int s, int32_t *dst)
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

    dst[0] = x0+x1+x2+x3+x4+x5+x6+x7;
    dst[1] = x0+x1-x6-x7;
    dst[2] = x0-x3-x4+x7;
    dst[3] = -x2-x3+x4+x5;
    dst[4] = x0-x1-x2+x3+x4-x5-x6+x7;
    dst[5] = x0-x1+x6-x7;
    dst[6] = -x1+x2+x5-x6;
    dst[7] = x2-x3+x4-x5;
}

static void idct_j3_1d_stride(const int32_t *src, int32_t *dst, int s)
{
    int32_t z0 = src[0];
    int32_t z1 = x2_i32(src[1]);
    int32_t z2 = x2_i32(src[2]);
    int32_t z3 = x2_i32(src[3]);
    int32_t z4 = src[4];
    int32_t z5 = x2_i32(src[5]);
    int32_t z6 = x2_i32(src[6]);
    int32_t z7 = x2_i32(src[7]);

    int32_t *p = dst;
    *p = codec_div_round_symm(z0+z1+z2+z4+z5, 8); p += s;
    *p = codec_div_round_symm(z0+z1-z4-z5-z6, 8); p += s;
    *p = codec_div_round_symm(z0-z3-z4+z6+z7, 8); p += s;
    *p = codec_div_round_symm(z0-z2-z3+z4-z7, 8); p += s;
    *p = codec_div_round_symm(z0-z2+z3+z4+z7, 8); p += s;
    *p = codec_div_round_symm(z0+z3-z4+z6-z7, 8); p += s;
    *p = codec_div_round_symm(z0-z1-z4+z5-z6, 8); p += s;
    *p = codec_div_round_symm(z0-z1+z2+z4-z5, 8);
}

static void dct_j7_1d_stride(const int32_t *src, int s, int32_t *dst)
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

    /* Implements 2*T(a) for the j=7 instance from Table 1. Scaling by
     * two removes the rational 1/2 coefficients without truncation. */
    dst[0] = x2_i32(x0+x1+x2+x3+x4+x5+x6+x7);
    dst[1] = x2_i32(x0) + x2_i32(x1) + x2
           - x5 - x2_i32(x6) - x2_i32(x7);
    dst[2] = x2_i32(x0-x3-x4+x7);
    dst[3] = x0 - x2_i32(x2) - x2_i32(x3)
           + x2_i32(x4) + x2_i32(x5) - x7;
    dst[4] = x2_i32(x0-x1-x2+x3+x4-x5-x6+x7);
    dst[5] = x2_i32(x0) - x2_i32(x1) + x3
           - x4 + x2_i32(x6) - x2_i32(x7);
    dst[6] = x2_i32(-x1+x2+x5-x6);
    dst[7] = -x1 + x2_i32(x2) - x2_i32(x3)
           + x2_i32(x4) - x2_i32(x5) + x6;
}

static void idct_j7_1d_stride(const int32_t *src, int32_t *dst, int s)
{
    int32_t z0 = x9_i32(src[0]);
    int32_t z1 = x16_i32(src[1]);
    int32_t z2 = x18_i32(src[2]);
    int32_t z3 = x16_i32(src[3]);
    int32_t z4 = x9_i32(src[4]);
    int32_t z5 = x16_i32(src[5]);
    int32_t z6 = x18_i32(src[6]);
    int32_t z7 = x16_i32(src[7]);

    int32_t *p = dst;
    *p = codec_div_round_symm(z0+z1+z2+(z3/2)+z4+z5, 144); p += s;
    *p = codec_div_round_symm(z0+z1-z4-z5-z6-(z7/2), 144); p += s;
    *p = codec_div_round_symm(z0+(z1/2)-z3-z4+z6+z7, 144); p += s;
    *p = codec_div_round_symm(z0-z2-z3+z4+(z5/2)-z7, 144); p += s;
    *p = codec_div_round_symm(z0-z2+z3+z4-(z5/2)+z7, 144); p += s;
    *p = codec_div_round_symm(z0-(z1/2)+z3-z4+z6-z7, 144); p += s;
    *p = codec_div_round_symm(z0-z1-z4+z5-z6+(z7/2), 144); p += s;
    *p = codec_div_round_symm(z0-z1+z2-(z3/2)+z4-z5, 144);
}

#define DEFINE_SILVEIRA(N)                                                     \
    void dct_silveira_j##N##_2d(const int32_t *in, int32_t *out)               \
    {                                                                          \
        int32_t tmp[64];                                                       \
        const int32_t *row_in = in;                                            \
        int32_t *row_tmp = tmp;                                                \
        for (int y = 0; y < 8; y++, row_in += 8, row_tmp += 8)                 \
            dct_j##N##_1d_stride(row_in, 1, row_tmp);                         \
        int32_t *out_row = out;                                                \
        for (int x = 0; x < 8; x++, out_row += 8)                              \
            dct_j##N##_1d_stride(tmp + x, 8, out_row);                        \
        transpose_8x8(out);                                                    \
    }                                                                          \
                                                                               \
    void idct_silveira_j##N##_2d(const int32_t *in, int32_t *out)              \
    {                                                                          \
        int32_t tmp[64], col[8];                                               \
        for (int x = 0; x < 8; x++) {                                          \
            const int32_t *p = in + x;                                         \
            for (int y = 0; y < 8; y++, p += 8) col[y] = *p;                  \
            idct_j##N##_1d_stride(col, tmp + x, 8);                           \
        }                                                                      \
        const int32_t *row_tmp = tmp;                                          \
        int32_t *row_out = out;                                                \
        for (int y = 0; y < 8; y++, row_tmp += 8, row_out += 8)                \
            idct_j##N##_1d_stride(row_tmp, row_out, 1);                       \
    }

DEFINE_SILVEIRA(3)
DEFINE_SILVEIRA(7)
