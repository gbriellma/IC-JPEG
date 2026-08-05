"""
main.py -- Pipeline principal para processar imagens com a libimage.

Pode usar o codec C via ctypes ou a reimplementacao Python pura. O modo C
continua usando PIL/NumPy para conversar com o wrapper. O modo --pure-python
le e salva PPM P6 com listas Python, sem bibliotecas externas.

Uso:
    cd src_py
    python main.py                    # usa libimage C por padrao
    python main.py --pure-python      # usa somente Python padrao e PPM P6
"""

import argparse
import math
import os
import sys
import time

from constantes import Q50_LUMA, Q50_CHROMA, CLASS_NORM_1024

# ---------------- CONFIGURATION ----------------
INPUT_DIR = 'imgs'
K_FACTORS = [0.1, 0.2, 0.5, 0.8]


# ------------------------------------------------------------------ PPM I/O --

def _read_token(f):
    token = bytearray()
    while True:
        c = f.read(1)
        if not c:
            return None
        if c == b"#":
            f.readline()
            continue
        if not c.isspace():
            token.extend(c)
            break
    while True:
        c = f.read(1)
        if not c or c.isspace():
            break
        token.extend(c)
    return token.decode("ascii")


def read_ppm(path):
    with open(path, "rb") as f:
        magic = _read_token(f)
        if magic != "P6":
            raise ValueError("Arquivo nao e PPM P6")
        width = int(_read_token(f))
        height = int(_read_token(f))
        maxv = int(_read_token(f))
        if maxv != 255:
            raise ValueError("PPM precisa ter maxval 255")
        raw = f.read(width * height * 3)
        if len(raw) != width * height * 3:
            raise ValueError("Dados PPM incompletos")

    img = []
    pos = 0
    for _ in range(height):
        row = []
        for _ in range(width):
            row.append([raw[pos], raw[pos + 1], raw[pos + 2]])
            pos += 3
        img.append(row)
    return img, width, height


def write_ppm(path, img):
    height = len(img)
    width = len(img[0]) if height else 0
    with open(path, "wb") as f:
        f.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
        out = bytearray()
        for y in range(height):
            for x in range(width):
                r, g, b = img[y][x]
                out.append(max(0, min(255, int(r))))
                out.append(max(0, min(255, int(g))))
                out.append(max(0, min(255, int(b))))
        f.write(out)


def split_rgb(img):
    height = len(img)
    width = len(img[0]) if height else 0

    r = [[0 for _ in range(width)] for _ in range(height)]
    g = [[0 for _ in range(width)] for _ in range(height)]
    b = [[0 for _ in range(width)] for _ in range(height)]

    for yy in range(height):
        for xx in range(width):
            r[yy][xx] = img[yy][xx][0]
            g[yy][xx] = img[yy][xx][1]
            b[yy][xx] = img[yy][xx][2]

    return r, g, b


def _quality_metrics_pure(original, reconstructed):
    return _psnr_pure(original, reconstructed), _ssim_global_rgb(original, reconstructed)


def _psnr_pure(original, reconstructed):
    total = 0.0
    count = 0

    for y in range(len(original)):
        for x in range(len(original[y])):
            for c in range(3):
                diff = int(original[y][x][c]) - int(reconstructed[y][x][c])
                total += diff * diff
                count += 1

    if count == 0 or total == 0:
        return float("inf")

    mse = total / count
    return 10.0 * math.log10((255.0 * 255.0) / mse)


def _ssim_global_rgb(original, reconstructed):
    height = len(original)
    width = len(original[0]) if height else 0
    n = height * width
    if n == 0:
        return 0.0

    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2
    scores = []

    for channel in range(3):
        sx = sy = sx2 = sy2 = sxy = 0.0

        for y in range(height):
            for x in range(width):
                a = float(original[y][x][channel])
                b = float(reconstructed[y][x][channel])
                sx += a
                sy += b
                sx2 += a * a
                sy2 += b * b
                sxy += a * b

        mux = sx / n
        muy = sy / n
        varx = max(0.0, sx2 / n - mux * mux)
        vary = max(0.0, sy2 / n - muy * muy)
        cov = sxy / n - mux * muy
        den = (mux * mux + muy * muy + c1) * (varx + vary + c2)

        if den == 0.0:
            scores.append(1.0)
        else:
            scores.append(((2.0 * mux * muy + c1) * (2.0 * cov + c2)) / den)

    return sum(scores) / len(scores)


