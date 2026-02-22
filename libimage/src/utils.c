/* utils.c - Block extraction and reconstruction utilities */

#include "../include/internal.h"
#include <stdlib.h>

/* Extract center blocks (no boundary checks) - maximum performance */
static void extract_center_blocks(const int32_t *channel, int32_t width,
                                   int32_t bx, int32_t bx_full, int32_t by_full,
                                   int32_t *blocks) {
    for (int32_t j = 0; j < by_full; j++) {
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = channel + (j * 8 * width) + (i * 8);
            int32_t *dst = blocks + (j * bx + i) * 64;
            
            /* Unrolled 8x8 copy - no bounds checks */
            for (int y = 0; y < 8; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += 8;
                src += width;
            }
        }
    }
}

/* Extract edge blocks (with boundary checks) */
static void extract_edge_blocks(const int32_t *channel, int32_t width, int32_t height,
                                 int32_t bx, int32_t by, int32_t bx_full, int32_t by_full,
                                 int32_t *blocks) {
    /* Right column of blocks (partial width) */
    if (bx > bx_full) {
        int32_t i = bx_full;
        int32_t px = width - i * 8;  /* remaining pixels */
        
        for (int32_t j = 0; j < by_full; j++) {
            int32_t *dst = blocks + (j * bx + i) * 64;
            const int32_t *src = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < 8; y++) {
                for (int k = 0; k < px; k++) dst[k] = src[k];
                for (int k = px; k < 8; k++) dst[k] = 0;
                dst += 8;
                src += width;
            }
        }
    }
    
    /* Bottom row of blocks (partial height) */
    if (by > by_full) {
        int32_t j = by_full;
        int32_t py = height - j * 8;  /* remaining rows */
        int32_t *dst = blocks + (j * bx + 0) * 64;
        
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < py; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += 8;
                src += width;
            }
            for (int y = py; y < 8; y++) {
                for (int k = 0; k < 8; k++) dst[k] = 0;
                dst += 8;
            }
        }
    }
    
    /* Bottom-right corner block (partial both dimensions) */
    if (bx > bx_full && by > by_full) {
        int32_t i = bx_full, j = by_full;
        int32_t px = width - i * 8, py = height - j * 8;
        int32_t *dst = blocks + (j * bx + i) * 64;
        const int32_t *src = channel + (j * 8 * width) + (i * 8);
        
        for (int y = 0; y < py; y++) {
            for (int k = 0; k < px; k++) dst[k] = src[k];
            for (int k = px; k < 8; k++) dst[k] = 0;
            dst += 8;
            src += width;
        }
        for (int y = py; y < 8; y++) {
            for (int k = 0; k < 8; k++) dst[k] = 0;
            dst += 8;
        }
    }
}

/* Extract 8x8 blocks from image channel - optimized with separate paths */
int extract_blocks(const int32_t *channel, int32_t width, int32_t height,
                   int32_t **blocks, int32_t *num_blocks) {
    if (!channel || !blocks || !num_blocks) return -1;
    
    int32_t bx = (width + 7) / 8, by = (height + 7) / 8;
    int32_t bx_full = width / 8, by_full = height / 8;  /* full blocks (no boundary) */
    
    *num_blocks = bx * by;
    *blocks = (int32_t*)CODEC_CALLOC(*num_blocks * 64, sizeof(int32_t));
    if (!*blocks) return -1;
    
    /* Fast path for center blocks - no boundary checks */
    extract_center_blocks(channel, width, bx, bx_full, by_full, *blocks);
    
    /* Slow path for edge blocks - with boundary checks */
    if (bx > bx_full || by > by_full) {
        extract_edge_blocks(channel, width, height, bx, by, bx_full, by_full, *blocks);
    }
    
    return 0;
}

