"""
plots.py — Métricas de qualidade e plotagem de resultados.
"""

import os
import numpy as np
import matplotlib.pyplot as plt
from skimage.metrics import peak_signal_noise_ratio as compute_psnr
from skimage.metrics import structural_similarity as compute_ssim


def quality_metrics(original, reconstructed):
    psnr = compute_psnr(original, reconstructed, data_range=255)
    ssim = compute_ssim(
        original.astype(np.uint8), reconstructed.astype(np.uint8),
        channel_axis=-1, data_range=255, win_size=7)
    return psnr, ssim


def compute_bitrate(quantized_blocks):
    """Bitrate via zigzag last-nonzero, matching C's metrics.c exactly.

    Uses ZIGZAG_SCAN (scan_position → flat_index), the standard JPEG
    zigzag order.  block[ZIGZAG_SCAN[i]] gives the i-th coefficient
    in zigzag scan order (DC first, highest frequency last).
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


def results_table(results, total_time, image_name, directory):
    if not directory:
        return
    os.makedirs(directory, exist_ok=True)
    headers = ["k Factor", "PSNR (dB)", "SSIM", "Bitrate (bpp)",
               "Tempo (ms)", "Tempo (s)", "Tempo (min)"]
    rows = []
    for k, psnr, ssim, time_ms, bitrate in results:
        time_s = time_ms / 1000.0
        time_min = time_s / 60.0
        rows.append([
            f"{int(k)}", f"{psnr:.2f}", f"{ssim:.4f}", f"{bitrate:.6f}",
            f"{time_ms:.2f}", f"{time_s:.2f}", f"{time_min:.2f}"
        ])
    fig_height = 1.5 + 0.4 * len(rows)
    fig, ax = plt.subplots(figsize=(10, fig_height))
    ax.axis('off')
    ax.set_title(f"Resultados: {os.path.basename(image_name)}", fontsize=12, pad=12)
    table = ax.table(cellText=rows, colLabels=headers, loc='center', cellLoc='center')
    table.auto_set_font_size(False)
    table.set_fontsize(10)
    table.scale(1, 1.2)
    total_time_s = total_time / 1000.0
    total_time_min = total_time_s / 60.0
    ax.text(0.5, -0.1,
            f"Tempo total: {total_time:.2f} ms | {total_time_s:.2f} s | {total_time_min:.2f} min",
            ha='center', va='center', fontsize=10, transform=ax.transAxes)
    plt.tight_layout()
    outfile = os.path.join(directory, 'results_table.png')
    fig.savefig(outfile, bbox_inches='tight', dpi=200)
    plt.close(fig)


def print_results(results, total_time, image_name, output_dir=None, plot_dir=None):
    lines = []
    lines.append(f"--- FINAL RESULTS: {image_name} ---")
    lines.append("┌─────────┬───────────┬─────────┬──────────────────────────────┬────────────────────┐")
    lines.append("│ k Factor│ PSNR (dB) │  SSIM   │ Bitrate (bits/pixel)       │ Time (ms | s | min)│")
    lines.append("├─────────┼─────────┼─────────┼──────────────────────────────┼────────────────────┤")
    for k, psnr, ssim, time_ms, bitrate in results:
        time_s = time_ms / 1000.0
        time_min = time_s / 60.0
        lines.append(f"│ {int(k):>7d}│ {psnr:9.2f} │ {ssim:7.4f} │ {bitrate:18.6f} │ {time_ms:8.2f} | {time_s:6.2f} | {time_min:6.2f} │")
    lines.append("└─────────┴───────────┴─────────┴──────────────────────────────┴────────────────────┘")
    total_time_s = total_time / 1000.0
    total_time_min = total_time_s / 60.0
    lines.append(f"\nTotal processing time: {total_time:.2f} ms | {total_time_s:.2f} s | {total_time_min:.2f} min")
    output_str = "\n".join(lines)
    print(output_str)
    target_dir = None
    if plot_dir:
        target_dir = os.path.join(plot_dir, os.path.basename(image_name).split('.')[0])
        os.makedirs(target_dir, exist_ok=True)
        with open(os.path.join(target_dir, 'results.txt'), 'w', encoding='utf-8') as f:
            f.write(output_str)
    elif output_dir:
        target_dir = os.path.join(output_dir, os.path.basename(image_name).split('.')[0])
        os.makedirs(target_dir, exist_ok=True)
        with open(os.path.join(target_dir, 'results.txt'), 'w', encoding='utf-8') as f:
            f.write(output_str)
    results_table(results, total_time, image_name, target_dir)


def plot_psnr(results, image_name, out_dir='plots'):
    ks = [r[0] for r in results]
    psnrs = [r[1] for r in results]
    name = os.path.basename(image_name).split('.')[0]
    sub = os.path.join(out_dir, name)
    os.makedirs(sub, exist_ok=True)
    plt.figure(figsize=(8, 5))
    plt.plot(ks, psnrs, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('PSNR (dB)')
    plt.title('PSNR vs k: ' + name)
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(sub, name + '_psnr.png'))
    plt.close()


def plot_ssim(results, image_name, out_dir='plots'):
    ks = [r[0] for r in results]
    ssims = [r[2] for r in results]
    name = os.path.basename(image_name).split('.')[0]
    sub = os.path.join(out_dir, name)
    os.makedirs(sub, exist_ok=True)
    plt.figure(figsize=(8, 5))
    plt.plot(ks, ssims, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('SSIM')
    plt.title('SSIM vs k: ' + name)
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(sub, name + '_ssim.png'))
    plt.close()


def plot_bitrate(bitrate_list, image_name, out_dir='plots'):
    ks = [b[0] for b in bitrate_list]
    bpp_amp = [b[1]['bpp_amplitude'] for b in bitrate_list]
    name = os.path.basename(image_name).split('.')[0]
    sub = os.path.join(out_dir, name)
    os.makedirs(sub, exist_ok=True)
    plt.figure(figsize=(8, 5))
    plt.plot(ks, bpp_amp, marker='o', label='Bitrate', linewidth=2)
    plt.xlabel('k factor', fontsize=11)
    plt.ylabel('Bits per pixel', fontsize=11)
    plt.title('Bitrate vs k: ' + name, fontsize=12)
    plt.yticks(range(1, 9))
    plt.ylim(0, 8.5)
    plt.legend()
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(sub, name + '_bitrate.png'), dpi=200, bbox_inches='tight')
    plt.close()


def plot_dataset(results_global, bitrate_global, out_dir='plots'):
    if not results_global:
        return
    flat = [item for sub in results_global for item in sub]
    import pandas as pd
    df = pd.DataFrame(flat, columns=['k', 'PSNR', 'SSIM', 'Time', 'Image'])
    analysis = os.path.join(out_dir, '_dataset_analysis')
    os.makedirs(analysis, exist_ok=True)
    unique_k = sorted(df['k'].unique())
    mean_psnr = [df[df['k'] == k]['PSNR'].mean() for k in unique_k]
    mean_ssim = [df[df['k'] == k]['SSIM'].mean() for k in unique_k]

    plt.figure(figsize=(8, 5))
    plt.plot(unique_k, mean_psnr, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('Mean PSNR (dB)')
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(analysis, 'dataset_mean_psnr.png'))
    plt.close()

    plt.figure(figsize=(8, 5))
    plt.plot(unique_k, mean_ssim, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('Mean SSIM')
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(analysis, 'dataset_mean_ssim.png'))
    plt.close()

    if bitrate_global:
        flat_bitrate = [item for sub in bitrate_global for item in sub]
        dfb = pd.DataFrame(flat_bitrate, columns=['k', 'stats', 'Image'])
        unique_k_b = sorted(dfb['k'].unique())
        mean_bpp_amp = [
            dfb[dfb['k'] == k]['stats'].apply(lambda x: x['bpp_amplitude']).mean()
            for k in unique_k_b
        ]
        plt.figure(figsize=(8, 5))
        plt.plot(unique_k_b, mean_bpp_amp, marker='o', label='Bitrate', linewidth=2)
        plt.xlabel('k factor', fontsize=11)
        plt.ylabel('Bits per pixel', fontsize=11)
        plt.title('Dataset Mean Bitrate', fontsize=12)
        plt.yticks(range(1, 9))
        plt.ylim(0, 8.5)
        plt.legend()
        plt.grid(True, linestyle='--', alpha=0.6)
        plt.savefig(os.path.join(analysis, 'dataset_mean_bitrate.png'),
                    dpi=200, bbox_inches='tight')
        plt.close()