def _print_results_plain(results, total_time, image_name, output_dir=None):
    lines = [
        f"--- FINAL RESULTS: {image_name} ---",
        "k       PSNR(dB)   SSIM(g)   Bitrate(bpp)   Time(ms) [Comp/Decomp]",
        "-" * 72,
    ]

    for k, psnr, ssim, time_ms, bitrate, comp_ms, decomp_ms in results:
        psnr_text = "inf" if math.isinf(psnr) else f"{psnr:.2f}"
        lines.append(
            f"{k:<7g} {psnr_text:>8}   {ssim:>7.4f}   {bitrate:>12.6f}   "
            f"{time_ms:>8.2f} [{comp_ms:>6.2f}/{decomp_ms:>6.2f}]"
        )

    total_time_s = total_time / 1000.0
    total_time_min = total_time_s / 60.0
    lines.append("")
    lines.append(
        f"Total processing time: {total_time:.2f} ms | "
        f"{total_time_s:.2f} s | {total_time_min:.2f} min"
    )

    output = "\n".join(lines)
    print(output)

    if output_dir:
        target_dir = os.path.join(output_dir, os.path.basename(image_name).split('.')[0])
        os.makedirs(target_dir, exist_ok=True)
        with open(os.path.join(target_dir, 'results.txt'), 'w', encoding='utf-8') as f:
            f.write(output)


# ================================================================
#  Mode A: Use C libimage via ctypes (exact same code as ESP32)
# ================================================================

def _get_libimage():
    """Load the C libimage wrapper."""
    # main.py lives at <repo>/experiments/src_py/main.py → go up 3 levels
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.path.insert(0, os.path.join(root, 'libimage', 'python'))
    from libimage_wrapper import LibImage
    lib_path = os.path.join(root, 'libimage', 'bin', 'libimage.so')
    return LibImage(lib_path)


