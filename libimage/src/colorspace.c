/* colorspace.c - Batch RGB/YCbCr conversion (ITU-R BT.601) */

#include "../include/internal.h"

/* Batch RGB to YCbCr - process entire buffer, no per-pixel function calls */
void rgb_to_ycbcr_batch(const uint8_t *rgb, int32_t *y, int32_t *cb, int32_t *cr, int n) {
    while (n--) {
        int32_t r = *rgb++, g = *rgb++, b = *rgb++;
        *y++  = ((299*r + 587*g + 114*b + 500) / 1000) - 128;
        *cb++ = ((-169*r - 331*g + 500*b + 500) / 1000);
        *cr++ = ((500*r - 419*g - 81*b + 500) / 1000);
    }
}

/* Batch YCbCr to RGB - process entire buffer, no per-pixel function calls */
void ycbcr_to_rgb_batch(const int32_t *y, const int32_t *cb, const int32_t *cr, 
                        uint8_t *rgb, int n) {
    while (n--) {
        int32_t yv = *y++ + 128, cbv = *cb++, crv = *cr++;
        int32_t r = yv + (1402 * crv + 500) / 1000;
        int32_t g = yv - (344 * cbv + 714 * crv + 500) / 1000;
        int32_t bv = yv + (1772 * cbv + 500) / 1000;
        /* Inline clamp */
        *rgb++ = (r < 0) ? 0 : ((r > 255) ? 255 : r);
        *rgb++ = (g < 0) ? 0 : ((g > 255) ? 255 : g);
        *rgb++ = (bv < 0) ? 0 : ((bv > 255) ? 255 : bv);
    }
}

/* Legacy single-pixel (kept for compatibility) */
void rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b,
                  int32_t *y, int32_t *cb, int32_t *cr) {
    *y  = ((299*(int32_t)r + 587*(int32_t)g + 114*(int32_t)b + 500) / 1000) - 128;
    *cb = ((-169*(int32_t)r - 331*(int32_t)g + 500*(int32_t)b + 500) / 1000);
    *cr = ((500*(int32_t)r - 419*(int32_t)g - 81*(int32_t)b + 500) / 1000);
}

void ycbcr_to_rgb(int32_t y, int32_t cb, int32_t cr,
                  uint8_t *r, uint8_t *g, uint8_t *b) {
    y += 128;
    int32_t rv = y + (1402 * cr + 500) / 1000;
    int32_t gv = y - (344 * cb + 714 * cr + 500) / 1000;
    int32_t bv = y + (1772 * cb + 500) / 1000;
    *r = (rv < 0) ? 0 : ((rv > 255) ? 255 : rv);
    *g = (gv < 0) ? 0 : ((gv > 255) ? 255 : gv);
    *b = (bv < 0) ? 0 : ((bv > 255) ? 255 : bv);
}