/* utils.c - Block extraction, reconstruction, and subsampling utilities
 *
 * v2: adds direct image-to-block extraction (no intermediate channel buffers),
 *     chroma subsampling (4:2:0, 4:2:2), and block-to-image write-back.
 *     Legacy extract_blocks/reconstruct_channel kept for compatibility.
 */

#include "../include/internal.h"
#include "../include/jpeg_codec.h"
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  v2: Direct block extraction from image
 * ================================================================ */

/* BT.601 fixed-point coefficients (shift 8) - same as colorspace.c */
#define YR  77
#define YG 150
#define YB  29
#define CBR (-43)
#define CBG (-85)
#define CBB 128
#define CRR 128
#define CRG (-107)
#define CRB (-21)

void extract_luma_block_rgb(const uint8_t *rgb, int32_t width, int32_t height,
                            int32_t bx, int32_t by, int32_t block[64])
{
    int32_t x0 = bx * 8, y0 = by * 8;
    for (int r = 0; r < 8; r++) {
        int32_t y = y0 + r;
        for (int c = 0; c < 8; c++) {
            int32_t x = x0 + c;
            if (x < width && y < height) {
                const uint8_t *px = &rgb[(y * width + x) * 3];
                block[r * 8 + c] = ((YR * (int32_t)px[0] + YG * (int32_t)px[1] +
                                     YB * (int32_t)px[2] + 128) >> 8) - 128;
            } else {
                block[r * 8 + c] = 0;
            }
        }
    }
}

void extract_luma_block_gray(const uint8_t *gray, int32_t width, int32_t height,
                             int32_t bx, int32_t by, int32_t block[64])
{
    int32_t x0 = bx * 8, y0 = by * 8;
    for (int r = 0; r < 8; r++) {
        int32_t y = y0 + r;
        for (int c = 0; c < 8; c++) {
            int32_t x = x0 + c;
            if (x < width && y < height)
                block[r * 8 + c] = (int32_t)gray[y * width + x] - 128;
            else
                block[r * 8 + c] = 0;
        }
    }
}

void extract_chroma_blocks_444(const uint8_t *rgb, int32_t width, int32_t height,
                               int32_t bx, int32_t by,
                               int32_t cb_block[64], int32_t cr_block[64])
{
    int32_t x0 = bx * 8, y0 = by * 8;
    for (int r = 0; r < 8; r++) {
        int32_t y = y0 + r;
        for (int c = 0; c < 8; c++) {
            int32_t x = x0 + c;
            if (x < width && y < height) {
                const uint8_t *px = &rgb[(y * width + x) * 3];
                int32_t rv = px[0], gv = px[1], bv = px[2];
                cb_block[r * 8 + c] = ((CBR * rv + CBG * gv + CBB * bv + 128) >> 8);
                cr_block[r * 8 + c] = ((CRR * rv + CRG * gv + CRB * bv + 128) >> 8);
            } else {
                cb_block[r * 8 + c] = 0;
                cr_block[r * 8 + c] = 0;
            }
        }
    }
}

/* 4:2:0: average a 16x16 area into one 8x8 chroma block.
 * Each output pixel = average of 2x2 input pixels. */
void extract_chroma_blocks_420(const uint8_t *rgb, int32_t width, int32_t height,
                               int32_t mcx, int32_t mcy,
                               int32_t cb_block[64], int32_t cr_block[64])
{
    int32_t x0 = mcx * 16, y0 = mcy * 16;

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int32_t cb_sum = 0, cr_sum = 0;
            int count = 0;

            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int32_t px = x0 + c * 2 + dx;
                    int32_t py = y0 + r * 2 + dy;
                    if (px < width && py < height) {
                        const uint8_t *p = &rgb[(py * width + px) * 3];
                        int32_t rv = p[0], gv = p[1], bv = p[2];
                        cb_sum += ((CBR * rv + CBG * gv + CBB * bv + 128) >> 8);
                        cr_sum += ((CRR * rv + CRG * gv + CRB * bv + 128) >> 8);
                        count++;
                    }
                }
            }

            if (count > 0) {
                cb_block[r * 8 + c] = codec_div_round_symm(cb_sum, count);
                cr_block[r * 8 + c] = codec_div_round_symm(cr_sum, count);
            } else {
                cb_block[r * 8 + c] = 0;
                cr_block[r * 8 + c] = 0;
            }
        }
    }
}

