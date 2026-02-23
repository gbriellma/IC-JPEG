"""
main.py — Pipeline principal para processar imagens com a libimage.

Pode usar o codec C via ctypes (modo padrão) ou a reimplementação Python pura
(que agora é idêntica ao C em aritmética).

Uso:
    cd src_py
    python main.py                    # usa libimage C por padrão
    python main.py --pure-python      # usa implementação Python pura
"""

import os
import sys
import time
import argparse

import numpy as np
from PIL import Image

from constantes import Q50_LUMA, Q50_CHROMA, TYPE
from plots import quality_metrics, compute_bitrate, print_results
from plots import plot_psnr, plot_ssim, plot_bitrate, plot_dataset

# ---------------- CONFIGURATION ----------------
INPUT_DIR = 'imgs'
K_FACTORS = [0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0]


# ================================================================
#  Mode A: Use C libimage via ctypes (exact same code as ESP32)
# ================================================================

def _get_libimage():
    """Load the C libimage wrapper."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sys.path.insert(0, os.path.join(root, 'libimage', 'python'))
    from libimage_wrapper import LibImage
    lib_path = os.path.join(root, 'libimage', 'bin', 'libimage.so')
    return LibImage(lib_path)


def process_image_c(path, dct_method, k_factors, lib):
    """Process image using C libimage (compress + decompress)."""
    img = Image.open(path).convert('RGB')
    arr = np.array(img).astype(np.uint8)

    results = []
    bitrate_list = []
    start_total = time.perf_counter()

    for k in k_factors:
        if dct_method == 'identity':
            k_used = 1.0
        else:
            k_used = k

        t0 = time.perf_counter()
        res = lib.process_image(arr, arr.shape[1], arr.shape[0], k_used, dct_method)
        t1 = time.perf_counter()

        recon = res['recon_rgb']

        # Save
        subdir = os.path.join(f'results_{dct_method}',
                              os.path.basename(path).split('.')[0])
        os.makedirs(subdir, exist_ok=True)
        try:
            Image.fromarray(recon).save(os.path.join(subdir, f"k={int(k)}.png"))
        except Exception:
            pass

        psnr_val, ssim_val = quality_metrics(arr, recon)

        stats_y  = compute_bitrate(res['y_quantized'])
        stats_cb = compute_bitrate(res['cb_quantized'])
        stats_cr = compute_bitrate(res['cr_quantized'])
        combined_bpp = (stats_y['bpp_amplitude'] + stats_cb['bpp_amplitude']
                        + stats_cr['bpp_amplitude']) / 3.0
        combined_stats = {'bpp_amplitude': combined_bpp}

        t_ms = (t1 - t0) * 1000.0
        results.append((k, psnr_val, ssim_val, t_ms, combined_bpp))
        bitrate_list.append((k, combined_stats, os.path.basename(path)))

    end_total = time.perf_counter()
    total_ms = (end_total - start_total) * 1000.0
    return results, total_ms, bitrate_list


# ================================================================
#  Mode B: Pure Python (identical arithmetic to C)
# ================================================================

def process_image_python(path, dct_method, k_factors):
    """Process image using pure Python pipeline (matching C exactly)."""
    from dct import (dct_loeffler_1d, idct_loeffler_1d,
                     dct_matrix_1d, idct_matrix_1d,
                     dct_approximate_1d, idct_approximate_1d,
                     dct_identity_1d, idct_identity_1d)
    from pipeline import rgb_to_ycbcr, ycbcr_to_rgb, process_channel

    METHOD_MAP = {
        'loeffler':    (dct_loeffler_1d,    idct_loeffler_1d),
        'matrix':      (dct_matrix_1d,      idct_matrix_1d),
        'approximate': (dct_approximate_1d, idct_approximate_1d),
        'approx':      (dct_approximate_1d, idct_approximate_1d),
        'identity':    (dct_identity_1d,    idct_identity_1d),
    }

    dct_1d, idct_1d = METHOD_MAP[dct_method]

    img = Image.open(path).convert('RGB')
    arr = np.array(img).astype(np.uint8)
    r, g, b = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
    y, cb, cr = rgb_to_ycbcr(r, g, b)

    is_identity = (dct_method == 'identity')
    is_approx = (dct_method in ('approximate', 'approx'))

    results = []
    bitrate_list = []
    start_total = time.perf_counter()

    for k in k_factors:
        t0 = time.perf_counter()

        if is_identity:
            k_used = 1.0
            q_luma = np.ones(64, dtype=np.int32)
            q_chroma = np.ones(64, dtype=np.int32)
        else:
            k_used = k
            q_luma = Q50_LUMA
            q_chroma = Q50_CHROMA

        y_rec, y_q = process_channel(y, q_luma, k_used, dct_1d, idct_1d,
                                     is_identity, is_approx=is_approx)
        cb_rec, cb_q = process_channel(cb, q_chroma, k_used, dct_1d, idct_1d,
                                       is_identity, is_approx=is_approx)
        cr_rec, cr_q = process_channel(cr, q_chroma, k_used, dct_1d, idct_1d,
                                       is_identity, is_approx=is_approx)

        recon = ycbcr_to_rgb(y_rec, cb_rec, cr_rec)

        # Save
        subdir = os.path.join(f'results_{dct_method}',
                              os.path.basename(path).split('.')[0])
        os.makedirs(subdir, exist_ok=True)
        try:
            Image.fromarray(recon).save(os.path.join(subdir, f"k={int(k)}.png"))
        except Exception:
            pass

        t1 = time.perf_counter()

        psnr_val, ssim_val = quality_metrics(arr, recon)

        stats_y  = compute_bitrate(y_q)
        stats_cb = compute_bitrate(cb_q)
        stats_cr = compute_bitrate(cr_q)
        combined_bpp = (stats_y['bpp_amplitude'] + stats_cb['bpp_amplitude']
                        + stats_cr['bpp_amplitude']) / 3.0
        combined_stats = {'bpp_amplitude': combined_bpp}

        t_ms = (t1 - t0) * 1000.0
        results.append((k, psnr_val, ssim_val, t_ms, combined_bpp))
        bitrate_list.append((k, combined_stats, os.path.basename(path)))

    end_total = time.perf_counter()
    total_ms = (end_total - start_total) * 1000.0
    return results, total_ms, bitrate_list


# ================================================================
#  Main
# ================================================================

def process_dataset(dct_method, use_python=False):
    print(f'\n{"="*60}')
    print(f'DCT METHOD: {dct_method.upper()}')
    print(f'ENGINE:     {"Python puro" if use_python else "C libimage"}')
    print(f'{"="*60}\n')

    results_dir = f'results_{dct_method}'
    plots_dir   = f'plots_{dct_method}'
    os.makedirs(results_dir, exist_ok=True)
    os.makedirs(plots_dir, exist_ok=True)

    lib = None
    if not use_python:
        lib = _get_libimage()
        print(f'  libimage version: {lib.version()}')

    files = sorted([
        f for f in os.listdir(INPUT_DIR)
        if f.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp'))
    ])

    if not files:
        print(f'  Nenhuma imagem encontrada em {INPUT_DIR}/')
        return

    global_results = []
    global_bitrates = []

    for f in files:
        path = os.path.join(INPUT_DIR, f)
        print('\n>>> Processing:', path)

        if use_python:
            results, t_ms, bitrate_list = process_image_python(
                path, dct_method, K_FACTORS)
        else:
            results, t_ms, bitrate_list = process_image_c(
                path, dct_method, K_FACTORS, lib)

        global_results.append(results)
        global_bitrates.append(bitrate_list)

        print_results(results, t_ms, os.path.basename(path),
                      output_dir=results_dir, plot_dir=plots_dir)
        plot_psnr(results, os.path.basename(path), out_dir=plots_dir)
        plot_ssim(results, os.path.basename(path), out_dir=plots_dir)
        plot_bitrate(bitrate_list, os.path.basename(path), out_dir=plots_dir)

    plot_dataset(global_results, global_bitrates, out_dir=plots_dir)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='IC-JPEG image processing pipeline')
    parser.add_argument('--method', default='loeffler',
                        choices=['loeffler', 'matrix', 'approximate', 'approx', 'identity'],
                        help='DCT method (default: loeffler)')
    parser.add_argument('--pure-python', action='store_true',
                        help='Use pure Python implementation instead of C libimage')
    parser.add_argument('--input-dir', default=None,
                        help='Input images directory (default: imgs)')
    args = parser.parse_args()

    if args.input_dir:
        INPUT_DIR = args.input_dir

    if not os.path.exists(INPUT_DIR):
        os.makedirs(INPUT_DIR)

    process_dataset(args.method, use_python=args.pure_python)
