import numpy as np
from dct import dct_2d, idct_2d
from constantes import TYPE

def split_into_blocks(channel_image):
    h,w = channel_image.shape
    ph = (8 - h % 8) % 8
    pw = (8 - w % 8) % 8
    padded = np.pad(channel_image, ((0,ph),(0,pw)), mode='edge')
    H,W = padded.shape
    blocks = padded.reshape(H//8,8,W//8,8).swapaxes(1,2).reshape(-1,8,8)
    return blocks.astype(TYPE), (H,W), (h,w)

def merge_blocks(blocks_array, padded_shape, original_shape):
    H,W = padded_shape
    h,w = original_shape
    img = blocks_array.reshape(H//8, W//8, 8,8).swapaxes(1,2).reshape(H,W)
    return img[:h,:w].astype(np.int32)

def deblock_image(img):
    h,w = img.shape
    out = img.copy().astype(np.int32)
    for c in range(8, w, 8):
        left = out[:, c-1].astype(np.int32)
        right = out[:, c].astype(np.int32)
        avg = (left + right + 1) // 2
        out[:, c-1] = (left * 3 + avg + 2) // 4
        out[:, c]   = (right * 3 + avg + 2) // 4
    for r in range(8, h, 8):
        top = out[r-1, :].astype(np.int32)
        bot = out[r, :].astype(np.int32)
        avg = (top + bot + 1) // 2
        out[r-1, :] = (top * 3 + avg + 2) // 4
        out[r,   :] = (bot * 3 + avg + 2) // 4
    return out.astype(np.int32)

zz = [
 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
 12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
 35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
 58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
]

def zigzag_flat(block):
    flat = block.flatten()
    return flat[np.array(zz, dtype=np.int32)]

def quantize_block(dct_block, qt):
    qt = qt.astype(np.int32)
    qt = np.where(qt==0, 1, qt).astype(np.int32)
    abs_dct = np.abs(dct_block).astype(np.int32)
    half = (qt // 2).astype(np.int32)
    qpos = (abs_dct + half) // qt
    sign = np.where(dct_block < 0, -1, 1).astype(np.int32)
    q = (qpos * sign).astype(np.int32)
    deq = (q.astype(np.int32) * qt).astype(np.int32)
    return q, deq

def process_channel(channel_image, quant_table, k_factor, dct_1d_func, idct_1d_func):
    blocks, padded_shape, original_shape = split_into_blocks(channel_image)
    qt64 = np.clip(np.round(quant_table.astype(np.int32) * float(k_factor)).astype(np.int32), 1, None)
    quantized_blocks = []
    idct_blocks = []
    for block in blocks:
        block = block.astype(np.int32)
        shifted = (block - 128).astype(np.int32)
        block_dct = dct_2d(shifted, dct_1d_func).astype(np.int32)
        q, deq = quantize_block(block_dct, qt64)
        block_idct = idct_2d(deq, idct_1d_func).astype(np.int32)
        quantized_blocks.append(q.astype(np.int32))
        idct_blocks.append(block_idct.astype(np.int32))
    recon = merge_blocks(np.array(idct_blocks), padded_shape, original_shape) + 128
    recon = deblock_image(recon)
    return recon.astype(np.int32), np.array(quantized_blocks).astype(np.int32)

def calc_bitrate_amplitude(quantized_blocks):
    flat = quantized_blocks.flatten()
    unique, counts = np.unique(flat, return_counts=True)
    total = len(flat)
    prob = counts / total
    entropy = -np.sum(prob * np.log2(prob + 1e-10))
    return entropy

def rgb_to_ycbcr(r,g,b):
    r = r.astype(np.int32); g = g.astype(np.int32); b = b.astype(np.int32)
    y = (299*r + 587*g + 114*b + 500) // 1000
    cb = 128 + ((-168*r - 331*g + 500*b + 500) // 1000)
    cr = 128 + ((500*r - 418*g - 81*b + 500) // 1000)
    return y.astype(np.int32), cb.astype(np.int32), cr.astype(np.int32)

def ycbcr_to_rgb(y,cb,cr):
    y = y.astype(np.int32); cb = cb.astype(np.int32); cr = cr.astype(np.int32)
    r = y + (1402 * (cr - 128) + 500) // 1000
    g = y - ((344 * (cb - 128) + 714 * (cr - 128) + 500) // 1000)
    b = y + (1772 * (cb - 128) + 500) // 1000
    out = np.stack([r,g,b], axis=-1)
    out = np.clip(out, 0, 255).astype(np.uint8)
    return out