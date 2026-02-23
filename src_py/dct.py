"""
dct.py — Implementações Python das DCTs idênticas ao código C da libimage.

Cada função replica *exatamente* a aritmética inteira do código C correspondente,
incluindo divisão truncada para zero (C-style) e div_round quando usado no C.

Arquivos C de referência:
    - dct_loeffler.c    (11 multiplicações, Loeffler 1989)
    - dct_matrix.c      (64 multiplicações, matriz de cossenos)
    - dct_approx.c      (0 multiplicações, Cintra-Bayer 2011)
    - dct_identity.c    (pass-through)
"""

import numpy as np
from constantes import (
    TYPE, C1, S1, C3, S3, C6, S6, SQRT_2, SCALE_CONST,
    MATRIX_COS, MATRIX_NORM, MATRIX_SCALE_SQ,
)


# =====================================================================
#  Helpers: C-style integer arithmetic
# =====================================================================

def _c_div(a, b):
    """C-style integer division: truncation toward zero (b > 0 assumed)."""
    if a >= 0:
        return a // b
    else:
        return -((-a) // b)


def _div_round(num, den):
    """Signed rounding division matching C's div_round (den > 0)."""
    if num >= 0:
        return (num + den // 2) // den
    else:
        return -((-num + den // 2) // den)


# =====================================================================
#  Loeffler DCT / IDCT  (dct_loeffler.c)
# =====================================================================

def dct_loeffler_1d(x):
    """Forward 1D DCT — Loeffler, matching C's dct_1d_stride exactly."""
    v = [int(xi) for xi in np.asarray(x, dtype=np.int64).flatten()]

    s07 = v[0] + v[7]; d07 = v[0] - v[7]
    s16 = v[1] + v[6]; d16 = v[1] - v[6]
    s25 = v[2] + v[5]; d25 = v[2] - v[5]
    s34 = v[3] + v[4]; d34 = v[3] - v[4]

    e0 = s07 + s34; e3 = s07 - s34
    e1 = s16 + s25; e2 = s16 - s25
    o0 = d07 + d34; o1 = d16 + d25; o2 = d16 - d25; o3 = d07 - d34

    out = [0] * 8
    out[0] = _div_round((e0 + e1) * SCALE_CONST, SQRT_2 * 2)
    out[4] = _div_round((e0 - e1) * SCALE_CONST, SQRT_2 * 2)
    out[2] = _div_round(C6 * e2 + S6 * e3,       SCALE_CONST * 2)
    out[6] = _div_round(-S6 * e2 + C6 * e3,      SCALE_CONST * 2)
    out[1] = _div_round(C3*o0 + C1*o1 + S1*o2 + S3*o3, SQRT_2 * 2)
    out[3] = _div_round(S1*o0 - C3*o1 + S3*o2 + C1*o3, SQRT_2 * 2)
    out[5] = _div_round(C1*o0 - S3*o1 - C3*o2 - S1*o3, SQRT_2 * 2)
    out[7] = _div_round(-S3*o0 + S1*o1 - C1*o2 + C3*o3, SQRT_2 * 2)

    return np.array(out, dtype=TYPE)


def idct_loeffler_1d(x):
    """Inverse 1D DCT — Loeffler, deferred-division strategy.

    Matches C's idct_1d_stride exactly:
    - Even path: zero intermediate divisions (all kept at scale SC)
    - Odd path: one div_round to normalize from SC² to 4·SC
    - Final: single div_round(... , 8·SC) per output
    """
    v = [int(xi) for xi in np.asarray(x, dtype=np.int64).flatten()]

    Z0 = v[0]*2; Z1 = v[1]*2; Z2 = v[2]*2; Z3 = v[3]*2
    Z4 = v[4]*2; Z5 = v[5]*2; Z6 = v[6]*2; Z7 = v[7]*2

    # Even part — zero intermediate divisions
    t0_s = Z0 * SQRT_2                        # t0 * SC
    t4_s = Z4 * SQRT_2                        # t4 * SC
    e0_2s = t0_s + t4_s                       # 2*e0 * SC
    e1_2s = t0_s - t4_s                       # 2*e1 * SC
    e2_2s = 2 * (C6*Z2 - S6*Z6)              # 2*e2 * SC
    e3_2s = 2 * (S6*Z2 + C6*Z6)              # 2*e3 * SC
    s07_4s = e0_2s + e3_2s                    # 4*s07 * SC
    s34_4s = e0_2s - e3_2s                    # 4*s34 * SC
    s16_4s = e1_2s + e2_2s                    # 4*s16 * SC
    s25_4s = e1_2s - e2_2s                    # 4*s25 * SC

    # Odd part — one rounding division to match even part scale
    n0 = C3*Z1 + S1*Z3 + C1*Z5 - S3*Z7       # scale SC
    n1 = C1*Z1 - C3*Z3 - S3*Z5 + S1*Z7
    n2 = S1*Z1 + S3*Z3 - C3*Z5 - C1*Z7
    n3 = S3*Z1 + C1*Z3 - S1*Z5 + C3*Z7

    d07_4s = _div_round(2 * SCALE_CONST * (n0 + n3), SQRT_2)
    d34_4s = _div_round(2 * SCALE_CONST * (n0 - n3), SQRT_2)
    d16_4s = _div_round(2 * SCALE_CONST * (n1 + n2), SQRT_2)
    d25_4s = _div_round(2 * SCALE_CONST * (n1 - n2), SQRT_2)

    # Final butterfly — single rounding division per output
    final_div = 8 * SCALE_CONST
    out = [0] * 8
    out[0] = _div_round(s07_4s + d07_4s, final_div)
    out[7] = _div_round(s07_4s - d07_4s, final_div)
    out[1] = _div_round(s16_4s + d16_4s, final_div)
    out[6] = _div_round(s16_4s - d16_4s, final_div)
    out[2] = _div_round(s25_4s + d25_4s, final_div)
    out[5] = _div_round(s25_4s - d25_4s, final_div)
    out[3] = _div_round(s34_4s + d34_4s, final_div)
    out[4] = _div_round(s34_4s - d34_4s, final_div)

    return np.array(out, dtype=TYPE)


# =====================================================================
#  Matrix DCT / IDCT  (dct_matrix.c)
# =====================================================================

def dct_matrix_1d(x):
    """Forward 1D DCT — matrix multiplication, matching C exactly."""
    v = np.asarray(x, dtype=np.int64).flatten()
    out = np.zeros(8, dtype=np.int64)

    for k in range(8):
        s = 0
        for n in range(8):
            s += int(v[n]) * int(MATRIX_COS[k, n])
        out[k] = _div_round(s * int(MATRIX_NORM[k]), MATRIX_SCALE_SQ)

    return out.astype(TYPE)


def idct_matrix_1d(X):
    """Inverse 1D DCT — matrix multiplication, matching C exactly."""
    v = np.asarray(X, dtype=np.int64).flatten()
    out = np.zeros(8, dtype=np.int64)

    for n in range(8):
        s = 0
        for k in range(8):
            s += int(v[k]) * int(MATRIX_NORM[k]) * int(MATRIX_COS[k, n])
        out[n] = _div_round(s, MATRIX_SCALE_SQ)

    return out.astype(TYPE)


# =====================================================================
#  Approximate DCT / IDCT — Cintra-Bayer 2011  (dct_approx.c)
# =====================================================================

def dct_approximate_1d(x):
    """Forward 1D approx DCT — only additions, matching C exactly."""
    v = [int(xi) for xi in np.asarray(x, dtype=np.int64).flatten()]
    x0, x1, x2, x3, x4, x5, x6, x7 = v

    out = [0] * 8
    out[0] = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7
    out[1] = x0 + x1 + x2 - x5 - x6 - x7
    out[2] = x0 - x3 - x4 + x7
    out[3] = x0 - x2 - x3 + x4 + x5 - x7
    out[4] = x0 - x1 - x2 + x3 + x4 - x5 - x6 + x7
    out[5] = x0 - x1 + x3 - x4 + x6 - x7
    out[6] = -x1 + x2 + x5 - x6
    out[7] = -x1 + x2 - x3 + x4 - x5 + x6

    return np.array(out, dtype=TYPE)


def idct_approximate_1d(Y):
    """Inverse 1D approx DCT — norm-based inverse, matching C exactly.

    C uses:
        norm^2 = {8, 6, 4, 6, 8, 6, 4, 6}
        scale  = 24/norm^2 = {3, 4, 6, 4, 3, 4, 6, 4}
        Pre-scale each coeff, then T^T multiply, then /24 with rounding.
    """
    v = [int(yi) for yi in np.asarray(Y, dtype=np.int64).flatten()]

    # Pre-scale by norm factor
    a0 = v[0] * 3   # norm^2 = 8
    a1 = v[1] * 4   # norm^2 = 6
    a2 = v[2] * 6   # norm^2 = 4
    a3 = v[3] * 4   # norm^2 = 6
    a4 = v[4] * 3   # norm^2 = 8
    a5 = v[5] * 4   # norm^2 = 6
    a6 = v[6] * 6   # norm^2 = 4
    a7 = v[7] * 4   # norm^2 = 6

    # T^T * scaled, divide by 24 with rounding (add 12 = 24/2)
    # C uses (... + 12) / 24 where / is C truncation toward zero
    out = [0] * 8
    out[0] = _c_div(a0 + a1 + a2 + a3 + a4 + a5 + 12, 24)
    out[1] = _c_div(a0 + a1 - a4 - a5 - a6 - a7 + 12, 24)
    out[2] = _c_div(a0 + a1 - a3 - a4 + a6 + a7 + 12, 24)
    out[3] = _c_div(a0 - a2 - a3 + a4 + a5 - a7 + 12, 24)
    out[4] = _c_div(a0 - a2 + a3 + a4 - a5 + a7 + 12, 24)
    out[5] = _c_div(a0 - a1 + a3 - a4 + a6 - a7 + 12, 24)
    out[6] = _c_div(a0 - a1 - a4 + a5 - a6 + a7 + 12, 24)
    out[7] = _c_div(a0 - a1 + a2 - a3 + a4 - a5 + 12, 24)

    return np.array(out, dtype=TYPE)


# =====================================================================
#  Identity DCT / IDCT  (dct_identity.c)
# =====================================================================

def dct_identity_1d(x):
    return np.asarray(x, dtype=TYPE).flatten().copy()


def idct_identity_1d(X):
    return np.asarray(X, dtype=TYPE).flatten().copy()


# =====================================================================
#  2D transforms  (matching C's row-column / column-row order)
# =====================================================================

def dct_2d(block_8x8, func_dct_1d):
    """Forward 2D DCT: rows first, then columns (matches C dct_*_2d)."""
    blk = np.asarray(block_8x8, dtype=TYPE).reshape(8, 8)
    # Row transform
    temp = np.array([func_dct_1d(blk[y, :]) for y in range(8)], dtype=TYPE)
    # Column transform
    result_cols = np.array([func_dct_1d(temp[:, x]) for x in range(8)], dtype=TYPE)
    # Transpose (matches C's explicit transpose step)
    return result_cols.T


def idct_2d(block_dct_8x8, func_idct_1d):
    """Inverse 2D DCT: columns first, then rows (matches C idct_*_2d)."""
    blk = np.asarray(block_dct_8x8, dtype=TYPE).reshape(8, 8)
    # Column transform first (matches C)
    temp_cols = np.array([func_idct_1d(blk[:, x]) for x in range(8)], dtype=TYPE)
    temp = temp_cols.T  # each result stored as row → transpose back
    # Row transform second
    result = np.array([func_idct_1d(temp[y, :]) for y in range(8)], dtype=TYPE)
    return result
