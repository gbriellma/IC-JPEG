import numpy as np
from dct import dct_2d, idct_2d
from constantes import TYPE

def split_into_blocks(channel_image):
    original_height, original_width = channel_image.shape
    pad_height = (8 - original_height % 8) % 8
    pad_width = (8 - original_width % 8) % 8
    padded = np.pad(channel_image, ((0, pad_height), (0, pad_width)), mode='edge')
    ph, pw = padded.shape
    blocks = padded.reshape(ph // 8, 8, pw // 8, 8).swapaxes(1, 2).reshape(-1, 8, 8)
    return blocks.astype(TYPE), (ph, pw), (original_height, original_width)

def merge_blocks(blocks_array, padded_shape, original_shape):
    ph, pw = padded_shape
    oh, ow = original_shape
    img = blocks_array.reshape(ph // 8, pw // 8, 8, 8).swapaxes(1, 2).reshape(ph, pw)
    return img[:oh, :ow]

def process_channel(channel_image, quant_table, k_factor, dct_1d_func, idct_1d_func):
    shifted = (channel_image.astype(TYPE) - 128) * 1000
    blocks, padded_shape, original_shape = split_into_blocks(shifted)
    qt64 = (quant_table.astype(np.int64) * int(k_factor) * 1000)
    quantized_blocks = []
    idct_blocks = []
    for block in blocks:
        block_dct = dct_2d(block, dct_1d_func).astype(np.int64)
        sign = np.where(block_dct < 0, -1, 1).astype(np.int64)
        half = (qt64 // 2).astype(np.int64)
        q = ((block_dct + sign * half) // qt64).astype(np.int32)
        deq = (q.astype(np.int64) * qt64).astype(np.int64)
        block_idct = idct_2d(deq.astype(np.int64), idct_1d_func).astype(np.int64)
        quantized_blocks.append(q.astype(np.int32))
        idct_blocks.append(block_idct.astype(np.int64))
    recon = merge_blocks(np.array(idct_blocks), padded_shape, original_shape) + (128 * 1000)
    recon = (recon // 1000).astype(np.int32)
    return recon, np.array(quantized_blocks)

def rgb_to_ycbcr(r, g, b):
    r = r.astype(np.int32)
    g = g.astype(np.int32)
    b = b.astype(np.int32)
    y  = (299 * r + 587 * g + 114 * b) // 1000
    cb = 128 + ((-168 * r - 331 * g + 500 * b) // 1000)
    cr = 128 + ((500 * r - 418 * g - 81 * b) // 1000)
    return y.astype(np.int32), cb.astype(np.int32), cr.astype(np.int32)

def ycbcr_to_rgb(y, cb, cr):
    r = y + (1402 * (cr - 128)) // 1000
    g = y - (344 * (cb - 128) + 714 * (cr - 128)) // 1000
    b = y + (1772 * (cb - 128)) // 1000
    out = np.stack([r, g, b], axis=-1)
    return np.clip(out, 0, 255).astype(np.uint8)
