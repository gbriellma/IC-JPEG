import numpy as np
from constantes import TYPE, C1, S1, C3, S3, C6, S6, SQRT_2, SCALE_CONST, T_CINTRA_BAYER, SQRT_8

def dct_loeffler_1d(input_vector):
    v = np.asarray(input_vector, dtype=TYPE).flatten()
    
    sum_07, diff_07 = v[0] + v[7], v[0] - v[7]
    sum_16, diff_16 = v[1] + v[6], v[1] - v[6]
    sum_25, diff_25 = v[2] + v[5], v[2] - v[5]
    sum_34, diff_34 = v[3] + v[4], v[3] - v[4]

    even_0, even_3 = sum_07 + sum_34, sum_07 - sum_34
    even_1, even_2 = sum_16 + sum_25, sum_16 - sum_25

    out_Z0 = ((even_0 + even_1) * SCALE_CONST) // SQRT_2
    out_Z4 = ((even_0 - even_1) * SCALE_CONST) // SQRT_2
    out_Z2 = (C6 * even_2 + S6 * even_3) // SCALE_CONST
    out_Z6 = (-S6 * even_2 + C6 * even_3) // SCALE_CONST

    odd_0, odd_1 = diff_07 + diff_34, diff_16 + diff_25
    odd_2, odd_3 = diff_16 - diff_25, diff_07 - diff_34


    out_Z1 = (C3 * odd_0 + C1 * odd_1 + S1 * odd_2 + S3 * odd_3) // SQRT_2
    out_Z3 = (S1 * odd_0 - C3 * odd_1 + S3 * odd_2 + C1 * odd_3) // SQRT_2
    out_Z5 = (C1 * odd_0 - S3 * odd_1 - C3 * odd_2 - S1 * odd_3) // SQRT_2
    out_Z7 = (-S3 * odd_0 + S1 * odd_1 - C1 * odd_2 + C3 * odd_3) // SQRT_2

    return (np.array([out_Z0, out_Z1, out_Z2, out_Z3, out_Z4, out_Z5, out_Z6, out_Z7], dtype=TYPE) // 2)

def idct_loeffler_1d(dct_coefficients):
    v = np.asarray(dct_coefficients, dtype=TYPE).flatten() * 2

    Z0, Z1, Z2, Z3, Z4, Z5, Z6, Z7 = v

    temp_0 = (Z0 * SQRT_2) // SCALE_CONST
    temp_4 = (Z4 * SQRT_2) // SCALE_CONST

    even_0 = (temp_0 + temp_4) // 2
    even_1 = (temp_0 - temp_4) // 2
    
    even_2 = (C6 * Z2 - S6 * Z6) // SCALE_CONST
    even_3 = (S6 * Z2 + C6 * Z6) // SCALE_CONST

    sum_07 = (even_0 + even_3) // 2
    sum_34 = (even_0 - even_3) // 2
    sum_16 = (even_1 + even_2) // 2
    sum_25 = (even_1 - even_2) // 2

    w1, w3, w5, w7 = Z1, Z3, Z5, Z7

    odd_0 = (C3 * w1 + S1 * w3 + C1 * w5 - S3 * w7) // SQRT_2
    odd_1 = (C1 * w1 - C3 * w3 - S3 * w5 + S1 * w7) // SQRT_2
    odd_2 = (S1 * w1 + S3 * w3 - C3 * w5 - C1 * w7) // SQRT_2
    odd_3 = (S3 * w1 + C1 * w3 - S1 * w5 + C3 * w7) // SQRT_2


    diff_07 = (odd_0 + odd_3) // 2
    diff_34 = (odd_0 - odd_3) // 2
    diff_16 = (odd_1 + odd_2) // 2
    diff_25 = (odd_1 - odd_2) // 2

    x0 = (sum_07 + diff_07) // 2
    x7 = (sum_07 - diff_07) // 2
    x1 = (sum_16 + diff_16) // 2
    x6 = (sum_16 - diff_16) // 2
    x2 = (sum_25 + diff_25) // 2
    x5 = (sum_25 - diff_25) // 2
    x3 = (sum_34 + diff_34) // 2
    x4 = (sum_34 - diff_34) // 2
    return np.array([x0, x1, x2, x3, x4, x5, x6, x7], dtype=TYPE)

def dct_2d(block_8x8, func_dct_1d):
    dct_rows = np.array([func_dct_1d(row) for row in block_8x8])
    dct_cols = np.array([func_dct_1d(col) for col in dct_rows.T])
    return dct_cols.T

def idct_2d(block_dct_8x8, func_idct_1d):
    idct_rows = np.array([func_idct_1d(row) for row in block_dct_8x8])
    idct_cols = np.array([func_idct_1d(col) for col in idct_rows.T])
    return idct_cols.T

# ----------------- MATRIX DCT IMPLEMENTATION -----------------
def dct_matrix_1d(x):
    """
    Formula: X[k] = C[k] * sum(x[n] * cos(pi*k*(2n+1)/(2N))) for n=0..N-1
    where C[0] = sqrt(1/N), C[k>0] = sqrt(2/N)
    """
    import math
    N = 8
    v = np.asarray(x, dtype=np.int64).flatten()
    X = np.zeros(N, dtype=np.int64)
    
    # sqrt(2) = 1.41421356... ≈ 1.414
    sqrt_2_scaled = int(round(math.sqrt(2.0) * SCALE_CONST))  # ≈ 1414
    sqrt_1_over_N = int(round(math.sqrt(1.0/N) * SCALE_CONST))  # sqrt(1/8) ≈ 354
    sqrt_2_over_N = int(round(math.sqrt(2.0/N) * SCALE_CONST))  # sqrt(2/8) ≈ 500
    
    for k in range(N):
        soma = 0
        for n in range(N):
            angle = math.pi * k * (2 * n + 1) / (2.0 * N)
            cos_val = int(round(math.cos(angle) * SCALE_CONST))
            soma += v[n] * cos_val
        
        if k == 0:
            # C[0] = sqrt(1/N)
            X[k] = (soma * sqrt_1_over_N) // (SCALE_CONST * SCALE_CONST)
        else:
            # C[k] = sqrt(2/N)
            X[k] = (soma * sqrt_2_over_N) // (SCALE_CONST * SCALE_CONST)
    
    return X.astype(TYPE)

def idct_matrix_1d(X):
    """
    IDCT-II 1D - Pure matrix implementation.
    Formula: x[n] = sum(C[k] * X[k] * cos(pi*k*(2n+1)/(2N))) for k=0..N-1
    where C[0] = sqrt(1/N), C[k>0] = sqrt(2/N)
    """
    import math
    N = 8
    v = np.asarray(X, dtype=np.int64).flatten()
    x = np.zeros(N, dtype=np.int64)
    
    sqrt_1_over_N = int(round(math.sqrt(1.0/N) * SCALE_CONST))  # sqrt(1/8) ≈ 354
    sqrt_2_over_N = int(round(math.sqrt(2.0/N) * SCALE_CONST))  # sqrt(2/8) ≈ 500
    
    for n in range(N):
        soma = 0
        for k in range(N):
            angle = math.pi * k * (2 * n + 1) / (2.0 * N)
            cos_val = int(round(math.cos(angle) * SCALE_CONST))
            
            if k == 0:
                # C[0] = sqrt(1/N)
                contrib = (v[k] * sqrt_1_over_N * cos_val) // (SCALE_CONST * SCALE_CONST)
            else:
                # C[k] = sqrt(2/N)
                contrib = (v[k] * sqrt_2_over_N * cos_val) // (SCALE_CONST * SCALE_CONST)
            soma += contrib
        
        x[n] = soma
    
    return x.astype(TYPE)

# ----------------- APPROXIMATE DCT IMPLEMENTATION (CINTRA-BAYER 2011) -----------------
def dct_approximate_1d(x):
    """
    1D DCT Aproximada de Cintra-Bayer (2011).
    
    Usa matriz T com apenas {-1, 0, 1} e normalização padrão 1/sqrt(8).
    MUITO MAIS RÁPIDO: apenas somas/subtrações, sem multiplicações complexas!
    
    Forward: Y = (1/sqrt(8)) * T * x
    """
    v = np.asarray(x, dtype=np.int64).flatten()
    
    # Apply T matrix: temp = T * v (apenas somas/subtrações com {-1,0,1})
    temp = np.zeros(8, dtype=np.int64)
    for k in range(8):
        temp[k] = sum(T_CINTRA_BAYER[k, n] * v[n] for n in range(8))
    
    # Normalize by 1/sqrt(8): Y = temp / sqrt(8)
    Y = (temp * SCALE_CONST) // SQRT_8
    
    return Y.astype(TYPE)

def idct_approximate_1d(Y):
    """
    1D IDCT Aproximada de Cintra-Bayer (2011).
    
    Inversa da dct_approximate_1d.
    Inverse: x = (1/sqrt(8)) * T^T * Y
    """
    v = np.asarray(Y, dtype=np.int64).flatten()
    
    # Apply T^T: temp = T^T * Y (apenas somas/subtrações com {-1,0,1})
    temp = np.zeros(8, dtype=np.int64)
    for n in range(8):
        soma = 0
        for k in range(8):
            soma += T_CINTRA_BAYER[k, n] * v[k]  # T^T[n,k] = T[k,n]
        temp[n] = soma
    
    # Normalize by 1/sqrt(8): x = temp / sqrt(8)
    x = (temp * SCALE_CONST) // SQRT_8
    
    return x.astype(TYPE)
