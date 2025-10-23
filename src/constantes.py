import numpy as np

TYPE = np.int32

C1 = 980  # 0.9807852804032304 | cos(pi/16) *1000
S1 = 195  # 0.19509032201612825 | sin(pi/16) *1000
C3 = 831  # 0.8314696123025452 | cos(3pi/16) *1000
S3 = 556  # 0.5555702330196022 | sin(3pi/16) *1000
C6 = 383  # 0.3826834323650898 | cos(6pi/16) *1000
S6 = 924  # 0.9238795325112867 | sin(6pi/16) *1000
SQRT_2 = 1414  # 1.4142135623730951 | sqrt(2) *1000
SCALE_CONST = 1000

# ----------------- APPROXIMATE DCT CONSTANTS (BAS-2008) -----------------
# Cintra-Bayer Approximation - scaling factors for forward transform
APPROX_SCALE_FACTOR = 181  # Scaling factor (181/256 ≈ sqrt(2)/2)

# Multiplicative constants for approximate DCT (scaled by 256 for integer arithmetic)
A_APPROX = 251  # alpha = 251/256 ≈ 0.980785
B_APPROX = 50   # beta = 50/256 ≈ 0.195312
C_APPROX = 213  # gamma = 213/256 ≈ 0.831470
D_APPROX = 142  # delta = 142/256 ≈ 0.554688
E_APPROX = 98   # epsilon = 98/256 ≈ 0.382683
F_APPROX = 236  # zeta = 236/256 ≈ 0.921875

APPROX_SHIFT = 8  # Shift for scaling (2^8 = 256)

# Q50_LUMA = np.ones((8, 8), dtype=TYPE)
# Q50_CHROMA = np.ones((8, 8), dtype=TYPE)


Q50_LUMA = np.array([
    [16, 11, 10, 16, 24, 40, 51, 61], [12, 12, 14, 19, 26, 58, 60, 55],
    [14, 13, 16, 24, 40, 57, 69, 56], [14, 17, 22, 29, 51, 87, 80, 62],
    [18, 22, 37, 56, 68, 109, 103, 77], [24, 35, 55, 64, 81, 104, 113, 92],
    [49, 64, 78, 87, 103, 121, 120, 101], [72, 92, 95, 98, 112, 100, 103, 99]
], dtype=TYPE)

Q50_CHROMA = np.array([
    [17, 18, 24, 47, 99, 99, 99, 99], [18, 21, 26, 66, 99, 99, 99, 99],
    [24, 26, 56, 99, 99, 99, 99, 99], [47, 66, 99, 99, 99, 99, 99, 99],
    [99, 99, 99, 99, 99, 99, 99, 99], [99, 99, 99, 99, 99, 99, 99, 99],
    [99, 99, 99, 99, 99, 99, 99, 99], [99, 99, 99, 99, 99, 99,  99, 99]
], dtype=TYPE)