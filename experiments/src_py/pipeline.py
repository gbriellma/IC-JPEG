"""
pipeline.py -- Pure-Python integer pipeline matching libimage.

This module intentionally uses only Python lists and Python arithmetic.
"""

from dct import dct_2d, idct_2d


RECIP_SHIFT = 16
_APPROX_NORM_1024 = [2896, 2508, 2048, 2508, 2896, 2508, 2048, 2508]


def _is_sequence(value):
    return hasattr(value, "__iter__") and not isinstance(value, (str, bytes, bytearray))


def _c_div(a, b):
    return a // b if a >= 0 else -((-a) // b)


def _clip_u8(v):
    if v < 0:
        return 0
    if v > 255:
        return 255
    return v


def _flat64(values):
    if len(values) == 64 and not _is_sequence(values[0]):
        return [int(values[i]) for i in range(64)]

    out = []
    for y in range(len(values)):
        row = values[y]
        if _is_sequence(row):
            for x in range(len(row)):
                out.append(int(row[x]))
                if len(out) == 64:
                    return out
        else:
            out.append(int(row))
            if len(out) == 64:
                return out

    if len(out) != 64:
        raise ValueError("Expected 64 values")
    return out


def apply_approx_norm_correction(quant_table_flat):
    return apply_generic_norm_correction(quant_table_flat, _APPROX_NORM_1024)


def apply_generic_norm_correction(quant_table_flat, norm_1024):
    qt = _flat64(quant_table_flat)
    N = [int(v) for v in norm_1024]
    for i in range(8):
        for j in range(8):
            idx = i*8 + j
            step1 = (qt[idx] * N[i] + 512) // 1024
            scaled = (step1 * N[j] + 512) // 1024
            qt[idx] = max(int(scaled), 1)
    return qt


def rgb_to_ycbcr(r, g, b):
    h = len(r)
    w = len(r[0]) if h else 0

    y = [[0 for _ in range(w)] for _ in range(h)]
    cb = [[0 for _ in range(w)] for _ in range(h)]
    cr = [[0 for _ in range(w)] for _ in range(h)]

    for yy in range(h):
        for xx in range(w):
            rv = int(r[yy][xx])
            gv = int(g[yy][xx])
            bv = int(b[yy][xx])

            y[yy][xx]  = (( 77*rv + 150*gv +  29*bv + 128) >> 8) - 128
            cb[yy][xx] = ((-43*rv -  85*gv + 128*bv + 128) >> 8)
            cr[yy][xx] = ((128*rv - 107*gv -  21*bv + 128) >> 8)

    return y, cb, cr


def ycbcr_to_rgb(y, cb, cr):
    h = len(y)
    w = len(y[0]) if h else 0

    out = [[[0, 0, 0] for _ in range(w)] for _ in range(h)]

    for yy in range(h):
        for xx in range(w):
            yv = int(y[yy][xx]) + 128
            cbv = int(cb[yy][xx])
            crv = int(cr[yy][xx])

            r  = yv + ((359 * crv + 128) >> 8)
            g  = yv - (( 88 * cbv + 183 * crv + 128) >> 8)
            bv = yv + ((454 * cbv + 128) >> 8)

            out[yy][xx][0] = _clip_u8(r)
            out[yy][xx][1] = _clip_u8(g)
            out[yy][xx][2] = _clip_u8(bv)

    return out


def scale_quant_table(base_flat, k):
    base = _flat64(base_flat)
    k_fixed = int(k * 1024)
    out = []
    for q in base:
        scaled = (int(q) * k_fixed) >> 10
        out.append(max(scaled, 1))
    return out


