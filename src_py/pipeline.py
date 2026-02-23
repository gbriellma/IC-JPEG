"""
pipeline.py — Pipeline de compressão/descompressão JPEG idêntico à libimage em C.

Cada função replica exatamente a aritmética inteira do código C:
    - codec.c          (compress / decompress)
    - colorspace.c     (rgb_to_ycbcr_batch / ycbcr_to_rgb_batch)
    - quantization.c   (scale_quant_table / quantize / dequantize)
    - utils.c          (extract_blocks / reconstruct_channel)

NOTA: Sem deblocking — o C não faz deblocking.
"""

import numpy as np
from dct import dct_2d, idct_2d
from constantes import TYPE, Q50_LUMA, Q50_CHROMA


# =====================================================================
#  C-style integer division helper (truncation toward zero)
# =====================================================================

def _c_div_vec(a, b):
    """Vectorized C-style integer division (truncation toward zero, b > 0).

    np.int32/int64 floor division in Python rounds toward -∞.
    C integer division truncates toward zero.
    """
    a = np.asarray(a, dtype=np.int64)
    result = np.where(a >= 0, a // b, -((-a) // b))
    return result.astype(np.int32)


# =====================================================================
#  Approximate DCT norm correction — matching quantization.c
# =====================================================================

# Row norms * 1024:  sqrt(8)=2896, sqrt(6)=2508, sqrt(4)=2048
_APPROX_NORM_1024 = np.array([2896, 2508, 2048, 2508, 2896, 2508, 2048, 2508],
                              dtype=np.int64)


def apply_approx_norm_correction(quant_table_flat):
    """Scale Q table by 2-D row norms for the Cintra-Bayer approximate DCT.

    Matches C's apply_approx_norm_correction exactly:
        n = norm[i] * norm[j]           (both * 1024)
        scaled = (qt[i*8+j] * n + 524288) / 1048576
        if scaled < 1: scaled = 1

    Returns a *new* int32 flat array (64,).
    """
    qt = np.asarray(quant_table_flat, dtype=np.int64).flatten().copy()
    for i in range(8):
        for j in range(8):
            n = _APPROX_NORM_1024[i] * _APPROX_NORM_1024[j]
            scaled = (qt[i * 8 + j] * n + 524288) // 1048576
            qt[i * 8 + j] = max(scaled, 1)
    return qt.astype(np.int32)


# =====================================================================
#  Color space conversion — matching colorspace.c
# =====================================================================

def rgb_to_ycbcr(r, g, b):
    """RGB → YCbCr (BT.601), matching C's rgb_to_ycbcr_batch exactly.

    C:
        *y++  = ((299*r + 587*g + 114*b + 500) / 1000) - 128;
        *cb++ = ((-169*r - 331*g + 500*b + 500) / 1000);
        *cr++ = ((500*r - 419*g - 81*b  + 500) / 1000);

    Note: Y is level-shifted by -128 inside colorspace (not in block processing).
    Cb, Cr are centered at 0.
    """
    r = np.asarray(r, dtype=np.int64)
    g = np.asarray(g, dtype=np.int64)
    b = np.asarray(b, dtype=np.int64)

    y  = _c_div_vec(299*r + 587*g + 114*b + 500, 1000) - 128
    cb = _c_div_vec(-169*r - 331*g + 500*b + 500, 1000)
    cr = _c_div_vec(500*r - 419*g - 81*b + 500, 1000)

    return y.astype(np.int32), cb.astype(np.int32), cr.astype(np.int32)


def ycbcr_to_rgb(y, cb, cr):
    """YCbCr → RGB, matching C's ycbcr_to_rgb_batch exactly.

    C:
        int32_t yv = *y++ + 128, cbv = *cb++, crv = *cr++;
        int32_t r  = yv + (1402 * crv + 500) / 1000;
        int32_t g  = yv - (344 * cbv + 714 * crv + 500) / 1000;
        int32_t bv = yv + (1772 * cbv + 500) / 1000;
    """
    yv  = np.asarray(y, dtype=np.int64) + 128
    cbv = np.asarray(cb, dtype=np.int64)
    crv = np.asarray(cr, dtype=np.int64)

    r = yv + _c_div_vec(1402 * crv + 500, 1000)
    g = yv - _c_div_vec(344 * cbv + 714 * crv + 500, 1000)
    b = yv + _c_div_vec(1772 * cbv + 500, 1000)

    out = np.stack([r, g, b], axis=-1)
    return np.clip(out, 0, 255).astype(np.uint8)


# =====================================================================
#  Quantization — matching quantization.c
# =====================================================================

def scale_quant_table(base_flat, k):
    """Scale Q table by k, matching C's scale_quant_table (fixed-point).

    C:
        int32_t k_fixed = (int32_t)(k * 1024);
        scaled = (base[i] * k_fixed) >> 10;
        if (scaled < 1) scaled = 1;
    """
    base = np.asarray(base_flat, dtype=np.int64).flatten()
    k_fixed = int(k * 1024)
    scaled = (base * k_fixed) >> 10
    return np.clip(scaled, 1, None).astype(np.int32)


RECIP_SHIFT = 16


def _compute_reciprocal_table(qt_flat):
    """Compute reciprocal table matching C's compute_reciprocal_table.

    C:  recip[i] = ((1U << 16) + qt[i] / 2) / qt[i];
    """
    qt = np.asarray(qt_flat, dtype=np.int64)
    qt = np.where(qt == 0, 1, qt)
    return (((1 << RECIP_SHIFT) + qt // 2) // qt).astype(np.int64)


def quantize(dct_block_flat, quant_table_flat):
    """Quantize a 64-element block, matching C's quantize_fast exactly.

    C uses reciprocal multiplication for speed (same as ESP32):
        recip = ((1 << 16) + qt/2) / qt
        if (dct >= 0)
            output = ((dct + qt/2) * recip) >> 16
        else
            output = -(((−dct + qt/2) * recip) >> 16)
    """
    dct = np.asarray(dct_block_flat, dtype=np.int64).flatten()
    qt  = np.asarray(quant_table_flat, dtype=np.int64).flatten()
    qt  = np.where(qt == 0, 1, qt)
    recip = _compute_reciprocal_table(qt)

    abs_dct = np.abs(dct)
    half = qt >> 1
    val = abs_dct + half
    magnitude = (val * recip) >> RECIP_SHIFT

    result = np.where(dct >= 0, magnitude, -magnitude)
    return result.astype(np.int32)


def dequantize(quant_block_flat, quant_table_flat):
    """Dequantize: simple multiply, matching C's dequantize.

    C:  output[i] = quant_block[i] * quant_table[i];
    """
    q  = np.asarray(quant_block_flat, dtype=np.int64).flatten()
    qt = np.asarray(quant_table_flat, dtype=np.int64).flatten()
    return (q * qt).astype(np.int32)


# =====================================================================
#  Block extraction / reconstruction — matching utils.c
# =====================================================================

def extract_blocks(channel, width, height):
    """Extract 8×8 blocks from a (height, width) int32 channel.

    Matches C's extract_blocks: zero-padding for edge blocks.

    Returns: blocks (N, 8, 8) int32, num_blocks int
    """
    channel = np.asarray(channel, dtype=np.int32)
    bx = (width + 7) // 8
    by = (height + 7) // 8
    num_blocks = bx * by

    blocks = np.zeros((num_blocks, 8, 8), dtype=np.int32)

    for j in range(by):
        for i in range(bx):
            y0 = j * 8
            x0 = i * 8
            h_copy = min(8, height - y0)
            w_copy = min(8, width - x0)
            idx = j * bx + i
            blocks[idx, :h_copy, :w_copy] = channel[y0:y0+h_copy, x0:x0+w_copy]

    return blocks, num_blocks


def reconstruct_channel(blocks, num_blocks, width, height):
    """Place 8×8 blocks back into a (height, width) channel.

    Matches C's reconstruct_channel.
    """
    bx = (width + 7) // 8
    channel = np.zeros((height, width), dtype=np.int32)

    for j in range((height + 7) // 8):
        for i in range(bx):
            y0 = j * 8
            x0 = i * 8
            h_copy = min(8, height - y0)
            w_copy = min(8, width - x0)
            idx = j * bx + i
            channel[y0:y0+h_copy, x0:x0+w_copy] = blocks[idx, :h_copy, :w_copy]

    return channel


# =====================================================================
#  Full compress / decompress pipeline — matching codec.c
# =====================================================================

def process_channel_compress(channel, quant_table_flat, k_factor, dct_1d_func,
                             is_approx=False):
    """Compress one channel: extract → DCT → quantize.

    Returns: quantized_blocks (N, 64), dct_blocks (N, 64), num_blocks
    """
    h, w = channel.shape
    blocks, num_blocks = extract_blocks(channel, w, h)
    qt = scale_quant_table(quant_table_flat, k_factor)
    if is_approx:
        qt = apply_approx_norm_correction(qt)

    quantized_all = np.zeros((num_blocks, 64), dtype=np.int32)
    dct_all       = np.zeros((num_blocks, 64), dtype=np.int32)

    for b in range(num_blocks):
        blk = blocks[b].astype(np.int32)
        dct_block = dct_2d(blk, dct_1d_func)
        dct_flat  = dct_block.flatten()
        q_flat    = quantize(dct_flat, qt)

        dct_all[b]       = dct_flat
        quantized_all[b] = q_flat

    return quantized_all, dct_all, num_blocks


def process_channel_decompress(quantized_blocks, num_blocks, width, height,
                                quant_table_flat, k_factor, idct_1d_func,
                                is_identity=False, is_approx=False):
    """Decompress one channel: dequantize → IDCT → reconstruct.

    Returns: channel (height, width) int32
    """
    qt = scale_quant_table(quant_table_flat, k_factor)
    if is_approx:
        qt = apply_approx_norm_correction(qt)
    idct_blocks = np.zeros((num_blocks, 8, 8), dtype=np.int32)

    for b in range(num_blocks):
        if is_identity:
            idct_blocks[b] = quantized_blocks[b].reshape(8, 8)
        else:
            deq = dequantize(quantized_blocks[b], qt)
            blk_idct = idct_2d(deq.reshape(8, 8), idct_1d_func)
            idct_blocks[b] = blk_idct.astype(np.int32)

    channel = reconstruct_channel(idct_blocks, num_blocks, width, height)
    return channel


def process_channel(channel_image, quant_table_flat, k_factor,
                    dct_1d_func, idct_1d_func, is_identity=False,
                    is_approx=False):
    """Full compress → decompress for one channel (matching codec.c pipeline).

    Note: channel_image is ALREADY level-shifted (Y has -128 applied in
    rgb_to_ycbcr, matching C). No separate -128 here.

    Returns: reconstructed channel (H, W) int32, quantized_blocks (N, 64) int32
    """
    h, w = channel_image.shape

    # Compress
    quantized, _, num_blocks = process_channel_compress(
        channel_image, quant_table_flat, k_factor, dct_1d_func,
        is_approx=is_approx)

    # Decompress
    recon = process_channel_decompress(
        quantized, num_blocks, w, h,
        quant_table_flat, k_factor, idct_1d_func, is_identity,
        is_approx=is_approx)

    return recon, quantized


# =====================================================================
#  Bitrate estimation — matching C's zigzag-based heuristic
# =====================================================================

def compute_bitrate(quantized_blocks):
    """Compute bitrate (bpp) from quantized blocks using zigzag last-nonzero.

    Matches the C-side bitrate computation (metrics.c) exactly.
    Uses ZIGZAG_SCAN (scan_position → flat_index).
    """
    from constantes import ZIGZAG_SCAN

    total_bits = 0.0
    num_blocks = quantized_blocks.shape[0]

    for i in range(num_blocks):
        flat = quantized_blocks[i].flatten()
        last_nz = -1
        for idx in range(63, -1, -1):
            if flat[ZIGZAG_SCAN[idx]] != 0:
                last_nz = idx
                break

        if last_nz >= 0:
            total_bits += (last_nz + 1) * 8.0

    total_pixels = num_blocks * 64
    bpp = total_bits / total_pixels if total_pixels > 0 else 0.0
    return {'bpp_amplitude': bpp}