/* 4:2:2: average a 16x8 area horizontally into one 8x8 chroma block.
 * Each output pixel = average of 2x1 input pixels. */
void extract_chroma_blocks_422(const uint8_t *rgb, int32_t width, int32_t height,
                               int32_t mcx, int32_t mcy,
                               int32_t cb_block[64], int32_t cr_block[64])
{
    int32_t x0 = mcx * 16, y0 = mcy * 8;

    for (int r = 0; r < 8; r++) {
        int32_t py = y0 + r;
        for (int c = 0; c < 8; c++) {
            int32_t cb_sum = 0, cr_sum = 0;
            int count = 0;

            for (int dx = 0; dx < 2; dx++) {
                int32_t px = x0 + c * 2 + dx;
                if (px < width && py < height) {
                    const uint8_t *p = &rgb[(py * width + px) * 3];
                    int32_t rv = p[0], gv = p[1], bv = p[2];
                    cb_sum += ((CBR * rv + CBG * gv + CBB * bv + 128) >> 8);
                    cr_sum += ((CRR * rv + CRG * gv + CRB * bv + 128) >> 8);
                    count++;
                }
            }

            if (count > 0) {
                cb_block[r * 8 + c] = codec_div_round_symm(cb_sum, count);
                cr_block[r * 8 + c] = codec_div_round_symm(cr_sum, count);
            } else {
                cb_block[r * 8 + c] = 0;
                cr_block[r * 8 + c] = 0;
            }
        }
    }
}

/* ================================================================
 *  v2: Block write-back to image
 * ================================================================ */

/* BT.601 inverse coefficients (shift 8) - same as colorspace.c */
#define INV_CR_R 359
#define INV_CB_G  88
#define INV_CR_G 183
#define INV_CB_B 454

void write_block_to_rgb(uint8_t *rgb, int32_t width, int32_t height,
                        int32_t bx, int32_t by,
                        const int32_t y_block[64],
                        const int32_t cb_block[64],
                        const int32_t cr_block[64])
{
    int32_t x0 = bx * 8, y0 = by * 8;
    for (int r = 0; r < 8; r++) {
        int32_t y = y0 + r;
        if (y >= height) break;
        for (int c = 0; c < 8; c++) {
            int32_t x = x0 + c;
            if (x >= width) break;

            int32_t yv  = y_block[r * 8 + c] + 128;
            int32_t cbv = cb_block[r * 8 + c];
            int32_t crv = cr_block[r * 8 + c];

            int32_t rv = yv + ((INV_CR_R * crv + 128) >> 8);
            int32_t gv = yv - ((INV_CB_G * cbv + INV_CR_G * crv + 128) >> 8);
            int32_t bv = yv + ((INV_CB_B * cbv + 128) >> 8);

            uint8_t *out = &rgb[(y * width + x) * 3];
            out[0] = clamp_uint8(rv);
            out[1] = clamp_uint8(gv);
            out[2] = clamp_uint8(bv);
        }
    }
}

void write_block_to_gray(uint8_t *gray, int32_t width, int32_t height,
                         int32_t bx, int32_t by,
                         const int32_t y_block[64])
{
    int32_t x0 = bx * 8, y0 = by * 8;
    for (int r = 0; r < 8; r++) {
        int32_t y = y0 + r;
        if (y >= height) break;
        for (int c = 0; c < 8; c++) {
            int32_t x = x0 + c;
            if (x >= width) break;
            gray[y * width + x] = clamp_uint8(y_block[r * 8 + c] + 128);
        }
    }
}

/* ================================================================
 *  Chroma upsampling (decode side)
 * ================================================================ */

