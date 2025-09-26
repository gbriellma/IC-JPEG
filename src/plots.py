import os
import numpy as np
import matplotlib.pyplot as plt
from skimage.metrics import peak_signal_noise_ratio as compute_psnr
from skimage.metrics import structural_similarity as compute_ssim

def quality_metrics(original, reconstructed):
    psnr = compute_psnr(original, reconstructed, data_range=255)
    ssim = compute_ssim(original.astype(np.uint8), reconstructed.astype(np.uint8), channel_axis=-1, data_range=255, win_size=7)
    return psnr, ssim

def compute_bitrate(quantized_blocks):
    zigzag_indices = [
        (0,0),(0,1),(1,0),(2,0),(1,1),(0,2),(0,3),(1,2),
        (2,1),(3,0),(4,0),(3,1),(2,2),(1,3),(0,4),(0,5),
        (1,4),(2,3),(3,2),(4,1),(5,0),(6,0),(5,1),(4,2),
        (3,3),(2,4),(1,5),(0,6),(0,7),(1,6),(2,5),(3,4),
        (4,3),(5,2),(6,1),(7,0),(7,1),(6,2),(5,3),(4,4),
        (3,5),(2,6),(1,7),(2,7),(3,6),(4,5),(5,4),(6,3),
        (7,2),(7,3),(6,4),(5,5),(4,6),(3,7),(4,7),(5,6),
        (6,5),(7,4),(7,5),(6,6),(5,7),(6,7),(7,6),(7,7)
    ]
    def zigzag(block):
        return np.array([block[i,j] for i,j in zigzag_indices])
    blocks = quantized_blocks.reshape(-1,8,8)
    total_last_count = 0
    total_bits_amp = 0
    last_indices = []
    nonzero_counts = []
    for block in blocks:
        vec = zigzag(block)
        nz = np.nonzero(vec)[0]
        if nz.size == 0:
            last_indices.append(-1)
            nonzero_counts.append(0)
        else:
            last_indices.append(int(nz[-1]))
            nonzero_counts.append(int(nz.size))
            total_last_count += int(nz[-1]) + 1
        for v in vec:
            if v != 0:
                total_bits_amp += abs(int(v)).bit_length() + 1
    mean_last = float(np.mean([li if li>=0 else 0 for li in last_indices])) if last_indices else 0.0
    mean_nonzero = float(np.mean(nonzero_counts)) if nonzero_counts else 0.0
    bpp_zigzag = ((mean_last + 1) * 8) / 64.0
    bpp_amplitude = (total_bits_amp / len(blocks)) / 64.0 if len(blocks)>0 else 0.0
    return {'mean_last': mean_last, 'mean_nonzero': mean_nonzero, 'bpp_zigzag': bpp_zigzag, 'bpp_amplitude': bpp_amplitude, 'total_last_count': total_last_count}

def results_table(results, total_time, image_name, directory):
    if not directory:
        return
    os.makedirs(directory, exist_ok=True)
    headers = ["k Factor", "PSNR (dB)", "SSIM", "Bitrate (bpp)", "Tempo (ms)", "Tempo (s)", "Tempo (min)"]
    rows = []
    for k, psnr, ssim, time_ms, bitrate in results:
        time_s = time_ms / 1000.0
        time_min = time_s / 60.0
        rows.append([
            f"{int(k)}",
            f"{psnr:.2f}",
            f"{ssim:.4f}",
            f"{bitrate:.6f}",
            f"{time_ms:.2f}",
            f"{time_s:.2f}",
            f"{time_min:.2f}"
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
    ax.text(0.5, -0.1, f"Tempo total: {total_time:.2f} ms | {total_time_s:.2f} s | {total_time_min:.2f} min", ha='center', va='center', fontsize=10, transform=ax.transAxes)
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
    plt.figure(figsize=(8,5))
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
    plt.figure(figsize=(8,5))
    plt.plot(ks, ssims, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('SSIM')
    plt.title('SSIM vs k: ' + name)
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(sub, name + '_ssim.png'))
    plt.close()

def plot_bitrate(bitrate_list, image_name, out_dir='plots'):
    ks = [b[0] for b in bitrate_list]
    bpp_z = [b[1]['bpp_zigzag'] for b in bitrate_list]
    bpp_amp = [b[1]['bpp_amplitude'] for b in bitrate_list]
    name = os.path.basename(image_name).split('.')[0]
    sub = os.path.join(out_dir, name)
    os.makedirs(sub, exist_ok=True)
    plt.figure(figsize=(8,5))
    plt.plot(ks, bpp_z, marker='o', label='BPP ZigZag')
    plt.plot(ks, bpp_amp, marker='s', label='BPP Amplitude')
    plt.xlabel('k factor')
    plt.ylabel('Bits per pixel (approx)')
    plt.title('Bitrate vs k: ' + name)
    plt.legend()
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(sub, name + '_bitrate.png'))
    plt.close()

def plot_dataset(results_global, bitrate_global, out_dir='plots'):
    if not results_global:
        return
    flat = [item for sub in results_global for item in sub]
    import pandas as pd
    df = pd.DataFrame(flat, columns=['k','PSNR','SSIM','Time','Image'])
    analysis = os.path.join(out_dir, '_dataset_analysis')
    os.makedirs(analysis, exist_ok=True)
    unique_k = sorted(df['k'].unique())
    mean_psnr = [df[df['k']==k]['PSNR'].mean() for k in unique_k]
    mean_ssim = [df[df['k']==k]['SSIM'].mean() for k in unique_k]
    plt.figure(figsize=(8,5))
    plt.plot(unique_k, mean_psnr, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('Mean PSNR (dB)')
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(analysis, 'dataset_mean_psnr.png'))
    plt.close()
    plt.figure(figsize=(8,5))
    plt.plot(unique_k, mean_ssim, marker='o')
    plt.xlabel('k factor')
    plt.ylabel('Mean SSIM')
    plt.grid(True, linestyle='--', alpha=0.6)
    plt.savefig(os.path.join(analysis, 'dataset_mean_ssim.png'))
    plt.close()
    if bitrate_global:
        flat_bitrate = [item for sub in bitrate_global for item in sub]
        dfb = pd.DataFrame(flat_bitrate, columns=['k','stats','Image'])
        unique_k_b = sorted(dfb['k'].unique())
        mean_bpp_z = [dfb[dfb['k']==k]['stats'].apply(lambda x: x['bpp_zigzag']).mean() for k in unique_k_b]
        mean_bpp_amp = [dfb[dfb['k']==k]['stats'].apply(lambda x: x['bpp_amplitude']).mean() for k in unique_k_b]
        plt.figure(figsize=(8,5))
        plt.plot(unique_k_b, mean_bpp_z, marker='o', label='BPP ZigZag')
        plt.plot(unique_k_b, mean_bpp_amp, marker='s', label='BPP Amplitude')
        plt.xlabel('k factor')
        plt.ylabel('Bits per pixel (approx)')
        plt.legend()
        plt.grid(True, linestyle='--', alpha=0.6)
        plt.savefig(os.path.join(analysis, 'dataset_mean_bitrate.png'))
        plt.close()
