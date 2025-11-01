import os
import sys
import time
import numpy as np
from PIL import Image
import matplotlib.pyplot as plt

sys.path.insert(0, 'src')

from dct import (dct_loeffler_1d, idct_loeffler_1d,
                 dct_matrix_1d, idct_matrix_1d,
                 dct_approximate_1d, idct_approximate_1d,
                 dct_2d, idct_2d)
from pipeline import process_channel, rgb_to_ycbcr, ycbcr_to_rgb, calc_bitrate_amplitude
from constantes import Q50_LUMA, Q50_CHROMA
from plots import quality_metrics

INPUT_DIR = 'src/imgs'
OUTPUT_DIR = 'comparison_results'
K_FACTORS = [2.0, 4.0, 6.0, 8.0]
#K_FACTORS = [1.0]

METHODS = {
    'Loeffler': (dct_loeffler_1d, idct_loeffler_1d),
    'Matrix': (dct_matrix_1d, idct_matrix_1d),
    'Approximate': (dct_approximate_1d, idct_approximate_1d)
}

def process_image_with_method(img_path, method_name, dct_func, idct_func):
    """Process a single image with a specific DCT method"""
    img = Image.open(img_path).convert('RGB')
    arr = np.array(img).astype(np.uint8)
    r, g, b = arr[:,:,0], arr[:,:,1], arr[:,:,2]
    y, cb, cr = rgb_to_ycbcr(r, g, b)
    
    results = []
    for k in K_FACTORS:
        t0 = time.perf_counter()
        
        y_rec, quant_y = process_channel(y, Q50_LUMA, k, dct_func, idct_func)
        cb_rec, quant_cb = process_channel(cb, Q50_CHROMA, k, dct_func, idct_func)
        cr_rec, quant_cr = process_channel(cr, Q50_CHROMA, k, dct_func, idct_func)
        recon = ycbcr_to_rgb(y_rec, cb_rec, cr_rec)
        
        t1 = time.perf_counter()
        psnr_val, ssim_val = quality_metrics(arr, recon)
        time_ms = (t1 - t0) * 1000.0
        
        # Calculate bitrate for all 3 channels (média)
        from plots import compute_bitrate
        bitrate_y = compute_bitrate(quant_y)
        bitrate_cb = compute_bitrate(quant_cb)
        bitrate_cr = compute_bitrate(quant_cr)
        bitrate = (bitrate_y['bpp_amplitude'] + bitrate_cb['bpp_amplitude'] + bitrate_cr['bpp_amplitude']) / 3.0
        
        results.append({
            'k': k,
            'psnr': psnr_val,
            'ssim': ssim_val,
            'time_ms': time_ms,
            'bitrate': bitrate,
            'method': method_name
        })
    
    return results

def compare_all_methods():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    files = [f for f in os.listdir(INPUT_DIR) 
             if f.lower().endswith(('.png', '.jpg', '.jpeg', '.bmp'))]
    
    if not files:
        print(f"No images found in {INPUT_DIR}")
        return
    
    all_results = []
    
    print("="*70)
    print("DCT METHODS COMPARISON ANALYSIS")
    print("="*70)
    
    for img_file in files:
        img_path = os.path.join(INPUT_DIR, img_file)
        print(f"\nProcessing: {img_file}")
        print("-"*70)
        
        for method_name, (dct_func, idct_func) in METHODS.items():
            print(f"  Running {method_name} method...", end=' ')
            results = process_image_with_method(img_path, method_name, dct_func, idct_func)
            all_results.extend(results)
            avg_time = np.mean([r['time_ms'] for r in results])
            print(f"Done (avg time: {avg_time:.2f} ms)")
    
    generate_comparison_plots(all_results)
    generate_bitrate_plots(all_results)
    
    print_summary_table(all_results)

