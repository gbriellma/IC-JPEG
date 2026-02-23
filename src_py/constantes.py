"""
constantes.py — Constantes idênticas à libimage em C (internal.h / quantization.c).

Todas as constantes de ponto fixo, tabelas de quantização e matrizes
de transformação são réplicas exatas do código C.
"""

import numpy as np

TYPE = np.int32

# ============ Fixed-point arithmetic (internal.h) — SCALE = 2^20 ============

SCALE_CONST = 1048576  # 2^20

C1 = 1028428      # cos(π/16)   * 2^20
S1 = 204567       # sin(π/16)   * 2^20
C3 = 871859       # cos(3π/16)  * 2^20
S3 = 582558       # sin(3π/16)  * 2^20
C6 = 401273       # cos(6π/16)  * 2^20
S6 = 968758       # sin(6π/16)  * 2^20
SQRT_2 = 1482910  # sqrt(2)     * 2^20
SQRT_8 = 2965821  # sqrt(8)     * 2^20

# ============ Matrix DCT constants (dct_matrix.c) — same SCALE = 2^20 ============
MATRIX_SCALE = 1048576
MATRIX_NORM_0 = 370728   # 1/sqrt(8) * 2^20
MATRIX_NORM_K = 524288   # sqrt(2/8) * 2^20 = 2^19 (exact)
MATRIX_SCALE_SQ = MATRIX_SCALE * MATRIX_SCALE

# Cosine matrix: cos(π*k*(2n+1)/16) * 2^20
MATRIX_COS = np.array([
    [ 1048576,  1048576,  1048576,  1048576,  1048576,  1048576,  1048576,  1048576],
    [ 1028428,   871859,   582558,   204567,  -204567,  -582558,  -871859, -1028428],
    [  968758,   401273,  -401273,  -968758,  -968758,  -401273,   401273,   968758],
    [  871859,  -204567, -1028428,  -582558,   582558,  1028428,   204567,  -871859],
    [  741455,  -741455,  -741455,   741455,   741455,  -741455,  -741455,   741455],
    [  582558, -1028428,   204567,   871859,  -871859,  -204567,  1028428,  -582558],
    [  401273,  -968758,   968758,  -401273,  -401273,   968758,  -968758,   401273],
    [  204567,  -582558,   871859, -1028428,  1028428,  -871859,   582558,  -204567],
], dtype=np.int64)

MATRIX_NORM = np.array([MATRIX_NORM_0] + [MATRIX_NORM_K]*7, dtype=np.int64)

# ============ Standard JPEG quantization tables Q=50 (quantization.c) ============

Q50_LUMA = np.array([
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
], dtype=TYPE)  # flat (64,) — matches C's flat int32_t[64]

Q50_CHROMA = np.array([
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
], dtype=TYPE)  # flat (64,)

# ============ Zigzag scan order ============

# ZIGZAG_ORDER: mapeamento flat_index → zigzag_position (quantization.c)
# NÃO usar para varredura zigzag! Usar ZIGZAG_SCAN abaixo.
ZIGZAG_ORDER = np.array([
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
], dtype=TYPE)

# ZIGZAG_SCAN: varredura zigzag padrão JPEG (metrics.c)
# zigzag_scan[scan_position] = flat_index
# Usar para bitrate: block[ZIGZAG_SCAN[i]] dá o i-ésimo coef em ordem zigzag.
ZIGZAG_SCAN = np.array([
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
], dtype=TYPE)
