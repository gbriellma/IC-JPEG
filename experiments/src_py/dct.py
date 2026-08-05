"""
dct.py -- implementacoes fieis aos artigos e ao codigo C.

Loeffler 1989 e Matrix (DCT ortonormal): float32 via struct, sem math, sem numpy.
RDCT Cintra-Bayer 2011 e Silveira 2022 (j=3 e j=7): int32 puro, inlineado.
Identity: passagem direta.

Fontes: docs/PDFs/Loeffler_FastDCT.pdf
        docs/PDFs/cintra-bayer-dct-approximation-ieee-spl.pdf
        docs/PDFs/dasilveira2022.pdf
        libimage/src/dct_loeffler.c
        libimage/src/dct_matrix.c
        libimage/src/dct_approx.c
        libimage/src/dct_class.c
"""

import struct


# -------------------------------------------------------------------------
# float32 puro -- sem math, sem numpy
# -------------------------------------------------------------------------

def _f32(x):
    return struct.unpack('f', struct.pack('f', float(x)))[0]


def _roundf(x):
    v = _f32(x)
    return int(v + 0.5) if v >= 0.0 else -int(-v + 0.5)


# -------------------------------------------------------------------------
# Divisao inteira com arredondamento half-away-from-zero
# codec_div_round_symm do C
# -------------------------------------------------------------------------

