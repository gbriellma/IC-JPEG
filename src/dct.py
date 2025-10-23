import numpy as np
from constantes import TYPE, C1, S1, C3, S3, C6, S6, SQRT_2, SCALE_CONST
from constantes import A_APPROX, B_APPROX, C_APPROX, D_APPROX, E_APPROX, F_APPROX, APPROX_SHIFT, APPROX_SCALE_FACTOR

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
    DCT-II 1D using direct mathematical definition.
    Matrix implementation for comparative performance analysis.
    Uses int64 internally to avoid overflow, outputs int32.
    """
    import math
    N = 8
    v = np.asarray(x, dtype=np.int64).flatten()  # Use int64 internally
    X = np.zeros(N, dtype=np.int64)
    
    for k in range(N):
        soma = 0
        for n in range(N):
            # Calculate cos(pi * k * (2n + 1) / 16) and scale by SCALE_CONST
            angle = math.pi * k * (2 * n + 1) / 16.0
            cos_val = int(round(math.cos(angle) * SCALE_CONST))
            soma += v[n] * cos_val
        
        # Apply normalization: alpha * sqrt(2/N)
        # alpha = 1/sqrt(2) for k=0, else 1
        # sqrt(2/8) = 1/2, so final scale is 1/(2*sqrt(2)) for k=0, 1/2 for others
        if k == 0:
            # For DC: multiply by 1/sqrt(2) * sqrt(2/8) = 1/sqrt(2) * 1/2 = 1/(2*sqrt(2))
            # Divide by (2 * SQRT_2)
            X[k] = soma // (2 * SQRT_2)
        else:
            # For AC: multiply by sqrt(2/8) = 1/2
            # Divide by 2 * SCALE_CONST
            X[k] = soma // (2 * SCALE_CONST)
    
    return X.astype(TYPE)  # Convert back to int32 for output

def idct_matrix_1d(X):
    """
    IDCT-II 1D using direct mathematical definition.
    Matrix implementation for comparative performance analysis.
    Uses int64 internally to avoid overflow, outputs int32.
    """
    import math
    N = 8
    # Multiply by 2 to reverse the division by 2 from forward DCT
    v = np.asarray(X, dtype=np.int64).flatten() * 2
    x = np.zeros(N, dtype=np.int64)
    
    for n in range(N):
        soma = 0
        for k in range(N):
            # Calculate cos(pi * k * (2n + 1) / 16) and scale by SCALE_CONST
            angle = math.pi * k * (2 * n + 1) / 16.0
            cos_val = int(round(math.cos(angle) * SCALE_CONST))
            
            # Apply alpha factor (1/sqrt(2) for k=0, 1 otherwise)
            if k == 0:
                # DC coefficient: multiply by SQRT_2 to match Loeffler's approach
                soma += (v[k] * cos_val * SQRT_2) // SCALE_CONST
            else:
                # AC coefficients
                soma += v[k] * cos_val
        
        # Divide by SCALE_CONST and then by 8 to match Loeffler's scale
        # Loeffler divides by 2 multiple times (2^3 = 8)
        x[n] = soma // (SCALE_CONST * 8)
    
    return x.astype(TYPE)  # Convert back to int32 for output

# ----------------- APPROXIMATE DCT IMPLEMENTATION (BAS-2008) -----------------
def dct_approximate_1d(x):



    v = np.asarray(x, dtype=TYPE).flatten()
    
    # Stage 1: Butterfly operations (additions/subtractions)
    x0, x1, x2, x3 = v[0], v[1], v[2], v[3]
    x4, x5, x6, x7 = v[4], v[5], v[6], v[7]
    
    a0 = x0 + x7
    a1 = x1 + x6
    a2 = x2 + x5
    a3 = x3 + x4
    a4 = x0 - x7
    a5 = x1 - x6
    a6 = x2 - x5
    a7 = x3 - x4
    
    # Stage 2: Even part
    b0 = a0 + a3
    b1 = a1 + a2
    b2 = a0 - a3
    b3 = a1 - a2
    
    # Stage 3: DCT coefficients for even indices
    # DC component and y4
    y0 = b0 + b1
    y4 = b0 - b1
    
    # y2 and y6 coefficients using approximate rotations
    t0 = (b2 * E_APPROX) >> APPROX_SHIFT
    t1 = (b3 * F_APPROX) >> APPROX_SHIFT
    y2 = t0 + t1
    
    t2 = (b2 * F_APPROX) >> APPROX_SHIFT
    t3 = (b3 * E_APPROX) >> APPROX_SHIFT
    y6 = t2 - t3
    
    # Stage 4: Odd part
    c0 = a4 + a7
    c1 = a5 + a6
    c2 = a5 - a6
    c3 = a4 - a7
    
    # Stage 5: Odd DCT coefficients using approximate rotations
    t4 = (c0 * C_APPROX) >> APPROX_SHIFT
    t5 = (c1 * A_APPROX) >> APPROX_SHIFT
    t6 = (c2 * B_APPROX) >> APPROX_SHIFT
    t7 = (c3 * D_APPROX) >> APPROX_SHIFT
    
    d0 = t4 + t5
    d1 = t6 + t7
    d2 = t4 - t5
    d3 = t6 - t7
    
    y1 = d0 + d1
    y7 = d0 - d1
    y5 = d2 + d3
    y3 = d2 - d3
    
    result = np.array([y0, y1, y2, y3, y4, y5, y6, y7], dtype=TYPE) // 2
    
    return result

def idct_approximate_1d(X):


    v = np.asarray(X, dtype=TYPE).flatten() * 2  # Reverse the "/2" from forward
    
    # Unpack coefficients
    y0, y1, y2, y3 = v[0], v[1], v[2], v[3]
    y4, y5, y6, y7 = v[4], v[5], v[6], v[7]
    
    # Inverse Stage 5: Recover d0, d1, d2, d3 from y1, y7, y5, y3
    d0 = (y1 + y7) // 2
    d1 = (y1 - y7) // 2
    d2 = (y5 + y3) // 2
    d3 = (y5 - y3) // 2
    
    # Recover t4, t5, t6, t7 from d0, d1, d2, d3
    t4 = (d0 + d2) // 2
    t5 = (d0 - d2) // 2
    t6 = (d1 + d3) // 2
    t7 = (d1 - d3) // 2
    
    # Apply inverse rotations
    c0 = (t4 * C_APPROX) >> APPROX_SHIFT
    c1 = (t5 * A_APPROX) >> APPROX_SHIFT
    c2 = (t6 * B_APPROX) >> APPROX_SHIFT
    c3 = (t7 * D_APPROX) >> APPROX_SHIFT
    
    # Inverse Stage 4: Recover a4, a5, a6, a7
    a4 = (c0 + c3) // 2
    a7 = (c0 - c3) // 2
    a5 = (c1 + c2) // 2
    a6 = (c1 - c2) // 2
    
    # Inverse Stage 3: Recover b0, b1, b2, b3 from y0, y4, y2, y6
    b0 = (y0 + y4) // 2
    b1 = (y0 - y4) // 2
    
    # Inverse rotations for even part
    t0_t1 = (y2 * F_APPROX) >> APPROX_SHIFT
    t2_t3 = (y6 * E_APPROX) >> APPROX_SHIFT
    b2 = (t0_t1 - t2_t3)
    
    t0_t1_2 = (y2 * E_APPROX) >> APPROX_SHIFT
    t2_t3_2 = (y6 * F_APPROX) >> APPROX_SHIFT
    b3 = (t0_t1_2 + t2_t3_2)
    
    # Inverse Stage 2
    a0 = (b0 + b2) // 2
    a3 = (b0 - b2) // 2
    a1 = (b1 + b3) // 2
    a2 = (b1 - b3) // 2
    
    # Inverse Stage 1: Final butterfly to recover x0...x7
    x0 = (a0 + a4) // 2
    x7 = (a0 - a4) // 2
    x1 = (a1 + a5) // 2
    x6 = (a1 - a5) // 2
    x2 = (a2 + a6) // 2
    x5 = (a2 - a6) // 2
    x3 = (a3 + a7) // 2
    x4 = (a3 - a7) // 2
    
    return np.array([x0, x1, x2, x3, x4, x5, x6, x7], dtype=TYPE)