def _compute_reciprocal_table(qt_flat):
    recip = []
    for q in qt_flat:
        qv = int(q) if int(q) != 0 else 1
        recip.append(((1 << RECIP_SHIFT) + qv // 2) // qv)
    return recip


def quantize(dct_block_flat, quant_table_flat):
    dct = _flat64(dct_block_flat)
    qt = [int(q) if int(q) != 0 else 1 for q in _flat64(quant_table_flat)]
    recip = _compute_reciprocal_table(qt)
    out = []
    for i in range(64):
        val = abs(int(dct[i])) + (qt[i] >> 1)
        mag = (val * recip[i]) >> RECIP_SHIFT
        out.append(mag if dct[i] >= 0 else -mag)
    return out


def dequantize(quant_block_flat, quant_table_flat):
    q = _flat64(quant_block_flat)
    qt = _flat64(quant_table_flat)
    return [int(q[i]) * int(qt[i]) for i in range(64)]


def extract_blocks(channel, width, height):
    bx = (width + 7) // 8
    by = (height + 7) // 8
    blocks = []

    for j in range(by):
        for i in range(bx):
            block = [[0 for _ in range(8)] for _ in range(8)]
            y0 = j * 8
            x0 = i * 8

            for yy in range(8):
                src_y = y0 + yy
                if src_y >= height:
                    continue

                for xx in range(8):
                    src_x = x0 + xx
                    if src_x >= width:
                        continue

                    block[yy][xx] = int(channel[src_y][src_x])

            blocks.append(block)

    return blocks, len(blocks)


def reconstruct_channel(blocks, num_blocks, width, height):
    bx = (width + 7) // 8
    by = (height + 7) // 8

    channel = [[0 for _ in range(width)] for _ in range(height)]

    for j in range(by):
        for i in range(bx):
            idx = j * bx + i
            if idx >= num_blocks:
                continue

            block = blocks[idx]
            y0 = j * 8
            x0 = i * 8

            for yy in range(8):
                dst_y = y0 + yy
                if dst_y >= height:
                    continue

                for xx in range(8):
                    dst_x = x0 + xx
                    if dst_x >= width:
                        continue

                    channel[dst_y][dst_x] = int(block[yy][xx])

    return channel


def process_channel_compress(channel, quant_table_flat, k_factor, dct_1d_func,
                             is_approx=False, norm_1024=None):
    h = len(channel)
    w = len(channel[0]) if h else 0

    blocks, num_blocks = extract_blocks(channel, w, h)
    qt = scale_quant_table(quant_table_flat, k_factor)
    if is_approx:
        qt = apply_approx_norm_correction(qt)
    if norm_1024 is not None:
        qt = apply_generic_norm_correction(qt, norm_1024)

    quantized_all = [[0 for _ in range(64)] for _ in range(num_blocks)]
    dct_all = [[0 for _ in range(64)] for _ in range(num_blocks)]

    for b in range(num_blocks):
        dct_block = dct_2d(blocks[b], dct_1d_func)
        dct_flat = [int(dct_block[y][x]) for y in range(8) for x in range(8)]
        q_flat = quantize(dct_flat, qt)
        dct_all[b] = dct_flat
        quantized_all[b] = q_flat
    return quantized_all, dct_all, num_blocks


def process_channel_decompress(quantized_blocks, num_blocks, width, height,
                                quant_table_flat, k_factor, idct_1d_func,
                                is_identity=False, is_approx=False,
                                norm_1024=None):
    qt = scale_quant_table(quant_table_flat, k_factor)
    if is_approx:
        qt = apply_approx_norm_correction(qt)
    if norm_1024 is not None:
        qt = apply_generic_norm_correction(qt, norm_1024)

    idct_blocks = [[[0 for _ in range(8)] for _ in range(8)]
                   for _ in range(num_blocks)]

    for b in range(num_blocks):
        if is_identity:
            flat = _flat64(quantized_blocks[b])
            idct_blocks[b] = [flat[i*8:(i+1)*8] for i in range(8)]
        else:
            deq = dequantize(quantized_blocks[b], qt)
            rows = [deq[i*8:(i+1)*8] for i in range(8)]
            blk_idct = idct_2d(rows, idct_1d_func)
            idct_blocks[b] = blk_idct
    return reconstruct_channel(idct_blocks, num_blocks, width, height)


def process_channel(channel_image, quant_table_flat, k_factor,
                    dct_1d_func, idct_1d_func, is_identity=False,
                    is_approx=False, norm_1024=None):
    h = len(channel_image)
    w = len(channel_image[0]) if h else 0

    quantized, _, num_blocks = process_channel_compress(
        channel_image, quant_table_flat, k_factor, dct_1d_func,
        is_approx=is_approx, norm_1024=norm_1024)
    recon = process_channel_decompress(
        quantized, num_blocks, w, h, quant_table_flat, k_factor,
        idct_1d_func, is_identity, is_approx=is_approx,
        norm_1024=norm_1024)
    return recon, quantized


def compute_bitrate(quantized_blocks):
    from constantes import ZIGZAG_SCAN

    total_bits = 0.0
    num_blocks = len(quantized_blocks)

    for i in range(num_blocks):
        flat = _flat64(quantized_blocks[i])
        last_nz = -1
        for idx in range(63, -1, -1):
            if flat[int(ZIGZAG_SCAN[idx])] != 0:
                last_nz = idx
                break
        if last_nz >= 0:
            total_bits += (last_nz + 1) * 8.0
    total_pixels = num_blocks * 64
    return {'bpp_amplitude': total_bits / total_pixels if total_pixels > 0 else 0.0}
