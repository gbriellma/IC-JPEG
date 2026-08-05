/* colorspace.c - Batch RGB/YCbCr conversion (ITU-R BT.601) */

#include "../include/internal.h"

/* ── Constantes de ponto fixo (fator 256, shift 8) ──────────────────────────
 * BT.601 com arredondamento: coef × 256, >> 8 no final.
 * Elimina divisão por 1000 por pixel - ~5× mais rápido em Xtensa LX6/LX7.
 *   Y  =  0.299 R + 0.587 G + 0.114 B      →  77, 150, 29
 *   Cb = -0.169 R - 0.331 G + 0.500 B      → -43, -85, 128
 *   Cr =  0.500 R - 0.419 G - 0.081 B      → 128,-107, -21
 * Inversa:
 *   R = Y + 1.402 Cr                       → 359
 *   G = Y - 0.344 Cb - 0.714 Cr            → -88, -183
 *   B = Y + 1.772 Cb                       → 454
 * ──────────────────────────────────────────────────────────────────────────*/

/* Batch RGB to YCbCr - bit-shift, no division */
void rgb_to_ycbcr_batch(const uint8_t *rgb, int32_t *y, int32_t *cb, int32_t *cr, int n) {
    while (n--) {
        int32_t r = *rgb++, g = *rgb++, b = *rgb++;
        *y++  = (( 77 * r + 150 * g +  29 * b + 128) >> 8) - 128;
        *cb++ = ((-43 * r -  85 * g + 128 * b + 128) >> 8);
        *cr++ = ((128 * r - 107 * g -  21 * b + 128) >> 8);
    }
}

/* Batch YCbCr to RGB - bit-shift, no division */
void ycbcr_to_rgb_batch(const int32_t *y, const int32_t *cb, const int32_t *cr,
                        uint8_t *rgb, int n) {
    while (n--) {
        int32_t yv = *y++ + 128, cbv = *cb++, crv = *cr++;
        int32_t r  = yv + ((359 * crv + 128) >> 8);
        int32_t g  = yv - (( 88 * cbv + 183 * crv + 128) >> 8);
        int32_t bv = yv + ((454 * cbv + 128) >> 8);
        *rgb++ = (r  < 0) ? 0 : ((r  > 255) ? 255 : r);
        *rgb++ = (g  < 0) ? 0 : ((g  > 255) ? 255 : g);
        *rgb++ = (bv < 0) ? 0 : ((bv > 255) ? 255 : bv);
    }
}

/* Single-pixel variants (same bit-shift logic) */
void rgb_to_ycbcr(uint8_t r, uint8_t g, uint8_t b,
                  int32_t *y, int32_t *cb, int32_t *cr) {
    *y  = (( 77*(int32_t)r + 150*(int32_t)g +  29*(int32_t)b + 128) >> 8) - 128;
    *cb = ((-43*(int32_t)r -  85*(int32_t)g + 128*(int32_t)b + 128) >> 8);
    *cr = ((128*(int32_t)r - 107*(int32_t)g -  21*(int32_t)b + 128) >> 8);
}

void ycbcr_to_rgb(int32_t y, int32_t cb, int32_t cr,
                  uint8_t *r, uint8_t *g, uint8_t *b) {
    y += 128;
    int32_t rv = y + ((359 * cr + 128) >> 8);
    int32_t gv = y - (( 88 * cb + 183 * cr + 128) >> 8);
    int32_t bv = y + ((454 * cb + 128) >> 8);
    *r = (rv < 0) ? 0 : ((rv > 255) ? 255 : rv);
    *g = (gv < 0) ? 0 : ((gv > 255) ? 255 : gv);
    *b = (bv < 0) ? 0 : ((bv > 255) ? 255 : bv);
}