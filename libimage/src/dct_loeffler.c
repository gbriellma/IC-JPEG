/* dct_loeffler.c -- Loeffler 1989 DCT (float, single-precision)
 *
 * Exact trig constants replace the Q13 integer approximations.
 * All butterfly arithmetic runs in float; outputs are rounded to int32_t
 * via roundf(). The public interface (int32_t in/out) is unchanged.
 *
 * Forward normalization matches the orthonormal DCT-II:
 *   X[0] = sum(x) / (2*sqrt(2))          factor INV_2SQRT2
 *   X[2] = (C6*e2 + S6*e3) / 2           factor 0.5
 *   X[1..7 odd] = butterfly / (2*sqrt(2)) factor INV_2SQRT2
 *
 * Inverse: the structure mirrors the forward, divided by 8 at output.
 */

#include <math.h>
#include "../include/internal.h"

static const float C1F = 0.9807852804f;   /* cos(  pi/16) */
static const float S1F = 0.1950903220f;   /* sin(  pi/16) */
static const float C3F = 0.8314696123f;   /* cos(3*pi/16) */
static const float S3F = 0.5555702330f;   /* sin(3*pi/16) */
static const float C6F = 0.3826834324f;   /* cos(6*pi/16) */
static const float S6F = 0.9238795325f;   /* sin(6*pi/16) */
static const float SQRT2F     = 1.4142135624f;
static const float INV_2SQRT2 = 0.3535533906f;  /* 1/(2*sqrt(2)) */

/* -- Forward 1D DCT with stride ---------------------------------------- */
static void dct_1d_stride(const int32_t *src, int s, int32_t *dst)
{
    float s07 = src[0] + src[7*s], d07 = src[0] - src[7*s];
    float s16 = src[s] + src[6*s], d16 = src[s] - src[6*s];
    float s25 = src[2*s] + src[5*s], d25 = src[2*s] - src[5*s];
    float s34 = src[3*s] + src[4*s], d34 = src[3*s] - src[4*s];

    float e0 = s07 + s34, e1 = s16 + s25;
    float e2 = s16 - s25, e3 = s07 - s34;

    dst[0] = (int32_t)roundf((e0 + e1) * INV_2SQRT2);
    dst[4] = (int32_t)roundf((e0 - e1) * INV_2SQRT2);
    dst[2] = (int32_t)roundf((C6F * e2 + S6F * e3) * 0.5f);
    dst[6] = (int32_t)roundf((-S6F * e2 + C6F * e3) * 0.5f);

    float o0 = d07 + d34, o1 = d16 + d25;
    float o2 = d16 - d25, o3 = d07 - d34;

    dst[1] = (int32_t)roundf((C3F*o0 + C1F*o1 + S1F*o2 + S3F*o3) * INV_2SQRT2);
    dst[3] = (int32_t)roundf((S1F*o0 - C3F*o1 + S3F*o2 + C1F*o3) * INV_2SQRT2);
    dst[5] = (int32_t)roundf((C1F*o0 - S3F*o1 - C3F*o2 - S1F*o3) * INV_2SQRT2);
    dst[7] = (int32_t)roundf((-S3F*o0 + S1F*o1 - C1F*o2 + C3F*o3) * INV_2SQRT2);
}

/* -- Inverse 1D DCT with stride ---------------------------------------- */
static void idct_1d_stride(const int32_t *src, int32_t *dst, int s)
{
    float z0=src[0]*2.0f, z1=src[1]*2.0f, z2=src[2]*2.0f, z3=src[3]*2.0f;
    float z4=src[4]*2.0f, z5=src[5]*2.0f, z6=src[6]*2.0f, z7=src[7]*2.0f;

    float t0 = z0 * SQRT2F, t4 = z4 * SQRT2F;
    float e0_2 = t0 + t4, e1_2 = t0 - t4;
    float e2_2 = 2.0f * (C6F * z2 - S6F * z6);
    float e3_2 = 2.0f * (S6F * z2 + C6F * z6);

    float s07_4 = e0_2 + e3_2, s34_4 = e0_2 - e3_2;
    float s16_4 = e1_2 + e2_2, s25_4 = e1_2 - e2_2;

    float n0 = C3F*z1 + S1F*z3 + C1F*z5 - S3F*z7;
    float n1 = C1F*z1 - C3F*z3 - S3F*z5 + S1F*z7;
    float n2 = S1F*z1 + S3F*z3 - C3F*z5 - C1F*z7;
    float n3 = S3F*z1 + C1F*z3 - S1F*z5 + C3F*z7;

    float d07_4 = (n0 + n3) * SQRT2F;
    float d34_4 = (n0 - n3) * SQRT2F;
    float d16_4 = (n1 + n2) * SQRT2F;
    float d25_4 = (n1 - n2) * SQRT2F;

    dst[0]   = (int32_t)roundf((s07_4 + d07_4) / 8.0f);
    dst[7*s] = (int32_t)roundf((s07_4 - d07_4) / 8.0f);
    dst[s]   = (int32_t)roundf((s16_4 + d16_4) / 8.0f);
    dst[6*s] = (int32_t)roundf((s16_4 - d16_4) / 8.0f);
    dst[2*s] = (int32_t)roundf((s25_4 + d25_4) / 8.0f);
    dst[5*s] = (int32_t)roundf((s25_4 - d25_4) / 8.0f);
    dst[3*s] = (int32_t)roundf((s34_4 + d34_4) / 8.0f);
    dst[4*s] = (int32_t)roundf((s34_4 - d34_4) / 8.0f);
}

/* -- Public API --------------------------------------------------------- */

void dct_loeffler_1d(const int32_t *in, int32_t *out)
    { dct_1d_stride(in, 1, out); }

void idct_loeffler_1d(const int32_t *in, int32_t *out)
    { idct_1d_stride(in, out, 1); }

void dct_loeffler_2d(const int32_t *in, int32_t *out)
{
    int32_t temp[64];
    for (int y = 0; y < 8; y++) dct_1d_stride(in+y*8, 1, temp+y*8);
    for (int x = 0; x < 8; x++) dct_1d_stride(temp+x, 8, out+x*8);
    for (int y = 0; y < 8; y++)
        for (int x = y+1; x < 8; x++) {
            int32_t t = out[y*8+x]; out[y*8+x] = out[x*8+y]; out[x*8+y] = t;
        }
}

void idct_loeffler_2d(const int32_t *in, int32_t *out)
{
    int32_t temp[64], col[8];
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) col[y] = in[y*8+x];
        idct_1d_stride(col, temp+x, 8);
    }
    for (int y = 0; y < 8; y++) idct_1d_stride(temp+y*8, out+y*8, 1);
}