def process_image_c(path, dct_method, k_factors, lib):
    """Process image using C libimage (compress + decompress)."""
    import numpy as np
    from PIL import Image
    from plots import quality_metrics, compute_bitrate

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
        comp = lib.compress(arr, arr.shape[1], arr.shape[0], k_used, dct_method)
        t_comp = time.perf_counter()

        recon = lib.decompress(
            comp["y_quantized"],
            comp.get("cb_quantized"), comp.get("cr_quantized"),
            arr.shape[1], arr.shape[0], k_used, dct_method
        )
        t1 = time.perf_counter()

        res = comp
        res['recon_rgb'] = recon

        comp_t_ms = (t_comp - t0) * 1000.0
        decomp_t_ms = (t1 - t_comp) * 1000.0

        # Save
        subdir = os.path.join(f'results_{dct_method}',
                              os.path.basename(path).split('.')[0])
        os.makedirs(subdir, exist_ok=True)
        try:
            Image.fromarray(recon).save(os.path.join(subdir, f"k={k:g}.png"))
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
        results.append((k, psnr_val, ssim_val, t_ms, combined_bpp, comp_t_ms, decomp_t_ms))
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
                     dct_rdct_1d, idct_rdct_1d,
                     dct_identity_1d, idct_identity_1d,
                     dct_silveira_j3_1d, idct_silveira_j3_1d,
                     dct_silveira_j7_1d, idct_silveira_j7_1d)
    from pipeline import (rgb_to_ycbcr, ycbcr_to_rgb,
                          process_channel_compress,
                          process_channel_decompress,
                          compute_bitrate)

    METHOD_MAP = {
        'loeffler':     (dct_loeffler_1d,     idct_loeffler_1d,     None),
        'matrix':       (dct_matrix_1d,       idct_matrix_1d,       None),
        'rdct':         (dct_rdct_1d,         idct_rdct_1d,         None),
        'silveira_j3':  (dct_silveira_j3_1d,  idct_silveira_j3_1d,  CLASS_NORM_1024[3]),
        'silveira_j7':  (dct_silveira_j7_1d,  idct_silveira_j7_1d,  CLASS_NORM_1024[7]),
        'identity':     (dct_identity_1d,     idct_identity_1d,     None),
    }

    dct_1d, idct_1d, norm_1024 = METHOD_MAP[dct_method]

    img, width, height = read_ppm(path)

    is_identity = (dct_method == 'identity')
    is_approx = (dct_method == 'rdct')

    results = []
    bitrate_list = []
    start_total = time.perf_counter()

    for k in k_factors:
        t0 = time.perf_counter()

        r, g, b = split_rgb(img)
        y, cb, cr = rgb_to_ycbcr(r, g, b)

        if is_identity:
            k_used = 1.0
            q_luma = [1 for _ in range(64)]
            q_chroma = [1 for _ in range(64)]
        else:
            k_used = k
            q_luma = Q50_LUMA
            q_chroma = Q50_CHROMA

        # Compress
        y_q, _, num_y = process_channel_compress(
            y, q_luma, k_used, dct_1d, is_approx=is_approx, norm_1024=norm_1024)
        cb_q, _, num_cb = process_channel_compress(
            cb, q_chroma, k_used, dct_1d, is_approx=is_approx, norm_1024=norm_1024)
        cr_q, _, num_cr = process_channel_compress(
            cr, q_chroma, k_used, dct_1d, is_approx=is_approx, norm_1024=norm_1024)
        t_comp = time.perf_counter()

        # Decompress
        y_rec = process_channel_decompress(
            y_q, num_y, width, height, q_luma, k_used,
            idct_1d, is_identity, is_approx=is_approx, norm_1024=norm_1024)
        cb_rec = process_channel_decompress(
            cb_q, num_cb, width, height, q_chroma, k_used,
            idct_1d, is_identity, is_approx=is_approx, norm_1024=norm_1024)
        cr_rec = process_channel_decompress(
            cr_q, num_cr, width, height, q_chroma, k_used,
            idct_1d, is_identity, is_approx=is_approx, norm_1024=norm_1024)

        recon = ycbcr_to_rgb(y_rec, cb_rec, cr_rec)
        t1 = time.perf_counter()

        # Save
        subdir = os.path.join(f'results_{dct_method}',
                              os.path.basename(path).split('.')[0])
        os.makedirs(subdir, exist_ok=True)
        write_ppm(os.path.join(subdir, f"k={k:g}.ppm"), recon)

        psnr_val, ssim_val = _quality_metrics_pure(img, recon)

        stats_y  = compute_bitrate(y_q)
        stats_cb = compute_bitrate(cb_q)
        stats_cr = compute_bitrate(cr_q)
        combined_bpp = (stats_y['bpp_amplitude'] + stats_cb['bpp_amplitude']
                        + stats_cr['bpp_amplitude']) / 3.0
        combined_stats = {'bpp_amplitude': combined_bpp}

        t_ms = (t1 - t0) * 1000.0
        comp_t_ms = (t_comp - t0) * 1000.0
        decomp_t_ms = (t1 - t_comp) * 1000.0
        results.append((k, psnr_val, ssim_val, t_ms, combined_bpp, comp_t_ms, decomp_t_ms))
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

    lib = None
    plot_psnr = plot_ssim = plot_bitrate = plot_dataset = None
    print_results = plot_comp_decomp_time = None

    if not use_python:
        from plots import (print_results, plot_psnr, plot_ssim, plot_bitrate,
                           plot_dataset, plot_comp_decomp_time)

        os.makedirs(plots_dir, exist_ok=True)
        lib = _get_libimage()
        print(f'  libimage version: {lib.version()}')

    extensions = ('.ppm',) if use_python else ('.png', '.jpg', '.jpeg', '.bmp', '.ppm')
    files = sorted([
        f for f in os.listdir(INPUT_DIR)
        if f.lower().endswith(extensions)
    ])

    if not files:
        if use_python:
            print(f'  Nenhum PPM P6 encontrado em {INPUT_DIR}/')
            print('  O modo --pure-python usa apenas .ppm para evitar PIL/NumPy.')
        else:
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

        if use_python:
            _print_results_plain(results, t_ms, os.path.basename(path),
                                 output_dir=results_dir)
        else:
            print_results(results, t_ms, os.path.basename(path),
                          output_dir=results_dir, plot_dir=plots_dir)
            plot_psnr(results, os.path.basename(path), out_dir=plots_dir)
            plot_ssim(results, os.path.basename(path), out_dir=plots_dir)
            plot_bitrate(bitrate_list, os.path.basename(path), out_dir=plots_dir)
            plot_comp_decomp_time(results, os.path.basename(path), out_dir=plots_dir)

    if not use_python:
        plot_dataset(global_results, global_bitrates, out_dir=plots_dir)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='IC-JPEG image processing pipeline')
    parser.add_argument('--method', default='loeffler',
                        choices=['loeffler', 'matrix', 'rdct',
                                 'silveira_j3', 'silveira_j7', 'identity'],
                        help='DCT method (default: loeffler)')
    parser.add_argument('--pure-python', action='store_true',
                        help='Use pure Python implementation over PPM P6 input')
    parser.add_argument('--input-dir', default=None,
                        help='Input images directory (default: imgs)')
    args = parser.parse_args()

    if args.input_dir:
        INPUT_DIR = args.input_dir

    if not os.path.exists(INPUT_DIR):
        os.makedirs(INPUT_DIR)

    process_dataset(args.method, use_python=args.pure_python)