/* 4:2:0: nearest-neighbor upsample 8x8 -> four 8x8 blocks covering 16x16 */
void upsample_chroma_420(const int32_t chroma_8x8[64],
                         int32_t out_tl[64], int32_t out_tr[64],
                         int32_t out_bl[64], int32_t out_br[64])
{
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            /* Source sample at (r/2, c/2) in the downsampled block
             * maps to (r, c) in each quadrant via nearest neighbor.
             * Quadrant mapping:
             *   TL: rows 0-3 of source -> rows 0-7 of TL
             *   TR: cols 4-7 of source -> cols 0-7 of TR
             *   BL: rows 4-7 of source -> rows 0-7 of BL
             *   BR: rows 4-7, cols 4-7 -> BR
             */
            int sr_top = r / 2;         /* source row for top quadrants */
            int sr_bot = 4 + r / 2;     /* source row for bottom quadrants */
            int sc_left = c / 2;        /* source col for left quadrants */
            int sc_right = 4 + c / 2;   /* source col for right quadrants */

            out_tl[r * 8 + c] = chroma_8x8[sr_top * 8 + sc_left];
            out_tr[r * 8 + c] = chroma_8x8[sr_top * 8 + sc_right];
            out_bl[r * 8 + c] = chroma_8x8[sr_bot * 8 + sc_left];
            out_br[r * 8 + c] = chroma_8x8[sr_bot * 8 + sc_right];
        }
    }
}

/* 4:2:2: nearest-neighbor upsample 8x8 -> two 8x8 blocks (left, right)
 * covering 16x8 pixels (horizontal-only downsample). */
void upsample_chroma_422(const int32_t chroma_8x8[64],
                         int32_t out_left[64], int32_t out_right[64])
{
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int sc_left  = c / 2;
            int sc_right = 4 + c / 2;
            out_left[r * 8 + c]  = chroma_8x8[r * 8 + sc_left];
            out_right[r * 8 + c] = chroma_8x8[r * 8 + sc_right];
        }
    }
}

/* ================================================================
 *  Legacy: full-channel block extraction/reconstruction
 *  Kept for compatibility with experiment scripts.
 * ================================================================ */

static void extract_center_blocks(const int32_t *channel, int32_t width,
                                   int32_t bx, int32_t bx_full, int32_t by_full,
                                   int32_t *blocks) {
    for (int32_t j = 0; j < by_full; j++) {
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = channel + (j * 8 * width) + (i * 8);
            int32_t *dst = blocks + (j * bx + i) * 64;
            for (int y = 0; y < 8; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += 8; src += width;
            }
        }
    }
}

static void extract_edge_blocks(const int32_t *channel, int32_t width, int32_t height,
                                 int32_t bx, int32_t by, int32_t bx_full, int32_t by_full,
                                 int32_t *blocks) {
    if (bx > bx_full) {
        int32_t i = bx_full, px = width - i * 8;
        for (int32_t j = 0; j < by_full; j++) {
            int32_t *dst = blocks + (j * bx + i) * 64;
            const int32_t *src = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < 8; y++) {
                for (int k = 0; k < px; k++) dst[k] = src[k];
                for (int k = px; k < 8; k++) dst[k] = 0;
                dst += 8; src += width;
            }
        }
    }
    if (by > by_full) {
        int32_t j = by_full, py = height - j * 8;
        for (int32_t i = 0; i < bx_full; i++) {
            int32_t *dst = blocks + (j * bx + i) * 64;
            const int32_t *src = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < py; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += 8; src += width;
            }
            for (int y = py; y < 8; y++) {
                memset(dst, 0, 8 * sizeof(int32_t));
                dst += 8;
            }
        }
    }
    if (bx > bx_full && by > by_full) {
        int32_t i = bx_full, j = by_full;
        int32_t px = width - i * 8, py = height - j * 8;
        int32_t *dst = blocks + (j * bx + i) * 64;
        const int32_t *src = channel + (j * 8 * width) + (i * 8);
        for (int y = 0; y < py; y++) {
            for (int k = 0; k < px; k++) dst[k] = src[k];
            for (int k = px; k < 8; k++) dst[k] = 0;
            dst += 8; src += width;
        }
        for (int y = py; y < 8; y++) {
            memset(dst, 0, 8 * sizeof(int32_t));
            dst += 8;
        }
    }
}

