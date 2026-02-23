#include "metrics.h"
#include <math.h>

static const int zigzag_order[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

double calc_psnr(const uint8_t *orig, const uint8_t *recon,
                 int width, int height)
{
    int total = width * height * 3;
    double mse = 0.0;

    for (int i = 0; i < total; i++) {
        double d = (double)orig[i] - (double)recon[i];
        mse += d * d;
    }
    mse /= total;

    if (mse < 1e-10) return 100.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

double calc_bitrate(const jpeg_compressed_t *comp)
{
    if (!comp) return 0.0;

    double total_bits  = 0.0;
    int    total_blocks = 0;

    const int32_t *channels[] = {
        comp->y_quantized, comp->cb_quantized, comp->cr_quantized
    };
    const int num_blocks[] = {
        comp->num_blocks_y, comp->num_blocks_chroma, comp->num_blocks_chroma
    };

    for (int ch = 0; ch < 3; ch++) {
        for (int b = 0; b < num_blocks[ch]; b++) {
            const int32_t *block = channels[ch] + b * 64;
            int last_nz = -1;
            for (int i = 63; i >= 0; i--) {
                if (block[zigzag_order[i]] != 0) { last_nz = i; break; }
            }
            if (last_nz >= 0)
                total_bits += (last_nz + 1) * 8.0;
            total_blocks++;
        }
    }
    int total_pixels = total_blocks * 64;
    return total_pixels > 0 ? total_bits / total_pixels : 0.0;
}
