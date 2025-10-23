import numpy as np
from constantes import TYPE, C1, S1, C3, S3, C6, S6, SQRT_2, SCALE_CONST, T_CINTRA_BAYER, SQRT_8

def dct_loeffler_1d(input_vector):
    v = np.asarray(input_vector, dtype=TYPE).flatten()
    s07, d07 = v[0] + v[7], v[0] - v[7]
    s16, d16 = v[1] + v[6], v[1] - v[6]
    s25, d25 = v[2] + v[5], v[2] - v[5]
    s34, d34 = v[3] + v[4], v[3] - v[4]
    e0, e3 = s07 + s34, s07 - s34
    e1, e2 = s16 + s25, s16 - s25
    Z0 = ((e0 + e1) * SCALE_CONST) // SQRT_2
    Z4 = ((e0 - e1) * SCALE_CONST) // SQRT_2
    Z2 = (C6 * e2 + S6 * e3) // SCALE_CONST
    Z6 = (-S6 * e2 + C6 * e3) // SCALE_CONST
    o0, o1 = d07 + d34, d16 + d25
    o2, o3 = d16 - d25, d07 - d34
    Z1 = (C3 * o0 + C1 * o1 + S1 * o2 + S3 * o3) // SQRT_2
    Z3 = (S1 * o0 - C3 * o1 + S3 * o2 + C1 * o3) // SQRT_2
    Z5 = (C1 * o0 - S3 * o1 - C3 * o2 - S1 * o3) // SQRT_2
    Z7 = (-S3 * o0 + S1 * o1 - C1 * o2 + C3 * o3) // SQRT_2
    return np.array([Z0, Z1, Z2, Z3, Z4, Z5, Z6, Z7], dtype=TYPE) // 2

def idct_loeffler_1d(dct_coefficients):
    v = np.asarray(dct_coefficients, dtype=TYPE).flatten() * 2
    Z0, Z1, Z2, Z3, Z4, Z5, Z6, Z7 = v
    t0, t4 = (Z0 * SQRT_2) // SCALE_CONST, (Z4 * SQRT_2) // SCALE_CONST
    e0, e1 = (t0 + t4) // 2, (t0 - t4) // 2
    e2 = (C6 * Z2 - S6 * Z6) // SCALE_CONST
    e3 = (S6 * Z2 + C6 * Z6) // SCALE_CONST
    s07, s34 = (e0 + e3) // 2, (e0 - e3) // 2
    s16, s25 = (e1 + e2) // 2, (e1 - e2) // 2
    o0 = (C3 * Z1 + S1 * Z3 + C1 * Z5 - S3 * Z7) // SQRT_2
    o1 = (C1 * Z1 - C3 * Z3 - S3 * Z5 + S1 * Z7) // SQRT_2
    o2 = (S1 * Z1 + S3 * Z3 - C3 * Z5 - C1 * Z7) // SQRT_2
    o3 = (S3 * Z1 + C1 * Z3 - S1 * Z5 + C3 * Z7) // SQRT_2
    d07, d34 = (o0 + o3) // 2, (o0 - o3) // 2
    d16, d25 = (o1 + o2) // 2, (o1 - o2) // 2
    x0, x7 = (s07 + d07) // 2, (s07 - d07) // 2
    x1, x6 = (s16 + d16) // 2, (s16 - d16) // 2
    x2, x5 = (s25 + d25) // 2, (s25 - d25) // 2
    x3, x4 = (s34 + d34) // 2, (s34 - d34) // 2
    return np.array([x0, x1, x2, x3, x4, x5, x6, x7], dtype=TYPE)

def dct_2d(block_8x8, func_dct_1d):
    dct_rows = np.array([func_dct_1d(row) for row in block_8x8])
    dct_cols = np.array([func_dct_1d(col) for col in dct_rows.T])
    return dct_cols.T

def idct_2d(block_dct_8x8, func_idct_1d):
    idct_rows = np.array([func_idct_1d(row) for row in block_dct_8x8])
    idct_cols = np.array([func_idct_1d(col) for col in idct_rows.T])
    return idct_cols.T

