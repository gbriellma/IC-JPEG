/* quantization.c - JPEG quantization tables and functions */

#include "../include/internal.h"

/* Reciprocal shift: result = (val * recip) >> RECIP_SHIFT */
#define RECIP_SHIFT 16

/* Standard JPEG luminance table (Q=50) */
const int32_t Q50_LUMA[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

const int32_t Q50_CHROMA[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Zigzag scan order */
const int32_t ZIGZAG_ORDER[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

/* Compute reciprocal table for fast division: recip[i] = (1 << RECIP_SHIFT) / qt[i] */
void compute_reciprocal_table(const int32_t quant_table[64], uint32_t recip_table[64]) {
    for (int i = 0; i < 64; i++) {
        recip_table[i] = ((1U << RECIP_SHIFT) + quant_table[i] / 2) / quant_table[i];
    }
}

/* Fast quantization using reciprocal multiplication - no division in inner loop */
void quantize_fast(const int32_t dct_block[64], const int32_t quant_table[64],
                   const uint32_t recip_table[64], int32_t output[64]) {
    for (int i = 0; i < 64; i++) {
        int32_t dct = dct_block[i];
        int32_t qt = quant_table[i];
        uint32_t recip = recip_table[i];
        
        /* Add half for rounding, then multiply by reciprocal and shift */
        if (dct >= 0) {
            output[i] = (int32_t)(((int64_t)(dct + (qt >> 1)) * recip) >> RECIP_SHIFT);
        } else {
            output[i] = -(int32_t)(((int64_t)((-dct) + (qt >> 1)) * recip) >> RECIP_SHIFT);
        }
    }
}

/* Original quantization (fallback, uses division) */
void quantize(const int32_t dct_block[64], const int32_t quant_table[64],
              int32_t output[64]) {
    for (int i = 0; i < 64; i++) {
        int32_t dct = dct_block[i];
        int32_t qt = quant_table[i];
        if (dct >= 0) {
            output[i] = (dct + (qt >> 1)) / qt;
        } else {
            output[i] = (dct - (qt >> 1)) / qt;
        }
    }
}

/* Dequantization - simple multiply, already optimal */
void dequantize(const int32_t quant_block[64], const int32_t quant_table[64],
                int32_t output[64]) {
    for (int i = 0; i < 64; i++) {
        output[i] = quant_block[i] * quant_table[i];
    }
}

/* Scale quantization table with integer factor (no float) */
void scale_quant_table(const int32_t base_table[64], float k,
                       int32_t output[64]) {
    /* Convert float to fixed-point scale (10 bits precision) */
    int32_t k_fixed = (int32_t)(k * 1024);
    
    for (int i = 0; i < 64; i++) {
        int32_t scaled = (base_table[i] * k_fixed) >> 10;
        output[i] = (scaled < 1) ? 1 : scaled;
    }
}