def generate_comparison_plots(results):

    
    # Organize data by method and k-factor
    methods = list(METHODS.keys())
    k_vals = sorted(list(set([r['k'] for r in results])))
    
    # Average metrics across all images for each method and k
    avg_psnr = {m: [] for m in methods}
    avg_ssim = {m: [] for m in methods}
    avg_time = {m: [] for m in methods}
    
    for k in k_vals:
        for method in methods:
            method_k_results = [r for r in results if r['method'] == method and r['k'] == k]
            avg_psnr[method].append(np.mean([r['psnr'] for r in method_k_results]))
            avg_ssim[method].append(np.mean([r['ssim'] for r in method_k_results]))
            avg_time[method].append(np.mean([r['time_ms'] for r in method_k_results]))
    
    # Create comparison plots
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # PSNR comparison
    ax = axes[0, 0]
    for method in methods:
        ax.plot(k_vals, avg_psnr[method], marker='o', label=method, linewidth=2)
    ax.set_xlabel('k-factor', fontsize=11)
    ax.set_ylabel('PSNR (dB)', fontsize=11)
    ax.set_title('PSNR Comparison Across Methods', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # SSIM comparison
    ax = axes[0, 1]
    for method in methods:
        ax.plot(k_vals, avg_ssim[method], marker='s', label=method, linewidth=2)
    ax.set_xlabel('k-factor', fontsize=11)
    ax.set_ylabel('SSIM', fontsize=11)
    ax.set_title('SSIM Comparison Across Methods', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # Processing time comparison
    ax = axes[1, 0]
    for method in methods:
        ax.plot(k_vals, avg_time[method], marker='^', label=method, linewidth=2)
    ax.set_xlabel('k-factor', fontsize=11)
    ax.set_ylabel('Time (ms)', fontsize=11)
    ax.set_title('Processing Time Comparison', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # PSNR vs Time trade-off
    ax = axes[1, 1]
    for method in methods:
        ax.scatter(avg_time[method], avg_psnr[method], s=100, label=method, alpha=0.7)
        # Connect points with lines
        ax.plot(avg_time[method], avg_psnr[method], alpha=0.3)
    ax.set_xlabel('Processing Time (ms)', fontsize=11)
    ax.set_ylabel('PSNR (dB)', fontsize=11)
    ax.set_title('Quality vs Speed Trade-off', fontsize=12, fontweight='bold')
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, 'methods_comparison.png'), dpi=200, bbox_inches='tight')
    print(f"\n\u2713 Comparison plot saved to {OUTPUT_DIR}/methods_comparison.png")
    plt.close()

def generate_bitrate_plots(results):
    """Generate comparative plots using bitrate instead of k-factor"""
    
    # Organize data by method and bitrate
    methods = list(METHODS.keys())
    
    # Average metrics across all images for each method and bitrate
    avg_psnr = {m: [] for m in methods}
    avg_ssim = {m: [] for m in methods}
    avg_time = {m: [] for m in methods}
    avg_bitrate = {m: [] for m in methods}
    
    # Group by k-factor first, then average bitrates
    k_vals = sorted(list(set([r['k'] for r in results])))
    
    for k in k_vals:
        for method in methods:
            method_k_results = [r for r in results if r['method'] == method and r['k'] == k]
            avg_psnr[method].append(np.mean([r['psnr'] for r in method_k_results]))
            avg_ssim[method].append(np.mean([r['ssim'] for r in method_k_results]))
            avg_time[method].append(np.mean([r['time_ms'] for r in method_k_results]))
            avg_bitrate[method].append(np.mean([r['bitrate'] for r in method_k_results]))
    
    # Create comparison plots with bitrate on x-axis
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # PSNR vs Bitrate
    ax = axes[0, 0]
    for method in methods:
        ax.plot(avg_bitrate[method], avg_psnr[method], marker='o', label=method, linewidth=2)
    ax.set_xlabel('Bitrate (bits per pixel)', fontsize=11)
    ax.set_ylabel('PSNR (dB)', fontsize=11)
    ax.set_title('PSNR vs Bitrate', fontsize=12, fontweight='bold')
    ax.set_xticks(range(1, 9))
    ax.set_xlim(0, 8.5)
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # SSIM vs Bitrate
    ax = axes[0, 1]
    for method in methods:
        ax.plot(avg_bitrate[method], avg_ssim[method], marker='s', label=method, linewidth=2)
    ax.set_xlabel('Bitrate (bits per pixel)', fontsize=11)
    ax.set_ylabel('SSIM', fontsize=11)
    ax.set_title('SSIM vs Bitrate', fontsize=12, fontweight='bold')
    ax.set_xticks(range(1, 9))
    ax.set_xlim(0, 8.5)
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # Bitrate vs k-factor
    ax = axes[1, 0]
    for method in methods:
        ax.plot(k_vals, avg_bitrate[method], marker='^', label=method, linewidth=2)
    ax.set_xlabel('k-factor', fontsize=11)
    ax.set_ylabel('Bitrate (bits per pixel)', fontsize=11)
    ax.set_title('Bitrate vs k-factor', fontsize=12, fontweight='bold')
    ax.set_yticks(range(0, 9))
    ax.set_ylim(0, 8.5)
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    # Processing Time vs Bitrate
    ax = axes[1, 1]
    for method in methods:
        ax.scatter(avg_bitrate[method], avg_time[method], s=100, label=method, alpha=0.7)
        ax.plot(avg_bitrate[method], avg_time[method], alpha=0.3)
    ax.set_xlabel('Bitrate (bits per pixel)', fontsize=11)
    ax.set_ylabel('Processing Time (ms)', fontsize=11)
    ax.set_title('Processing Time vs Bitrate', fontsize=12, fontweight='bold')
    ax.set_xticks(range(1, 9))
    ax.set_xlim(0, 8.5)
    ax.legend()
    ax.grid(True, linestyle='--', alpha=0.6)
    
    plt.tight_layout()
    plt.savefig(os.path.join(OUTPUT_DIR, 'methods_comparison_bitrate.png'), dpi=200, bbox_inches='tight')
    print(f"\u2713 Bitrate comparison plot saved to {OUTPUT_DIR}/methods_comparison_bitrate.png")
    plt.close()

def print_summary_table(results):
    """Print summary comparison table"""
    print("\n" + "="*70)
    print("SUMMARY TABLE - Average Metrics Across All Images")
    print("="*70)
    
    methods = list(METHODS.keys())
    k_vals = sorted(list(set([r['k'] for r in results])))
    
    for k in k_vals:
        print(f"\n\u250c\u2500\u2500\u2500 k-factor = {k} " + "\u2500"*58)
        print("\u2502 Method        \u2502  PSNR (dB)  \u2502   SSIM   \u2502  Time (ms)  \u2502 Bitrate (bpp) \u2502")
        print("\u251c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2524")
        
        for method in methods:
            method_k_results = [r for r in results if r['method'] == method and r['k'] == k]
            if method_k_results:
                avg_psnr = np.mean([r['psnr'] for r in method_k_results])
                avg_ssim = np.mean([r['ssim'] for r in method_k_results])
                avg_time = np.mean([r['time_ms'] for r in method_k_results])
                avg_bitrate = np.mean([r['bitrate'] for r in method_k_results])
                print(f"\u2502 {method:13s} \u2502  {avg_psnr:9.2f}  \u2502 {avg_ssim:8.4f} \u2502  {avg_time:9.2f}  \u2502    {avg_bitrate:9.3f}  \u2502")
        
        print("\u2514" + "\u2500"*15 + "\u2534" + "\u2500"*13 + "\u2534" + "\u2500"*10 + "\u2534" + "\u2500"*13 + "\u2534" + "\u2500"*15 + "\u2518")
    
    # Overall method comparison
    print("\n" + "="*70)
    print("OVERALL METHOD CHARACTERISTICS")
    print("="*70)
    
    for method in methods:
        method_results = [r for r in results if r['method'] == method]
        avg_psnr = np.mean([r['psnr'] for r in method_results])
        avg_ssim = np.mean([r['ssim'] for r in method_results])
        avg_time = np.mean([r['time_ms'] for r in method_results])
        
        print(f"\n{method} Method:")
        print(f"  Average PSNR: {avg_psnr:.2f} dB")
        print(f"  Average SSIM: {avg_ssim:.4f}")
        print(f"  Average Time: {avg_time:.2f} ms")
    
    print("\n" + "="*70)

if __name__ == '__main__':
    compare_all_methods()