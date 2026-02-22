/* dct_matrix.c - Reference DCT using matrix multiplication (64 mults) */

#include "../include/internal.h"

#define SCALE 2048
#define NORM_0 724   /* 1/sqrt(8) * 2048 */
#define NORM_K 1024  /* sqrt(2/8) * 2048 */
#define SCALE_SQ ((int64_t)SCALE * SCALE)

/* Signed integer division with rounding to nearest */
static inline int32_t div_round(int64_t num, int64_t den) {
    if (num >= 0)
        return (int32_t)((num + den / 2) / den);
    else
        return (int32_t)((num - den / 2) / den);
}

/* DCT coefficients: cos(pi*k*(2n+1)/16) * 2048 */
static const int32_t C[8][8] = {
    { 2048,  2048,  2048,  2048,  2048,  2048,  2048,  2048},
    { 2009,  1703,  1138,   400,  -400, -1138, -1703, -2009},
    { 1892,   784,  -784, -1892, -1892,  -784,   784,  1892},
    { 1703,  -400, -2009, -1138,  1138,  2009,   400, -1703},
    { 1448, -1448, -1448,  1448,  1448, -1448, -1448,  1448},
    { 1138, -2009,   400,  1703, -1703,  -400,  2009, -1138},
    {  784, -1892,  1892,  -784,  -784,  1892, -1892,   784},
    {  400, -1138,  1703, -2009,  2009, -1703,  1138,  -400}
};

/* Pre-scaled norm factors per coefficient index */
static const int32_t NORM[8] = {NORM_0, NORM_K, NORM_K, NORM_K, NORM_K, NORM_K, NORM_K, NORM_K};

/* Forward 1D DCT with stride - read from src[i*stride], write contiguous */
static void dct_1d_stride(const int32_t *src, int stride, int32_t *dst) {
    for (int k = 0; k < 8; k++) {
        const int32_t *c = C[k];
        int64_t sum = (int64_t)src[0] * c[0]
                    + (int64_t)src[stride] * c[1]
                    + (int64_t)src[2*stride] * c[2]
                    + (int64_t)src[3*stride] * c[3]
                    + (int64_t)src[4*stride] * c[4]
                    + (int64_t)src[5*stride] * c[5]
                    + (int64_t)src[6*stride] * c[6]
                    + (int64_t)src[7*stride] * c[7];
        dst[k] = div_round(sum * NORM[k], SCALE_SQ);
    }
}

/* Inverse 1D DCT with stride - read contiguous, write to dst[i*stride] */
static void idct_1d_stride(const int32_t *src, int32_t *dst, int stride) {
    for (int n = 0; n < 8; n++) {
        int64_t sum = (int64_t)src[0] * NORM[0] * C[0][n]
                    + (int64_t)src[1] * NORM[1] * C[1][n]
                    + (int64_t)src[2] * NORM[2] * C[2][n]
                    + (int64_t)src[3] * NORM[3] * C[3][n]
                    + (int64_t)src[4] * NORM[4] * C[4][n]
                    + (int64_t)src[5] * NORM[5] * C[5][n]
                    + (int64_t)src[6] * NORM[6] * C[6][n]
                    + (int64_t)src[7] * NORM[7] * C[7][n];
        dst[n * stride] = div_round(sum, SCALE_SQ);
    }
}

/* Forward 1D DCT - contiguous (wrapper) */
void dct_matrix_1d(const int32_t *in, int32_t *out) {
    dct_1d_stride(in, 1, out);
}

/* Inverse 1D DCT - contiguous (wrapper) */
void idct_matrix_1d(const int32_t *in, int32_t *out) {
    idct_1d_stride(in, out, 1);
}

/* Forward 2D DCT - row-column with stride */
void dct_matrix_2d(const int32_t *in, int32_t *out) {
    int32_t temp[64];
    
    /* Transform rows: stride=1 */
    for (int y = 0; y < 8; y++)
        dct_1d_stride(in + y*8, 1, temp + y*8);
    
    /* Transform columns: stride=8, write to temp col storage */
    for (int x = 0; x < 8; x++)
        dct_1d_stride(temp + x, 8, out + x*8);
    
    /* Transpose result */
    for (int y = 0; y < 8; y++) {
        for (int x = y + 1; x < 8; x++) {
            int32_t t = out[y*8 + x];
            out[y*8 + x] = out[x*8 + y];
            out[x*8 + y] = t;
        }
    }
}

/* Inverse 2D DCT - column-row with stride */
void idct_matrix_2d(const int32_t *in, int32_t *out) {
    int32_t temp[64];
    int32_t col[8];
    
    /* Transform columns first */
    for (int x = 0; x < 8; x++) {
        /* Read column with stride */
        for (int y = 0; y < 8; y++) col[y] = in[y*8 + x];
        idct_1d_stride(col, temp + x, 8);
    }
    
    /* Transform rows - write directly */
    for (int y = 0; y < 8; y++)
        idct_1d_stride(temp + y*8, out + y*8, 1);
}