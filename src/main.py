import os, time
from PIL import Image
import numpy as np
from dct import dct_loeffler_1d, idct_loeffler_1d, dct_matrix_1d, idct_matrix_1d, dct_approximate_1d, idct_approximate_1d
from pipeline import process_channel, rgb_to_ycbcr, ycbcr_to_rgb
from constantes import Q50_LUMA, Q50_CHROMA
from plots import quality_metrics, compute_bitrate, print_results, plot_psnr, plot_ssim, plot_bitrate, plot_dataset

# ---------------- CONFIGURATION ----------------
DCT_METHOD = 'matrix'  # loeffler | matrix | approximate
INPUT_DIR = 'src/imgs'
K_FACTORS = [2.0, 5.0, 10.0, 15.0]
#K_FACTORS = [1.0]

# ----------------- METHOD SELECTION -----------------
if DCT_METHOD == 'loeffler':
    dct_1d, idct_1d = dct_loeffler_1d, idct_loeffler_1d
elif DCT_METHOD == 'matrix':
    dct_1d, idct_1d = dct_matrix_1d, idct_matrix_1d
elif DCT_METHOD == 'approximate':
    dct_1d, idct_1d = dct_approximate_1d, idct_approximate_1d
else:
    raise ValueError("Method must be 'loeffler', 'matrix', or 'approximate'")

RESULTS_DIR = f'results_{DCT_METHOD}'
PLOTS_DIR = f'plots_{DCT_METHOD}'

def process_image(path):
    img = Image.open(path).convert('RGB')
    arr = np.array(img).astype(np.uint8)
    r,g,b = arr[:,:,0], arr[:,:,1], arr[:,:,2]
    y,cb,cr = rgb_to_ycbcr(r,g,b)
    results = []
    bitrate_list = []
    start_total = time.perf_counter()
    for k in K_FACTORS:
        t0 = time.perf_counter()
        y_rec, y_q = process_channel(y, Q50_LUMA, k, dct_1d, idct_1d)
        cb_rec, cb_q = process_channel(cb, Q50_CHROMA, k, dct_1d, idct_1d)
        cr_rec, cr_q = process_channel(cr, Q50_CHROMA, k, dct_1d, idct_1d)
        recon = ycbcr_to_rgb(y_rec, cb_rec, cr_rec)
        subdir = os.path.join(RESULTS_DIR, os.path.basename(path).split('.')[0])
        os.makedirs(subdir, exist_ok=True)
        try:
            out_img_path = os.path.join(subdir, f"k={int(k)}.png")
            Image.fromarray(recon).save(out_img_path)
        except Exception:
            pass
        t1 = time.perf_counter()
        psnr_val, ssim_val = quality_metrics(arr, recon)
        bitrate_stats_y = compute_bitrate(y_q)
        bitrate_stats_cb = compute_bitrate(cb_q)
        bitrate_stats_cr = compute_bitrate(cr_q)
        combined_stats = {'bpp_zigzag': (bitrate_stats_y['bpp_zigzag']+bitrate_stats_cb['bpp_zigzag']+bitrate_stats_cr['bpp_zigzag'])/3.0,
                          'bpp_amplitude': (bitrate_stats_y['bpp_amplitude']+bitrate_stats_cb['bpp_amplitude']+bitrate_stats_cr['bpp_amplitude'])/3.0,
                          'mean_last': (bitrate_stats_y['mean_last']+bitrate_stats_cb['mean_last']+bitrate_stats_cr['mean_last'])/3.0,
                          'mean_nonzero': (bitrate_stats_y['mean_nonzero']+bitrate_stats_cb['mean_nonzero']+bitrate_stats_cr['mean_nonzero'])/3.0,
                          'total_last_count': bitrate_stats_y['total_last_count']+bitrate_stats_cb['total_last_count']+bitrate_stats_cr['total_last_count']}
        bitrate_list.append((k, combined_stats, os.path.basename(path)))
        t_ms = (t1 - t0) * 1000.0
        results.append((k, psnr_val, ssim_val, t_ms, combined_stats['bpp_zigzag']))
    end_total = time.perf_counter()
    total_ms = (end_total - start_total) * 1000.0
    return results, total_ms, bitrate_list

def process_dataset():
    print(f'\n{"="*60}')
    print(f'SELECTED DCT METHOD: {DCT_METHOD.upper()}')
    print(f'{"="*60}\n')
    os.makedirs(RESULTS_DIR, exist_ok=True)
    os.makedirs(PLOTS_DIR, exist_ok=True)
    files = [f for f in os.listdir(INPUT_DIR) if f.lower().endswith(('.png','.jpg','.jpeg','.bmp'))]
    global_results = []
    global_bitrates = []
    for f in files:
        path = os.path.join(INPUT_DIR, f)
        print('\n>>> Processing:', path)
        results, t_ms, bitrate_list = process_image(path)
        global_results.append(results)
        global_bitrates.append(bitrate_list)
        print_results(results, t_ms, os.path.basename(path), output_dir=RESULTS_DIR, plot_dir=PLOTS_DIR)
        plot_psnr(results, os.path.basename(path), out_dir=PLOTS_DIR)
        plot_ssim(results, os.path.basename(path), out_dir=PLOTS_DIR)
        plot_bitrate(bitrate_list, os.path.basename(path), out_dir=PLOTS_DIR)
    plot_dataset(global_results, global_bitrates, out_dir=PLOTS_DIR)

if __name__ == '__main__':
    if not os.path.exists(INPUT_DIR):
        os.makedirs(INPUT_DIR)
    process_dataset()