# ----------------- MATRIX DCT -----------------
def dct_matrix_1d(x):
    # DCT-II: X[k] = C[k] * sum(x[n] * cos(pi*k*(2n+1)/(2N)))
    # C[0] = sqrt(1/N), C[k] = sqrt(2/N) for k>0
    import math
    v = np.asarray(x, dtype=np.int64).flatten()
    X = np.zeros(8, dtype=np.int64)
    
    for k in range(8):
        soma = 0
        for n in range(8):
            cos_val = int(round(math.cos(math.pi * k * (2*n + 1) / 16) * SCALE_CONST))
            soma += v[n] * cos_val
        
        if k == 0:
            # C[0] = sqrt(1/8)
            c = int(round(math.sqrt(1.0/8) * SCALE_CONST))
        else:
            # C[k] = sqrt(2/8)
            c = int(round(math.sqrt(2.0/8) * SCALE_CONST))
        
        X[k] = (soma * c) // (SCALE_CONST * SCALE_CONST)
    
    return X.astype(TYPE)

def idct_matrix_1d(X):
    # IDCT-II: x[n] = sum(C[k] * X[k] * cos(pi*k*(2n+1)/(2N)))
    # C[0] = sqrt(1/N), C[k] = sqrt(2/N) for k>0
    import math
    v = np.asarray(X, dtype=np.int64).flatten()
    x = np.zeros(8, dtype=np.int64)
    
    for n in range(8):
        soma = 0
        for k in range(8):
            cos_val = int(round(math.cos(math.pi * k * (2*n + 1) / 16) * SCALE_CONST))
            
            if k == 0:
                # C[0] = sqrt(1/8)
                c = int(round(math.sqrt(1.0/8) * SCALE_CONST))
            else:
                # C[k] = sqrt(2/8)
                c = int(round(math.sqrt(2.0/8) * SCALE_CONST))
            
            soma += (v[k] * c * cos_val) // (SCALE_CONST * SCALE_CONST)
        
        x[n] = soma
    
    return x.astype(TYPE)

# ----------------- APPROXIMATE DCT (CINTRA-BAYER 2011) -----------------
def dct_approximate_1d(x):
    # Y = (1/sqrt(8)) * T * x onde T[k,n] são {-1, 0, 1}
    v = np.asarray(x, dtype=TYPE).flatten()
    temp = np.zeros(8, dtype=TYPE)
    temp[0] = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7]
    temp[1] = v[0] + v[1] + v[2] - v[5] - v[6] - v[7]
    temp[2] = v[0] - v[3] - v[4] + v[7]
    temp[3] = v[0] - v[2] - v[3] + v[4] + v[5] - v[7]
    temp[4] = v[0] - v[1] - v[2] + v[3] + v[4] - v[5] - v[6] + v[7]
    temp[5] = v[0] - v[1] + v[3] - v[4] + v[6] - v[7]
    temp[6] = -v[1] + v[2] + v[5] - v[6]
    temp[7] = -v[1] + v[2] - v[3] + v[4] - v[5] + v[6]
    return (temp * SCALE_CONST) // SQRT_8

def idct_approximate_1d(Y):
    # x = (1/sqrt(8)) * T^T * Y
    v = np.asarray(Y, dtype=TYPE).flatten()
    temp = np.zeros(8, dtype=TYPE)
    temp[0] = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[7]
    temp[1] = v[0] + v[1] - v[4] - v[5] - v[6] - v[7]
    temp[2] = v[0] + v[1] - v[3] - v[4] + v[6] + v[7]
    temp[3] = v[0] - v[2] - v[3] + v[4] + v[5] - v[7]
    temp[4] = v[0] - v[2] + v[3] + v[4] - v[5] + v[7]
    temp[5] = v[0] - v[1] + v[3] - v[4] + v[6] - v[7]
    temp[6] = v[0] - v[1] + v[5] - v[6] + v[7]
    temp[7] = v[0] - v[1] + v[2] - v[3] + v[4] - v[5] + v[7]
    return (temp * SCALE_CONST) // SQRT_8