/* Reconstruct center blocks (no boundary checks) */
static void reconstruct_center_blocks(const int32_t *blocks, int32_t width,
                                       int32_t bx, int32_t bx_full, int32_t by_full,
                                       int32_t *channel) {
    for (int32_t j = 0; j < by_full; j++) {
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = blocks + (j * bx + i) * 64;
            int32_t *dst = channel + (j * 8 * width) + (i * 8);
            
            /* Unrolled 8x8 copy - no bounds checks */
            for (int y = 0; y < 8; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += width;
                src += 8;
            }
        }
    }
}

/* Reconstruct edge blocks (with boundary checks) */
static void reconstruct_edge_blocks(const int32_t *blocks, int32_t width, int32_t height,
                                     int32_t bx, int32_t by, int32_t bx_full, int32_t by_full,
                                     int32_t *channel) {
    /* Right column */
    if (bx > bx_full) {
        int32_t i = bx_full;
        int32_t px = width - i * 8;
        
        for (int32_t j = 0; j < by_full; j++) {
            const int32_t *src = blocks + (j * bx + i) * 64;
            int32_t *dst = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < 8; y++) {
                for (int k = 0; k < px; k++) dst[k] = src[k];
                dst += width;
                src += 8;
            }
        }
    }
    
    /* Bottom row */
    if (by > by_full) {
        int32_t j = by_full;
        int32_t py = height - j * 8;
        
        for (int32_t i = 0; i < bx_full; i++) {
            const int32_t *src = blocks + (j * bx + i) * 64;
            int32_t *dst = channel + (j * 8 * width) + (i * 8);
            for (int y = 0; y < py; y++) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst[4] = src[4]; dst[5] = src[5]; dst[6] = src[6]; dst[7] = src[7];
                dst += width;
                src += 8;
            }
        }
    }
    
    /* Bottom-right corner */
    if (bx > bx_full && by > by_full) {
        int32_t i = bx_full, j = by_full;
        int32_t px = width - i * 8, py = height - j * 8;
        const int32_t *src = blocks + (j * bx + i) * 64;
        int32_t *dst = channel + (j * 8 * width) + (i * 8);
        
        for (int y = 0; y < py; y++) {
            for (int k = 0; k < px; k++) dst[k] = src[k];
            dst += width;
            src += 8;
        }
    }
}

/* Reconstruct image channel from 8x8 blocks - optimized */
int reconstruct_channel(const int32_t *blocks, int32_t num_blocks __attribute__((unused)),
                        int32_t width, int32_t height, int32_t *channel) {
    if (!blocks || !channel) return -1;
    
    int32_t bx = (width + 7) / 8, by = (height + 7) / 8;
    int32_t bx_full = width / 8, by_full = height / 8;
    
    /* Fast path for center blocks */
    reconstruct_center_blocks(blocks, width, bx, bx_full, by_full, channel);
    
    /* Slow path for edge blocks */
    if (bx > bx_full || by > by_full) {
        reconstruct_edge_blocks(blocks, width, height, bx, by, bx_full, by_full, channel);
    }
    
    return 0;
}

/* Convert RGB565 to RGB888 (for embedded cameras) - byte-level access
 * Camera DMA produces big-endian RGB565: byte[0]=RRRRRGGG, byte[1]=GGGBBBBB.
 * Reading individual bytes avoids endianness issues. */
void convert_rgb565_to_rgb888(const uint16_t *in, uint8_t *out, int w, int h) {
    const uint8_t *src = (const uint8_t *)in;
    int n = w * h;

    while (n--) {
        uint8_t hb = *src++;  /* byte 0: RRRRRGGG */
        uint8_t lb = *src++;  /* byte 1: GGGBBBBB */
        *out++ = hb & 0xF8;                            /* R: top 5 bits */
        *out++ = (hb & 0x07) << 5 | (lb & 0xE0) >> 3;  /* G: 6 bits   */
        *out++ = (lb & 0x1F) << 3;                      /* B: low 5 bits */
    }
}