def _div_round(n, d):
    if n >= 0:
        return (n + d // 2) // d
    else:
        return -((-n + d // 2) // d)


# -------------------------------------------------------------------------
# Constantes Loeffler -- float32, identicas ao dct_loeffler.c
# -------------------------------------------------------------------------

_C1F        = _f32(0.9807852804)   # cos(   pi/16)
_S1F        = _f32(0.1950903220)   # sin(   pi/16)
_C3F        = _f32(0.8314696123)   # cos( 3*pi/16)
_S3F        = _f32(0.5555702330)   # sin( 3*pi/16)
_C6F        = _f32(0.3826834324)   # cos( 6*pi/16)
_S6F        = _f32(0.9238795325)   # sin( 6*pi/16)
_SQRT2F     = _f32(1.4142135624)
_INV_2SQRT2 = _f32(0.3535533906)   # 1 / (2 * sqrt(2))
_HALF       = _f32(0.5)
_TWO        = _f32(2.0)
_ONE_8TH    = _f32(0.125)          # 1/8


# -------------------------------------------------------------------------
# Constantes Matrix -- float32, identicas ao dct_matrix.c  CF[8][8]
# CF[k][n] = alpha_k * cos(pi * k * (2n+1) / 16)
#   alpha_0 = 1/sqrt(8),   alpha_k>0 = 0.5
# -------------------------------------------------------------------------

_CF = [
    [ _f32( 0.3535533906), _f32( 0.3535533906), _f32( 0.3535533906), _f32( 0.3535533906),
      _f32( 0.3535533906), _f32( 0.3535533906), _f32( 0.3535533906), _f32( 0.3535533906) ],
    [ _f32( 0.4903926402), _f32( 0.4157348062), _f32( 0.2777851165), _f32( 0.0975451610),
      _f32(-0.0975451610), _f32(-0.2777851165), _f32(-0.4157348062), _f32(-0.4903926402) ],
    [ _f32( 0.4619397663), _f32( 0.1913417162), _f32(-0.1913417162), _f32(-0.4619397663),
      _f32(-0.4619397663), _f32(-0.1913417162), _f32( 0.1913417162), _f32( 0.4619397663) ],
    [ _f32( 0.4157348062), _f32(-0.0975451610), _f32(-0.4903926402), _f32(-0.2777851165),
      _f32( 0.2777851165), _f32( 0.4903926402), _f32( 0.0975451610), _f32(-0.4157348062) ],
    [ _f32( 0.3535533906), _f32(-0.3535533906), _f32(-0.3535533906), _f32( 0.3535533906),
      _f32( 0.3535533906), _f32(-0.3535533906), _f32(-0.3535533906), _f32( 0.3535533906) ],
    [ _f32( 0.2777851165), _f32(-0.4903926402), _f32( 0.0975451610), _f32( 0.4157348062),
      _f32(-0.4157348062), _f32(-0.0975451610), _f32( 0.4903926402), _f32(-0.2777851165) ],
    [ _f32( 0.1913417162), _f32(-0.4619397663), _f32( 0.4619397663), _f32(-0.1913417162),
      _f32(-0.1913417162), _f32( 0.4619397663), _f32(-0.4619397663), _f32( 0.1913417162) ],
    [ _f32( 0.0975451610), _f32(-0.2777851165), _f32( 0.4157348062), _f32(-0.4903926402),
      _f32( 0.4903926402), _f32(-0.4157348062), _f32( 0.2777851165), _f32(-0.0975451610) ],
]


# -------------------------------------------------------------------------
# Auxiliar para transforms 2D
# -------------------------------------------------------------------------

def _rows8(block):
    if hasattr(block[0], '__iter__'):
        return [[int(block[y][x]) for x in range(8)] for y in range(8)]
    v = [int(block[i]) for i in range(64)]
    return [v[i*8:(i+1)*8] for i in range(8)]


# =========================================================================
# Loeffler 1989  --  float32
# "Practical Fast 1-D DCT Algorithms with 11 Multiplications"
# Loeffler, Ligtenberg, Moschytz -- ICASSP 1989
#
# Implementacao identica ao dct_loeffler.c:
#   entradas int32 -> float32, todas operacoes em float32, saidas roundf.
# =========================================================================

def dct_loeffler_1d(x):
    v0=_f32(x[0]); v1=_f32(x[1]); v2=_f32(x[2]); v3=_f32(x[3])
    v4=_f32(x[4]); v5=_f32(x[5]); v6=_f32(x[6]); v7=_f32(x[7])

    s07=_f32(v0+v7); d07=_f32(v0-v7)
    s16=_f32(v1+v6); d16=_f32(v1-v6)
    s25=_f32(v2+v5); d25=_f32(v2-v5)
    s34=_f32(v3+v4); d34=_f32(v3-v4)

    e0=_f32(s07+s34); e1=_f32(s16+s25)
    e2=_f32(s16-s25); e3=_f32(s07-s34)
    o0=_f32(d07+d34); o1=_f32(d16+d25)
    o2=_f32(d16-d25); o3=_f32(d07-d34)

    out = [0]*8
    out[0] = _roundf(_f32(_f32(e0+e1)*_INV_2SQRT2))
    out[4] = _roundf(_f32(_f32(e0-e1)*_INV_2SQRT2))
    out[2] = _roundf(_f32(_f32(_f32(_C6F*e2)+_f32(_S6F*e3))*_HALF))
    out[6] = _roundf(_f32(_f32(-_f32(_S6F*e2)+_f32(_C6F*e3))*_HALF))
    out[1] = _roundf(_f32(_f32(_f32(_C3F*o0)+_f32(_C1F*o1)+_f32(_S1F*o2)+_f32(_S3F*o3))*_INV_2SQRT2))
    out[3] = _roundf(_f32(_f32(_f32(_S1F*o0)-_f32(_C3F*o1)+_f32(_S3F*o2)+_f32(_C1F*o3))*_INV_2SQRT2))
    out[5] = _roundf(_f32(_f32(_f32(_C1F*o0)-_f32(_S3F*o1)-_f32(_C3F*o2)-_f32(_S1F*o3))*_INV_2SQRT2))
    out[7] = _roundf(_f32(_f32(-_f32(_S3F*o0)+_f32(_S1F*o1)-_f32(_C1F*o2)+_f32(_C3F*o3))*_INV_2SQRT2))
    return out


def idct_loeffler_1d(x):
    z0=_f32(x[0]+x[0]); z1=_f32(x[1]+x[1]); z2=_f32(x[2]+x[2]); z3=_f32(x[3]+x[3])
    z4=_f32(x[4]+x[4]); z5=_f32(x[5]+x[5]); z6=_f32(x[6]+x[6]); z7=_f32(x[7]+x[7])

    t0=_f32(z0*_SQRT2F); t4=_f32(z4*_SQRT2F)
    e0_2=_f32(t0+t4);    e1_2=_f32(t0-t4)
    e2_2=_f32(_TWO*_f32(_f32(_C6F*z2)-_f32(_S6F*z6)))
    e3_2=_f32(_TWO*_f32(_f32(_S6F*z2)+_f32(_C6F*z6)))

    s07_4=_f32(e0_2+e3_2); s34_4=_f32(e0_2-e3_2)
    s16_4=_f32(e1_2+e2_2); s25_4=_f32(e1_2-e2_2)

    n0=_f32(_f32(_C3F*z1)+_f32(_S1F*z3)+_f32(_C1F*z5)-_f32(_S3F*z7))
    n1=_f32(_f32(_C1F*z1)-_f32(_C3F*z3)-_f32(_S3F*z5)+_f32(_S1F*z7))
    n2=_f32(_f32(_S1F*z1)+_f32(_S3F*z3)-_f32(_C3F*z5)-_f32(_C1F*z7))
    n3=_f32(_f32(_S3F*z1)+_f32(_C1F*z3)-_f32(_S1F*z5)+_f32(_C3F*z7))

    d07_4=_f32(_f32(n0+n3)*_SQRT2F); d34_4=_f32(_f32(n0-n3)*_SQRT2F)
    d16_4=_f32(_f32(n1+n2)*_SQRT2F); d25_4=_f32(_f32(n1-n2)*_SQRT2F)

    return [
        _roundf(_f32(_f32(s07_4+d07_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s16_4+d16_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s25_4+d25_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s34_4+d34_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s34_4-d34_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s25_4-d25_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s16_4-d16_4)*_ONE_8TH)),
        _roundf(_f32(_f32(s07_4-d07_4)*_ONE_8TH)),
    ]


# =========================================================================
# Matrix DCT  --  float32
# CF[k][n] = alpha_k * cos(pi*k*(2n+1)/16)
#
# Acumulacao identica ao dct_matrix.c:
#   sum = 0.0f;  for n:  sum += (float)src[n] * CF[k][n];
# =========================================================================

def dct_matrix_1d(x):
    out = [0]*8
    for k in range(8):
        acc = _f32(0.0)
        row = _CF[k]
        for n in range(8):
            acc = _f32(acc + _f32(_f32(x[n]) * row[n]))
        out[k] = _roundf(acc)
    return out


def idct_matrix_1d(X):
    out = [0]*8
    for n in range(8):
        acc = _f32(0.0)
        for k in range(8):
            acc = _f32(acc + _f32(_f32(X[k]) * _CF[k][n]))
        out[n] = _roundf(acc)
    return out


# =========================================================================
# RDCT  Cintra & Bayer 2011  --  int32 puro
# "A DCT Approximation for Image Compression"
# IEEE Signal Processing Letters, Vol. 18, No. 10, Oct 2011
#
# C0 = round(2 * C_ortonormal)  ->  elementos em {-1, 0, 1}
# Implementacao identica ao dct_approx.c.
# =========================================================================

def dct_rdct_1d(x):
    x0=int(x[0]); x1=int(x[1]); x2=int(x[2]); x3=int(x[3])
    x4=int(x[4]); x5=int(x[5]); x6=int(x[6]); x7=int(x[7])
    return [
        x0+x1+x2+x3+x4+x5+x6+x7,    # [1, 1, 1, 1, 1, 1, 1, 1]
        x0+x1+x2      -x5-x6-x7,     # [1, 1, 1, 0, 0,-1,-1,-1]
        x0      -x3-x4      +x7,     # [1, 0, 0,-1,-1, 0, 0, 1]
        x0   -x2-x3+x4+x5   -x7,    # [1, 0,-1,-1, 1, 1, 0,-1]
        x0-x1-x2+x3+x4-x5-x6+x7,    # [1,-1,-1, 1, 1,-1,-1, 1]
        x0-x1   +x3-x4   +x6-x7,    # [1,-1, 0, 1,-1, 0, 1,-1]
           -x1+x2      +x5-x6,       # [0,-1, 1, 0, 0, 1,-1, 0]
           -x1+x2-x3+x4-x5+x6,      # [0,-1, 1,-1, 1,-1, 1, 0]
    ]


def idct_rdct_1d(Y):
    # fatores de escala por linha: 3, 4, 6, 4, 3, 4, 6, 4  -- denominador 24
    a=int(Y[0]); b=int(Y[1]); c=int(Y[2]); d=int(Y[3])
    e=int(Y[4]); f=int(Y[5]); g=int(Y[6]); h=int(Y[7])
    z0=a+a+a
    z1=b+b+b+b
    z2=c+c+c+c+c+c
    z3=d+d+d+d
    z4=e+e+e
    z5=f+f+f+f
    z6=g+g+g+g+g+g
    z7=h+h+h+h
    return [
        _div_round(z0+z1+z2+z3+z4+z5, 24),
        _div_round(z0+z1-z4-z5-z6-z7, 24),
        _div_round(z0+z1-z3-z4+z6+z7, 24),
        _div_round(z0-z2-z3+z4+z5-z7, 24),
        _div_round(z0-z2+z3+z4-z5+z7, 24),
        _div_round(z0-z1+z3-z4+z6-z7, 24),
        _div_round(z0-z1-z4+z5-z6+z7, 24),
        _div_round(z0-z1+z2-z3+z4-z5, 24),
    ]


# =========================================================================
# Identity  --  passagem direta (para validacao do codec)
# =========================================================================

def dct_identity_1d(x):
    return [int(x[i]) for i in range(8)]

def idct_identity_1d(X):
    return [int(X[i]) for i in range(8)]


# =========================================================================
# Silveira et al. 2022  j=3  --  int32 puro
# "A Class of Low-Complexity DCT-Like Transforms for Image and Video Coding"
# IEEE TCSVT, Vol. 32, No. 7, July 2022  --  Tabela I, j=3
#
# a = [1, 0, 0, 1, 1, 0, 0, 1]
# Implementacao identica ao dct_class.c dct_j3_1d_stride.
# =========================================================================

def dct_silveira_j3_1d(x):
    x0=int(x[0]); x1=int(x[1]); x2=int(x[2]); x3=int(x[3])
    x4=int(x[4]); x5=int(x[5]); x6=int(x[6]); x7=int(x[7])
    return [
        x0+x1+x2+x3+x4+x5+x6+x7,   # [1, 1, 1, 1, 1, 1, 1, 1]
        x0+x1            -x6-x7,    # [1, 1, 0, 0, 0, 0,-1,-1]
        x0      -x3-x4      +x7,    # [1, 0, 0,-1,-1, 0, 0, 1]
           -x2-x3+x4+x5,            # [0, 0,-1,-1, 1, 1, 0, 0]
        x0-x1-x2+x3+x4-x5-x6+x7,   # [1,-1,-1, 1, 1,-1,-1, 1]
        x0-x1            +x6-x7,    # [1,-1, 0, 0, 0, 0, 1,-1]
           -x1+x2      +x5-x6,      # [0,-1, 1, 0, 0, 1,-1, 0]
               x2-x3+x4-x5,         # [0, 0, 1,-1, 1,-1, 0, 0]
    ]


def idct_silveira_j3_1d(Y):
    # fatores: 1, 2, 2, 2, 1, 2, 2, 2  -- denominador 8
    a=int(Y[0]); b=int(Y[1]); c=int(Y[2]); d=int(Y[3])
    e=int(Y[4]); f=int(Y[5]); g=int(Y[6]); h=int(Y[7])
    z0=a; z1=b+b; z2=c+c; z3=d+d; z4=e; z5=f+f; z6=g+g; z7=h+h
    return [
        _div_round(z0+z1+z2+z4+z5, 8),
        _div_round(z0+z1-z4-z5-z6, 8),
        _div_round(z0-z3-z4+z6+z7, 8),
        _div_round(z0-z2-z3+z4-z7, 8),
        _div_round(z0-z2+z3+z4+z7, 8),
        _div_round(z0+z3-z4+z6-z7, 8),
        _div_round(z0-z1-z4+z5-z6, 8),
        _div_round(z0-z1+z2+z4-z5, 8),
    ]


# =========================================================================
# Silveira et al. 2022  j=7  --  int32 puro
# Tabela I, j=7:  a = [1, 1/2, 1/2, 1, 1, 1/2, 1/2, 1]
#
# A DCT direta calcula 2*T(a) para evitar truncamento dos coeficientes 1/2.
# A IDCT aplica D^{-1} T^T com fatores inteiros e divisao / 144.
# Implementacao identica ao dct_class.c dct_j7_1d_stride / idct_j7_1d_stride.
# =========================================================================

def dct_silveira_j7_1d(x):
    x0=int(x[0]); x1=int(x[1]); x2=int(x[2]); x3=int(x[3])
    x4=int(x[4]); x5=int(x[5]); x6=int(x[6]); x7=int(x[7])
    s = x0+x1+x2+x3+x4+x5+x6+x7
    return [
        s+s,
        (x0+x0)+(x1+x1)+x2-x5-(x6+x6)-(x7+x7),
        (x0-x3-x4+x7)+(x0-x3-x4+x7),
        x0-(x2+x2)-(x3+x3)+(x4+x4)+(x5+x5)-x7,
        (x0-x1-x2+x3+x4-x5-x6+x7)+(x0-x1-x2+x3+x4-x5-x6+x7),
        (x0+x0)-(x1+x1)+x3-x4+(x6+x6)-(x7+x7),
        (-x1+x2+x5-x6)+(-x1+x2+x5-x6),
        -x1+(x2+x2)-(x3+x3)+(x4+x4)-(x5+x5)+x6,
    ]


def idct_silveira_j7_1d(Y):
    # fatores: 9, 16, 18, 16, 9, 16, 18, 16  -- denominador 144
    a=int(Y[0]); b=int(Y[1]); c=int(Y[2]); d=int(Y[3])
    e=int(Y[4]); f=int(Y[5]); g=int(Y[6]); h=int(Y[7])
    z0=a+a+a+a+a+a+a+a+a
    z1=b+b+b+b+b+b+b+b+b+b+b+b+b+b+b+b
    z2=c+c+c+c+c+c+c+c+c+c+c+c+c+c+c+c+c+c
    z3=d+d+d+d+d+d+d+d+d+d+d+d+d+d+d+d
    z4=e+e+e+e+e+e+e+e+e
    z5=f+f+f+f+f+f+f+f+f+f+f+f+f+f+f+f
    z6=g+g+g+g+g+g+g+g+g+g+g+g+g+g+g+g+g+g
    z7=h+h+h+h+h+h+h+h+h+h+h+h+h+h+h+h
    # z1,z3,z5,z7 sao multiplos de 16 -- divisao exata, sem truncamento
    return [
        _div_round(z0+z1+z2+(z3//2)+z4+z5, 144),
        _div_round(z0+z1-z4-z5-z6-(z7//2), 144),
        _div_round(z0+(z1//2)-z3-z4+z6+z7, 144),
        _div_round(z0-z2-z3+z4+(z5//2)-z7, 144),
        _div_round(z0-z2+z3+z4-(z5//2)+z7, 144),
        _div_round(z0-(z1//2)+z3-z4+z6-z7, 144),
        _div_round(z0-z1-z4+z5-z6+(z7//2), 144),
        _div_round(z0-z1+z2-(z3//2)+z4-z5, 144),
    ]


# =========================================================================
# Transforms 2D  (separaveis: linhas depois colunas, depois transposta)
# =========================================================================

def dct_2d(block_8x8, func_dct_1d):
    blk = _rows8(block_8x8)
    temp = [func_dct_1d(blk[y]) for y in range(8)]
    cols = [func_dct_1d([temp[y][x] for y in range(8)]) for x in range(8)]
    return [[cols[x][y] for x in range(8)] for y in range(8)]


def idct_2d(block_dct_8x8, func_idct_1d):
    blk = _rows8(block_dct_8x8)
    cols = [func_idct_1d([blk[y][x] for y in range(8)]) for x in range(8)]
    temp = [[cols[x][y] for x in range(8)] for y in range(8)]
    return [func_idct_1d(temp[y]) for y in range(8)]


# =========================================================================
# Wrappers 2D nomeados
# =========================================================================

def dct_loeffler_2d(b):    return dct_2d(b, dct_loeffler_1d)
def idct_loeffler_2d(b):   return idct_2d(b, idct_loeffler_1d)
def dct_matrix_2d(b):      return dct_2d(b, dct_matrix_1d)
def idct_matrix_2d(b):     return idct_2d(b, idct_matrix_1d)
def dct_rdct_2d(b):        return dct_2d(b, dct_rdct_1d)
def idct_rdct_2d(b):       return idct_2d(b, idct_rdct_1d)
def dct_silveira_j3_2d(b): return dct_2d(b, dct_silveira_j3_1d)
def idct_silveira_j3_2d(b):return idct_2d(b, idct_silveira_j3_1d)
def dct_silveira_j7_2d(b): return dct_2d(b, dct_silveira_j7_1d)
def idct_silveira_j7_2d(b):return idct_2d(b, idct_silveira_j7_1d)
