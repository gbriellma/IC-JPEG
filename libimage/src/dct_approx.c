/* dct_approx.c - Multiplierless DCT approximation (Cintra-Bayer 2011) */

#include "../include/internal.h"

/* Forward 1D Approximate DCT with stride - uses only additions */
static void dct_1d_stride(const int32_t *src, int s, int32_t *dst) {
    int32_t x0 = src[0], x1 = src[s], x2 = src[2*s], x3 = src[3*s];
    int32_t x4 = src[4*s], x5 = src[5*s], x6 = src[6*s], x7 = src[7*s];
    dst[0] = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;
    dst[1] = x0 + x1 + x2 - x5 - x6 - x7;
    dst[2] = x0 - x3 - x4 + x7;
    dst[3] = x0 - x2 - x3 + x4 + x5 - x7;
    dst[4] = x0 - x1 - x2 + x3 + x4 - x5 - x6 + x7;
    dst[5] = x0 - x1 + x3 - x4 + x6 - x7;
    dst[6] = -x1 + x2 + x5 - x6;
    dst[7] = -x1 + x2 - x3 + x4 - x5 + x6;
}

/* Inverse 1D Approximate DCT with stride
 * 
 * The forward matrix T has rows with different squared norms:
 *   ||row_k||^2 = {8, 6, 4, 6, 8, 6, 4, 6} for k = 0..7
 * 
 * The exact inverse is: T_inv = T^T @ diag(1/||row_k||^2)
 * Using common denominator 24 (LCM of 8,6,4):
 *   x[n] = (sum of T^T[n][k] * (24/norm_k^2) * y[k]) / 24
 * 
 * Scaling factors per coefficient: 24/8=3, 24/6=4, 24/4=6, 24/6=4, 24/8=3, 24/6=4, 24/4=6, 24/6=4
 */
static void idct_1d_stride(const int32_t *src, int32_t *dst, int s) {
    /* Pre-scale each coefficient by its norm factor to avoid per-output division */
    int32_t a0 = src[0] * 3;   /* norm^2 = 8, scale = 24/8 = 3 */
    int32_t a1 = src[1] * 4;   /* norm^2 = 6, scale = 24/6 = 4 */
    int32_t a2 = src[2] * 6;   /* norm^2 = 4, scale = 24/4 = 6 */
    int32_t a3 = src[3] * 4;   /* norm^2 = 6, scale = 24/6 = 4 */
    int32_t a4 = src[4] * 3;   /* norm^2 = 8, scale = 24/8 = 3 */
    int32_t a5 = src[5] * 4;   /* norm^2 = 6, scale = 24/6 = 4 */
    int32_t a6 = src[6] * 6;   /* norm^2 = 4, scale = 24/4 = 6 */
    int32_t a7 = src[7] * 4;   /* norm^2 = 6, scale = 24/6 = 4 */
    
    /* T^T[n][k] * scaled_coeff, divided by 24
     * Rounding: add 12 (= 24/2) before dividing for proper rounding */
    dst[0]   = (a0 + a1 + a2 + a3 + a4 + a5 + 12) / 24;
    dst[s]   = (a0 + a1 - a4 - a5 - a6 - a7 + 12) / 24;
    dst[2*s] = (a0 + a1 - a3 - a4 + a6 + a7 + 12) / 24;
    dst[3*s] = (a0 - a2 - a3 + a4 + a5 - a7 + 12) / 24;
    dst[4*s] = (a0 - a2 + a3 + a4 - a5 + a7 + 12) / 24;
    dst[5*s] = (a0 - a1 + a3 - a4 + a6 - a7 + 12) / 24;
    dst[6*s] = (a0 - a1 - a4 + a5 - a6 + a7 + 12) / 24;
    dst[7*s] = (a0 - a1 + a2 - a3 + a4 - a5 + 12) / 24;
}

/* Forward 1D - contiguous (wrapper) */
void dct_approx_1d(const int32_t *in, int32_t *out) {
    dct_1d_stride(in, 1, out);
}

/* Inverse 1D - contiguous (wrapper) */
void idct_approx_1d(const int32_t *in, int32_t *out) {
    idct_1d_stride(in, out, 1);
}

/* Forward 2D DCT - row-column with stride */
void dct_approx_2d(const int32_t *in, int32_t *out) {
    int32_t temp[64];
    
    /* Transform rows: stride=1 */
    for (int y = 0; y < 8; y++)
        dct_1d_stride(in + y*8, 1, temp + y*8);
    
    /* Transform columns: stride=8 */
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
void idct_approx_2d(const int32_t *in, int32_t *out) {
    int32_t temp[64];
    int32_t col[8];
    
    /* Transform columns first */
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) col[y] = in[y*8 + x];
        idct_1d_stride(col, temp + x, 8);
    }
    
    /* Transform rows */
    for (int y = 0; y < 8; y++)
        idct_1d_stride(temp + y*8, out + y*8, 1);
}