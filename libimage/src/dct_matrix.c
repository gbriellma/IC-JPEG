/* dct_matrix.c -- Matrix DCT (orthonormal float reference)
 *
 * CF[k][n] = alpha_k * cos(pi*k*(2n+1)/16)
 *   alpha_0 = 1/sqrt(8),  alpha_k = 0.5 for k > 0
 *
 * Forward and inverse are simple float dot products with the same matrix
 * (it is orthonormal: inverse = transpose). Outputs are rounded to int32_t
 * via roundf(). The public interface (int32_t in/out) is unchanged.
 */

#include <math.h>
#include "../include/internal.h"

static const float CF[8][8] = {
    { 0.3535533906f,  0.3535533906f,  0.3535533906f,  0.3535533906f,  0.3535533906f,  0.3535533906f,  0.3535533906f,  0.3535533906f },
    { 0.4903926402f,  0.4157348062f,  0.2777851165f,  0.0975451610f, -0.0975451610f, -0.2777851165f, -0.4157348062f, -0.4903926402f },
    { 0.4619397663f,  0.1913417162f, -0.1913417162f, -0.4619397663f, -0.4619397663f, -0.1913417162f,  0.1913417162f,  0.4619397663f },
    { 0.4157348062f, -0.0975451610f, -0.4903926402f, -0.2777851165f,  0.2777851165f,  0.4903926402f,  0.0975451610f, -0.4157348062f },
    { 0.3535533906f, -0.3535533906f, -0.3535533906f,  0.3535533906f,  0.3535533906f, -0.3535533906f, -0.3535533906f,  0.3535533906f },
    { 0.2777851165f, -0.4903926402f,  0.0975451610f,  0.4157348062f, -0.4157348062f, -0.0975451610f,  0.4903926402f, -0.2777851165f },
    { 0.1913417162f, -0.4619397663f,  0.4619397663f, -0.1913417162f, -0.1913417162f,  0.4619397663f, -0.4619397663f,  0.1913417162f },
    { 0.0975451610f, -0.2777851165f,  0.4157348062f, -0.4903926402f,  0.4903926402f, -0.4157348062f,  0.2777851165f, -0.0975451610f },
};

/* -- Forward 1D DCT with stride ---------------------------------------- */
static void dct_1d_stride(const int32_t *src, int s, int32_t *dst)
{
    for (int k = 0; k < 8; k++) {
        float sum = 0.0f;
        for (int n = 0; n < 8; n++)
            sum += (float)src[n*s] * CF[k][n];
        dst[k] = (int32_t)roundf(sum);
    }
}

/* -- Inverse 1D DCT with stride ---------------------------------------- */
static void idct_1d_stride(const int32_t *src, int32_t *dst, int s)
{
    for (int n = 0; n < 8; n++) {
        float sum = 0.0f;
        for (int k = 0; k < 8; k++)
            sum += (float)src[k] * CF[k][n];
        dst[n*s] = (int32_t)roundf(sum);
    }
}

/* -- Public API --------------------------------------------------------- */

void dct_matrix_1d(const int32_t *in, int32_t *out)
{
    dct_1d_stride(in, 1, out);
}

void idct_matrix_1d(const int32_t *in, int32_t *out)
{
    idct_1d_stride(in, out, 1);
}

void dct_matrix_2d(const int32_t *in, int32_t *out)
{
    int32_t temp[64];

    for (int y = 0; y < 8; y++)
        dct_1d_stride(in + y*8, 1, temp + y*8);

    for (int x = 0; x < 8; x++)
        dct_1d_stride(temp + x, 8, out + x*8);

    for (int y = 0; y < 8; y++)
        for (int x = y+1; x < 8; x++) {
            int32_t t = out[y*8+x];
            out[y*8+x] = out[x*8+y];
            out[x*8+y] = t;
        }
}

void idct_matrix_2d(const int32_t *in, int32_t *out)
{
    int32_t temp[64], col[8];

    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) col[y] = in[y*8+x];
        idct_1d_stride(col, temp + x, 8);
    }

    for (int y = 0; y < 8; y++)
        idct_1d_stride(temp + y*8, out + y*8, 1);
}
