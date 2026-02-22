/* dct_loeffler.c - Fast DCT with 11 multiplications (Loeffler 1989) */

#include "../include/internal.h"

/* Local trig constants (use internal.h values) */
#define LF_C1 C1
#define LF_S1 S1
#define LF_C3 C3
#define LF_S3 S3
#define LF_C6 C6
#define LF_S6 S6
#define LF_SQRT2 SQRT_2
#define LF_SCALE SCALE_CONST

/* Signed integer division with rounding to nearest */
static inline int32_t div_round(int64_t num, int64_t den) {
    if (num >= 0)
        return (int32_t)((num + den / 2) / den);
    else
        return (int32_t)((num - den / 2) / den);
}

/* Forward 1D DCT with stride - read from src[i*stride], write contiguous */
static void dct_1d_stride(const int32_t *src, int stride, int32_t *dst) {
    int64_t s07 = src[0] + src[7*stride], d07 = src[0] - src[7*stride];
    int64_t s16 = src[stride] + src[6*stride], d16 = src[stride] - src[6*stride];
    int64_t s25 = src[2*stride] + src[5*stride], d25 = src[2*stride] - src[5*stride];
    int64_t s34 = src[3*stride] + src[4*stride], d34 = src[3*stride] - src[4*stride];
    
    int64_t e0 = s07 + s34, e3 = s07 - s34;
    int64_t e1 = s16 + s25, e2 = s16 - s25;
    int64_t o0 = d07 + d34, o1 = d16 + d25, o2 = d16 - d25, o3 = d07 - d34;
    
    dst[0] = div_round((e0 + e1) * LF_SCALE, (int64_t)LF_SQRT2 * 2);
    dst[4] = div_round((e0 - e1) * LF_SCALE, (int64_t)LF_SQRT2 * 2);
    dst[2] = div_round(LF_C6 * e2 + LF_S6 * e3, (int64_t)LF_SCALE * 2);
    dst[6] = div_round(-LF_S6 * e2 + LF_C6 * e3, (int64_t)LF_SCALE * 2);
    dst[1] = div_round(LF_C3*o0 + LF_C1*o1 + LF_S1*o2 + LF_S3*o3, (int64_t)LF_SQRT2 * 2);
    dst[3] = div_round(LF_S1*o0 - LF_C3*o1 + LF_S3*o2 + LF_C1*o3, (int64_t)LF_SQRT2 * 2);
    dst[5] = div_round(LF_C1*o0 - LF_S3*o1 - LF_C3*o2 - LF_S1*o3, (int64_t)LF_SQRT2 * 2);
    dst[7] = div_round(-LF_S3*o0 + LF_S1*o1 - LF_C1*o2 + LF_C3*o3, (int64_t)LF_SQRT2 * 2);
}
/* Inverse 1D DCT with stride - read contiguous, write to dst[i*stride] */
static void idct_1d_stride(const int32_t *src, int32_t *dst, int stride) {
    int64_t Z0 = src[0]*2, Z1 = src[1]*2, Z2 = src[2]*2, Z3 = src[3]*2;
    int64_t Z4 = src[4]*2, Z5 = src[5]*2, Z6 = src[6]*2, Z7 = src[7]*2;
    
    /* Even part - DC/AC through cos rotation */
    int64_t t0 = Z0 * LF_SQRT2 / LF_SCALE, t4 = Z4 * LF_SQRT2 / LF_SCALE;
    int64_t e0 = (t0 + t4) / 2, e1 = (t0 - t4) / 2;
    int64_t e2 = (LF_C6*Z2 - LF_S6*Z6) / LF_SCALE;
    int64_t e3 = (LF_S6*Z2 + LF_C6*Z6) / LF_SCALE;
    int64_t s07 = (e0+e3)/2, s34 = (e0-e3)/2, s16 = (e1+e2)/2, s25 = (e1-e2)/2;
    
    /* Odd part - transposed rotation matrix from forward */
    int64_t o0 = (LF_C3*Z1 + LF_S1*Z3 + LF_C1*Z5 - LF_S3*Z7) / LF_SQRT2;
    int64_t o1 = (LF_C1*Z1 - LF_C3*Z3 - LF_S3*Z5 + LF_S1*Z7) / LF_SQRT2;
    int64_t o2 = (LF_S1*Z1 + LF_S3*Z3 - LF_C3*Z5 - LF_C1*Z7) / LF_SQRT2;
    int64_t o3 = (LF_S3*Z1 + LF_C1*Z3 - LF_S1*Z5 + LF_C3*Z7) / LF_SQRT2;
    int64_t d07 = (o0+o3)/2, d34 = (o0-o3)/2, d16 = (o1+o2)/2, d25 = (o1-o2)/2;
    
    /* Final butterfly with rounding */
    dst[0]        = (int32_t)div_round(s07+d07, 2);
    dst[7*stride] = (int32_t)div_round(s07-d07, 2);
    dst[stride]   = (int32_t)div_round(s16+d16, 2);
    dst[6*stride] = (int32_t)div_round(s16-d16, 2);
    dst[2*stride] = (int32_t)div_round(s25+d25, 2);
    dst[5*stride] = (int32_t)div_round(s25-d25, 2);
    dst[3*stride] = (int32_t)div_round(s34+d34, 2);
    dst[4*stride] = (int32_t)div_round(s34-d34, 2);
}

/* Forward 1D DCT - contiguous input/output (wrapper) */
void dct_loeffler_1d(const int32_t *in, int32_t *out) {
    dct_1d_stride(in, 1, out);
}

/* Inverse 1D DCT - contiguous input/output (wrapper) */
void idct_loeffler_1d(const int32_t *in, int32_t *out) {
    idct_1d_stride(in, out, 1);
}

/* Forward 2D DCT - row-column with stride, single temp buffer */
void dct_loeffler_2d(const int32_t *in, int32_t *out) {
    int32_t temp[64];
    
    /* Transform rows: stride=1 for rows, write to temp */
    for (int y = 0; y < 8; y++)
        dct_1d_stride(in + y*8, 1, temp + y*8);
    
    /* Transform columns: stride=8 for columns, write directly to out */
    for (int x = 0; x < 8; x++)
        dct_1d_stride(temp + x, 8, out + x*8);
    
    /* Transpose output (column results stored row-wise) */
    for (int y = 0; y < 8; y++) {
        for (int x = y + 1; x < 8; x++) {
            int32_t t = out[y*8 + x];
            out[y*8 + x] = out[x*8 + y];
            out[x*8 + y] = t;
        }
    }
}

/* Inverse 2D DCT - column-row with stride */
void idct_loeffler_2d(const int32_t *in, int32_t *out) {
    int32_t temp[64];
    int32_t col[8];
    
    /* Transform columns first */
    for (int x = 0; x < 8; x++) {
        /* Read column with stride */
        for (int y = 0; y < 8; y++) col[y] = in[y*8 + x];
        idct_1d_stride(col, temp + x, 8);
    }
    
    /* Transform rows - write directly to output */
    for (int y = 0; y < 8; y++)
        idct_1d_stride(temp + y*8, out + y*8, 1);
}