int extract_blocks(const int32_t *channel, int32_t width, int32_t height,
                   int32_t **blocks, int32_t *num_blocks) {
    size_t total_coeffs, total_bytes;

    if (!channel || !blocks || !num_blocks) return -1;
    if (width <= 0 || height <= 0) return -1;
    int32_t bx = codec_ceil_div_pos(width, 8), by = codec_ceil_div_pos(height, 8);
    int32_t bx_full = width / 8, by_full = height / 8;
    if (!codec_checked_mul_i32(bx, by, num_blocks)) return -1;
    if (!codec_checked_mul_size((size_t)(*num_blocks), 64u, &total_coeffs)) return -1;
    if (!codec_checked_mul_size(total_coeffs, sizeof(int32_t), &total_bytes)) return -1;
    *blocks = (int32_t*)CODEC_CALLOC(total_bytes, 1u);
    if (!*blocks) return -1;
    extract_center_blocks(channel, width, bx, bx_full, by_full, *blocks);
    if (bx > bx_full || by > by_full)
        extract_edge_blocks(channel, width, height, bx, by, bx_full, by_full, *blocks);
    return 0;
}

static void reconstruct_center_blocks(const int32_t *blocks, int32_t width,
                                       int32_t bx, int32_t bx_full, int32_t by_full,
                                       int32_t *channel) {
    for (int32_t j = 0; j < by_full; j++) {
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = blocks + (j * bx + i) * 64;
            int32_t *dst = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < 8; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += width; src += 8;
            }
        }
    }
}

static void reconstruct_edge_blocks(const int32_t *blocks, int32_t width, int32_t height,
                                     int32_t bx, int32_t by, int32_t bx_full, int32_t by_full,
                                     int32_t *channel) {
    if (bx > bx_full) {
        int32_t i = bx_full, px = width - i * 8;
        for (int32_t j = 0; j < by_full; j++) {
            const int32_t *src = blocks + (j * bx + i) * 64;
            int32_t *dst = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < 8; y++) {
                for (int k = 0; k < px; k++) dst[k] = src[k];
                dst += width; src += 8;
            }
        }
    }
    if (by > by_full) {
        int32_t j = by_full, py = height - j * 8;
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = blocks + (j * bx + i) * 64;
            int32_t *dst = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < py; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += width; src += 8;
            }
        }
    }
    if (bx > bx_full && by > by_full) {
        int32_t i = bx_full, j = by_full;
        int32_t px = width - i * 8, py = height - j * 8;
        const int32_t *src = blocks + (j * bx + i) * 64;
        int32_t *dst = channel + (j * 8 * width) + (i * 8);
        for (int y = 0; y < py; y++) {
            for (int k = 0; k < px; k++) dst[k] = src[k];
            dst += width; src += 8;
        }
    }
}

int reconstruct_channel(const int32_t *blocks, int32_t num_blocks __attribute__((unused)),
                        int32_t width, int32_t height, int32_t *channel) {
    if (!blocks || !channel) return -1;
    int32_t bx = (width + 7) / 8, by = (height + 7) / 8;
    int32_t bx_full = width / 8, by_full = height / 8;
    reconstruct_center_blocks(blocks, width, bx, bx_full, by_full, channel);
    if (bx > bx_full || by > by_full)
        reconstruct_edge_blocks(blocks, width, height, bx, by, bx_full, by_full, channel);
    return 0;
}

/* RGB565 to RGB888 (for embedded cameras) */
void convert_rgb565_to_rgb888(const uint16_t *in, uint8_t *out, int w, int h) {
    const uint8_t *src = (const uint8_t *)in;
    int n = w * h;
    while (n--) {
        uint8_t hb = *src++;
        uint8_t lb = *src++;
        uint8_t r5 = (uint8_t)((hb >> 3) & 0x1F);
        uint8_t g6 = (uint8_t)(((hb & 0x07u) << 3) | (lb >> 5));
        uint8_t b5 = (uint8_t)(lb & 0x1F);
        *out++ = (uint8_t)((r5 << 3) | (r5 >> 2));
        *out++ = (uint8_t)((g6 << 2) | (g6 >> 4));
        *out++ = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
}
