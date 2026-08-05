"""
compare.py - Analise comparativa: Loeffler, Matrix, RDCT e Silveira.

Processa todas as imagens de imgs/ para k = 0.1, 0.2, 0.5, 0.8.
Prova que Loeffler e Matrix produzem coeficientes quantizados identicos.
Gera graficos (PSNR vs k, BPP vs k, curva RD, SSIM vs k, prova de equivalencia)
e salva os resultados em CSV.

Uso:
    cd experiments
    python compare.py
    python compare.py --imgs imgs --out results/comparison
"""

import sys
import os
import csv
import time
import argparse
import shutil
from collections import defaultdict
from decimal import Decimal, ROUND_HALF_UP

import numpy as np
from PIL import Image
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from skimage.metrics import peak_signal_noise_ratio as _psnr_fn
from skimage.metrics import structural_similarity as _ssim_fn

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, 'src_py'))

from constantes import Q50_LUMA, Q50_CHROMA, ZIGZAG_SCAN, CLASS_NORM_1024
from pipeline import (rgb_to_ycbcr, ycbcr_to_rgb,
                      process_channel_compress, process_channel_decompress)
from dct import (dct_loeffler_1d,     idct_loeffler_1d,
                 dct_matrix_1d,       idct_matrix_1d,
                 dct_rdct_1d,         idct_rdct_1d,
                 dct_silveira_j3_1d,  idct_silveira_j3_1d,
                 dct_silveira_j7_1d,  idct_silveira_j7_1d)

# ─── Configuracao ─────────────────────────────────────────────────────────────

K_FACTORS = [0.1, 0.2, 0.5, 0.8]

# (dct_1d, idct_1d, is_approx, norm_1024)
METHODS = {
    'loeffler':    (dct_loeffler_1d,    idct_loeffler_1d,    False, None),
    'matrix':      (dct_matrix_1d,      idct_matrix_1d,      False, None),
    'rdct':        (dct_rdct_1d,        idct_rdct_1d,        True,  None),
    'silveira_j3': (dct_silveira_j3_1d, idct_silveira_j3_1d, False, CLASS_NORM_1024[3]),
    'silveira_j7': (dct_silveira_j7_1d, idct_silveira_j7_1d, False, CLASS_NORM_1024[7]),
}

LABELS = {
    'loeffler':    'Loeffler (1989)',
    'matrix':      'Matrix (referencia)',
    'rdct':        'RDCT (Cintra-Bayer, 2011)',
    'silveira_j3': 'Silveira j=3 (2022)',
    'silveira_j7': 'Silveira j=7 (2022)',
}

COLORS = {
    'loeffler':    '#2c7fb8',
    'matrix':      '#d95f0e',
    'rdct':        '#238b45',
    'silveira_j3': '#9467bd',
    'silveira_j7': '#d62728',
}

MARKERS = {
    'loeffler':    'o',
    'matrix':      's',
    'rdct':        '^',
    'silveira_j3': 'D',
    'silveira_j7': 'P',
}

ROUND_DIGITS = {
    'psnr': Decimal('0.01'),
    'ssim': Decimal('0.01'),
    'bpp': Decimal('0.01'),
}


# ─── BPP via contagem exata de bytes RLE ────────────────────────────────────
# Replica zigzag_rle.c: DC (2B) + cada AC nao-zero (3B) + EOB (3B se last<63)

def _flat64(block):
    if hasattr(block, 'flatten'):
        block = block.flatten()
    return [int(block[i]) for i in range(64)]


def _count_coeff_diffs(a, b):
    diffs = 0
    total = 0

    for i in range(len(a)):
        aa = _flat64(a[i])
        bb = _flat64(b[i])
        for j in range(64):
            if aa[j] != bb[j]:
                diffs += 1
            total += 1

    return diffs, total


def _channel_rle_bytes(blocks):
    """Bytes totais do RLE para todos os blocos de um canal."""
    total = 0
    for block in blocks:
        flat = _flat64(block)
        zz = [flat[int(i)] for i in ZIGZAG_SCAN]
        total += 2                           # DC: sempre 2 bytes
        last_nz = 0
        for i in range(63, 0, -1):
            if zz[i] != 0:
                last_nz = i
                break
        if last_nz == 0:
            total += 3                       # EOB: todos os ACs sao zero
        else:
            total += sum(3 for i in range(1, last_nz + 1) if zz[i] != 0)
            if last_nz < 63:
                total += 3                   # EOB: ainda ha zeros no final
    return total


def compute_rle_bpp(y_q, cb_q, cr_q, width, height):
    """BPP baseado no tamanho real do RLE (3 canais)."""
    total_bytes = (_channel_rle_bytes(y_q) +
                   _channel_rle_bytes(cb_q) +
                   _channel_rle_bytes(cr_q))
    return (total_bytes * 8) / (width * height)


# ─── Processamento de uma imagem / metodo / k ────────────────────────────────

def process_one(arr, k, dct_1d, idct_1d, is_approx, norm_1024):
    """Comprime e descomprime arr (H,W,3 uint8) com um metodo e fator k.

    Retorna: psnr, ssim, bpp, y_q, cb_q, cr_q, comp_ms, decomp_ms
    """
    h, w = arr.shape[:2]
    y, cb, cr = rgb_to_ycbcr(arr[:, :, 0], arr[:, :, 1], arr[:, :, 2])

    t0_comp = time.perf_counter()

    y_q,  _, nb = process_channel_compress(
        y, Q50_LUMA, k, dct_1d, is_approx=is_approx, norm_1024=norm_1024)
    cb_q, _, _ = process_channel_compress(
        cb, Q50_CHROMA, k, dct_1d, is_approx=is_approx, norm_1024=norm_1024)
    cr_q, _, _ = process_channel_compress(
        cr, Q50_CHROMA, k, dct_1d, is_approx=is_approx, norm_1024=norm_1024)

    t1_comp = time.perf_counter()
    t0_decomp = time.perf_counter()

    y_rec = process_channel_decompress(
        y_q, nb, w, h, Q50_LUMA, k, idct_1d,
        is_approx=is_approx, norm_1024=norm_1024)
    cb_rec = process_channel_decompress(
        cb_q, nb, w, h, Q50_CHROMA, k, idct_1d,
        is_approx=is_approx, norm_1024=norm_1024)
    cr_rec = process_channel_decompress(
        cr_q, nb, w, h, Q50_CHROMA, k, idct_1d,
        is_approx=is_approx, norm_1024=norm_1024)

    recon = np.array(ycbcr_to_rgb(y_rec, cb_rec, cr_rec), dtype=np.uint8)

    t1_decomp = time.perf_counter()

    psnr = _psnr_fn(arr, recon, data_range=255)
    ssim = _ssim_fn(arr, recon, channel_axis=-1, data_range=255, win_size=7)
    bpp  = compute_rle_bpp(y_q, cb_q, cr_q, w, h)

    comp_ms = (t1_comp - t0_comp) * 1000.0
    decomp_ms = (t1_decomp - t0_decomp) * 1000.0

    return psnr, ssim, bpp, y_q, cb_q, cr_q, comp_ms, decomp_ms


def _round_half_up(value, key):
    return str(Decimal(str(value)).quantize(ROUND_DIGITS[key], rounding=ROUND_HALF_UP))


def aggregate(records):
    grouped = defaultdict(list)
    for rec in records:
        grouped[(rec['method'], rec['k'])].append(rec)

    summary = {}
    for key, rows in grouped.items():
        summary[key] = {
            'psnr': float(np.mean([r['psnr'] for r in rows])),
            'ssim': float(np.mean([r['ssim'] for r in rows])),
            'bpp': float(np.mean([r['bpp'] for r in rows])),
            'comp_ms': float(np.mean([r['comp_ms'] for r in rows])),
            'decomp_ms': float(np.mean([r['decomp_ms'] for r in rows])),
        }
    return summary


def build_presentation_snapshot(summary):
    """Presentation table: Matrix reference against retained methods."""
    rows = []
    for k in K_FACTORS:
        m = summary[('matrix', k)]
        row = {
            'k': k,
            'matrix_psnr': m['psnr'],
            'matrix_ssim': m['ssim'],
            'matrix_bpp': m['bpp'],
        }
        for method in ['loeffler', 'rdct', 'silveira_j3', 'silveira_j7']:
            x = summary[(method, k)]
            row[f'{method}_psnr'] = x['psnr']
            row[f'{method}_ssim'] = x['ssim']
            row[f'{method}_bpp'] = x['bpp']
        rows.append(row)
    return rows


# ─── Loop principal ──────────────────────────────────────────────────────────

def run_comparison(imgs_dir):
    """Processa todas as imagens, retorna metricas e dados de equivalencia."""
    img_files = sorted(
        f for f in os.listdir(imgs_dir)
        if f.lower().endswith(('.bmp', '.png', '.jpg', '.jpeg'))
    )
    if not img_files:
        print(f"Nenhuma imagem encontrada em: {imgs_dir}")
        sys.exit(1)

    records = []   # uma entrada por (imagem, metodo, k)
    equiv   = []   # uma entrada por (imagem, k) com prova Loeffler == Matrix

    for fname in img_files:
        name = os.path.splitext(fname)[0]
        arr  = np.array(Image.open(os.path.join(imgs_dir, fname)).convert('RGB'),
                        dtype=np.uint8)
        h, w = arr.shape[:2]
        print(f"\n{name}  ({w}x{h})")
        print(f"  {'metodo':<12}  {'k':>3}  {'PSNR':>8}  {'SSIM':>7}  {'bpp':>7}  {'comp_ms':>7}  {'decomp_ms':>9}")
        print(f"  {'-'*66}")

        # guarda coeficientes para prova de equivalencia
        coeff_store = {}

        for method, (dct_fn, idct_fn, is_approx, norm_1024) in METHODS.items():
            for k in K_FACTORS:
                psnr, ssim, bpp, y_q, cb_q, cr_q, comp_ms, decomp_ms = process_one(
                    arr, k, dct_fn, idct_fn, is_approx, norm_1024)

                records.append({
                    'image': name, 'method': method, 'k': k,
                    'psnr': psnr, 'ssim': ssim, 'bpp': bpp, 'comp_ms': comp_ms, 'decomp_ms': decomp_ms,
                })
                coeff_store[(method, k)] = (y_q, cb_q, cr_q)

                print(f"  {method:<12}  {k:>3}  {psnr:>8.3f}  {ssim:>7.4f}"
                      f"  {bpp:>7.3f}  {comp_ms:>7.1f}  {decomp_ms:>9.1f}")

        # prova Loeffler == Matrix
        for k in K_FACTORS:
            y_l, cb_l, cr_l = coeff_store[('loeffler', k)]
            y_m, cb_m, cr_m = coeff_store[('matrix',   k)]

            diffs_y, total_y = _count_coeff_diffs(y_l, y_m)
            diffs_cb, total_cb = _count_coeff_diffs(cb_l, cb_m)
            diffs_cr, total_cr = _count_coeff_diffs(cr_l, cr_m)
            diffs = diffs_y + diffs_cb + diffs_cr
            total = total_y + total_cb + total_cr

            psnr_l = next(r['psnr'] for r in records
                          if r['image'] == name and r['method'] == 'loeffler' and r['k'] == k)
            psnr_m = next(r['psnr'] for r in records
                          if r['image'] == name and r['method'] == 'matrix' and r['k'] == k)

            equiv.append({
                'image': name, 'k': k,
                'coeff_diffs': diffs, 'total_coeffs': total,
                'psnr_loeffler': psnr_l, 'psnr_matrix': psnr_m,
                'delta_psnr': abs(psnr_l - psnr_m),
            })

    return records, equiv


# ─── CSV ─────────────────────────────────────────────────────────────────────

def save_metrics_csv(records, path):
    fields = ['image', 'method', 'k', 'psnr', 'ssim', 'bpp', 'comp_ms', 'decomp_ms']
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in records:
            w.writerow({
                'image': r['image'], 'method': r['method'], 'k': r['k'],
                'psnr':      f"{r['psnr']:.4f}",
                'ssim':      f"{r['ssim']:.4f}",
                'bpp':       f"{r['bpp']:.4f}",
                'comp_ms':   f"{r['comp_ms']:.2f}",
                'decomp_ms': f"{r['decomp_ms']:.2f}",
            })


def save_equiv_csv(equiv, path):
    fields = ['image', 'k', 'coeff_diffs', 'total_coeffs',
              'psnr_loeffler', 'psnr_matrix', 'delta_psnr']
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in equiv:
            w.writerow({
                'image': r['image'], 'k': r['k'],
                'coeff_diffs':   r['coeff_diffs'],
                'total_coeffs':  r['total_coeffs'],
                'psnr_loeffler': f"{r['psnr_loeffler']:.4f}",
                'psnr_matrix':   f"{r['psnr_matrix']:.4f}",
                'delta_psnr':    f"{r['delta_psnr']:.6f}",
            })


def save_summary_csv(summary, path):
    fields = ['method', 'k', 'psnr', 'ssim', 'bpp', 'comp_ms', 'decomp_ms']
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for method in METHODS:
            for k in K_FACTORS:
                row = summary[(method, k)]
                w.writerow({
                    'method': method,
                    'k': k,
                    'psnr': f"{row['psnr']:.8f}",
                    'ssim': f"{row['ssim']:.8f}",
                    'bpp': f"{row['bpp']:.8f}",
                    'comp_ms': f"{row['comp_ms']:.6f}",
                    'decomp_ms': f"{row['decomp_ms']:.6f}",
                })


def save_presentation_csv(snapshot, path):
    fields = ['k', 'matrix_psnr', 'matrix_ssim', 'matrix_bpp']
    for method in ['loeffler', 'rdct', 'silveira_j3', 'silveira_j7']:
        fields.extend([f'{method}_psnr', f'{method}_ssim', f'{method}_bpp'])
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in snapshot:
            w.writerow(row)


def save_note(snapshot, path):
    with open(path, 'w', encoding='utf-8') as f:
        f.write("Snapshot de apresentacao: Matrix como referencia.\n")
        f.write("Metodos comparados: Loeffler, RDCT, Silveira j=3, Silveira j=7.\n\n")
        for row in snapshot:
            f.write(
                f"k={row['k']}: matrix PSNR={row['matrix_psnr']:.4f}, "
                f"silveira_j7 PSNR={row['silveira_j7_psnr']:.4f}\n"
            )


# ─── Plots ────────────────────────────────────────────────────────────────────

def _avg_by_method_k(records, metric):
    """Media da metrica agrupada por (method, k) sobre todas as imagens."""
    from collections import defaultdict
    sums = defaultdict(list)
    for r in records:
        sums[(r['method'], r['k'])].append(r[metric])
    return {key: float(np.mean(vals)) for key, vals in sums.items()}


def _setup_ax(ax, xlabel, ylabel, title):
    ax.set_xlabel(xlabel, fontsize=11)
    ax.set_ylabel(ylabel, fontsize=11)
    ax.set_title(title, fontsize=12)
    ax.grid(True, linestyle='--', alpha=0.5)


def _legend_below(fig, ax, ncol=3, fontsize=8):
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc='lower center',
                   bbox_to_anchor=(0.5, -0.01), ncol=ncol, fontsize=fontsize)


def plot_psnr_vs_k(records, out_dir):
    avg = _avg_by_method_k(records, 'psnr')
    fig, ax = plt.subplots(figsize=(7, 5))
    for m in METHODS:
        ys = [avg[(m, k)] for k in K_FACTORS]
        ax.plot(K_FACTORS, ys, marker=MARKERS[m], color=COLORS[m],
                label=LABELS[m], linewidth=2)
    ax.set_xticks(K_FACTORS)
    _setup_ax(ax, 'Fator k', 'PSNR medio (dB)', 'PSNR vs Fator de Quantizacao k')
    _legend_below(fig, ax, ncol=4)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(out_dir, 'psnr_vs_k.png'), dpi=200)
    plt.close(fig)


def plot_bpp_vs_k(records, out_dir):
    avg = _avg_by_method_k(records, 'bpp')
    fig, ax = plt.subplots(figsize=(7, 5))
    for m in METHODS:
        ys = [avg[(m, k)] for k in K_FACTORS]
        ax.plot(K_FACTORS, ys, marker=MARKERS[m], color=COLORS[m],
                label=LABELS[m], linewidth=2)
    ax.set_xticks(K_FACTORS)
    _setup_ax(ax, 'Fator k', 'Bits por pixel (bpp)', 'Taxa de compressao (bpp) vs Fator k')
    _legend_below(fig, ax, ncol=4)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(out_dir, 'bpp_vs_k.png'), dpi=200)
    plt.close(fig)


def plot_ssim_vs_k(records, out_dir):
    avg = _avg_by_method_k(records, 'ssim')
    fig, ax = plt.subplots(figsize=(7, 5))
    for m in METHODS:
        ys = [avg[(m, k)] for k in K_FACTORS]
        ax.plot(K_FACTORS, ys, marker=MARKERS[m], color=COLORS[m],
                label=LABELS[m], linewidth=2)
    ax.set_xticks(K_FACTORS)
    _setup_ax(ax, 'Fator k', 'SSIM medio', 'SSIM vs Fator de Quantizacao k')
    _legend_below(fig, ax, ncol=4)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(out_dir, 'ssim_vs_k.png'), dpi=200)
    plt.close(fig)


def plot_rd_curve(records, out_dir):
    """Curva taxa-distorcao: PSNR vs BPP."""
    avg_psnr = _avg_by_method_k(records, 'psnr')
    avg_bpp  = _avg_by_method_k(records, 'bpp')
    fig, ax  = plt.subplots(figsize=(7, 5))
    for m in METHODS:
        xs = [avg_bpp[(m, k)]  for k in K_FACTORS]
        ys = [avg_psnr[(m, k)] for k in K_FACTORS]
        ax.plot(xs, ys, marker=MARKERS[m], color=COLORS[m],
                label=LABELS[m], linewidth=2)
        for k, x, y in zip(K_FACTORS, xs, ys):
            ax.annotate(f'k={k}', xy=(x, y),
                        xytext=(5, 3), textcoords='offset points', fontsize=8)
    _setup_ax(ax, 'Bits por pixel (bpp)', 'PSNR (dB)', 'Curva Taxa-Distorcao (RD)')
    _legend_below(fig, ax, ncol=4)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(out_dir, 'rd_curve.png'), dpi=200)
    plt.close(fig)


def plot_time_metric_vs_k(records, metric, filename, title):
    avg = _avg_by_method_k(records, metric)
    labels = [f'k={k}' for k in K_FACTORS]
    y = np.arange(len(labels))
    height = min(0.8 / max(len(METHODS), 1), 0.25)

    fig, ax = plt.subplots(figsize=(9, 5))
    for idx, m in enumerate(METHODS):
        xs = [avg[(m, k)] for k in K_FACTORS]
        offset = (idx - (len(METHODS) - 1) / 2.0) * height
        ax.barh(y + offset, xs, height, label=LABELS[m], color=COLORS[m])

    ax.set_xlabel('Tempo medio (ms)')
    ax.set_ylabel('Fator k')
    ax.set_yticks(y)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_title(title)
    ax.grid(True, axis='x', linestyle='--', alpha=0.4)
    _legend_below(fig, ax, ncol=4)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(filename, dpi=200)
    plt.close(fig)


def plot_time_bar_chart(summary, out_path):
    labels = [f'k={k}' for k in K_FACTORS]
    y = np.arange(len(labels))
    height = min(0.8 / max(len(METHODS), 1), 0.25)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))
    for idx, method in enumerate(METHODS):
        comp_times = [summary[(method, k)]['comp_ms'] for k in K_FACTORS]
        decomp_times = [summary[(method, k)]['decomp_ms'] for k in K_FACTORS]
        offset = (idx - (len(METHODS) - 1) / 2.0) * height
        ax1.barh(y + offset, comp_times, height, label=LABELS[method], color=COLORS[method])
        ax2.barh(y + offset, decomp_times, height, label=LABELS[method], color=COLORS[method])

    for ax, title, xlabel in (
        (ax1, 'Python - Tempo medio de compressao', 'ms'),
        (ax2, 'Python - Tempo medio de descompressao', 'ms'),
    ):
        ax.set_xlabel(xlabel)
        ax.set_ylabel('Fator k')
        ax.set_yticks(y)
        ax.set_yticklabels(labels)
        ax.invert_yaxis()
        ax.set_title(title)
        ax.grid(True, axis='x', linestyle='--', alpha=0.4)

    handles, labels = ax1.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc='lower center',
                   bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    fig.tight_layout(rect=(0, 0.09, 1, 1))
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_matrix_loeffler(snapshot, out_path):
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.5))
    metrics = [
        ('PSNR', 'psnr', 'dB'),
        ('SSIM', 'ssim', ''),
        ('bpp', 'bpp', ''),
    ]

    for ax, (label, key_suffix, unit) in zip(axes, metrics):
        lo_vals = [float(row[f'loeffler_{key_suffix}']) for row in snapshot]
        ma_vals = [float(row[f'matrix_{key_suffix}']) for row in snapshot]
        ax.plot(K_FACTORS, lo_vals, marker='o', color=COLORS['loeffler'], linewidth=2, label='Loeffler')
        ax.plot(K_FACTORS, ma_vals, marker='s', color=COLORS['matrix'], linewidth=2, label='Matrix')
        ax.set_xticks(K_FACTORS)
        ax.set_xlabel('Fator k')
        ax.set_title(label)
        ax.grid(True, linestyle='--', alpha=0.4)
        if unit:
            ax.set_ylabel(unit)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='lower center', bbox_to_anchor=(0.5, -0.02), ncol=2, fontsize=8)
    fig.suptitle('Matrix vs Loeffler - valores de apresentacao (Python)', y=0.98)
    fig.tight_layout(rect=(0, 0.08, 1, 0.94))
    fig.savefig(out_path, dpi=180, bbox_inches='tight')
    plt.close(fig)


def plot_equivalence(equiv, out_dir):
    """Prova visual: Loeffler == Matrix."""
    images = sorted(set(r['image'] for r in equiv))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))

    # Painel esquerdo: delta PSNR por imagem e k
    for img in images:
        rows = sorted((r for r in equiv if r['image'] == img), key=lambda r: r['k'])
        ax1.plot([r['k'] for r in rows], [r['delta_psnr'] for r in rows],
                 marker='o', linewidth=1.5, label=img)
    ax1.set_xlabel('Fator k', fontsize=11)
    ax1.set_ylabel('|PSNR Loeffler - PSNR Matrix| (dB)', fontsize=11)
    ax1.set_title('Delta PSNR: Loeffler vs Matrix\n(deve ser 0 em todas as celulas)',
                  fontsize=11)
    ax1.set_xticks(K_FACTORS)
    ax1.set_ylim(-0.001, max(0.005, max(r['delta_psnr'] for r in equiv) * 1.5))
    ax1.grid(True, linestyle='--', alpha=0.5)

    # Painel direito: mapa de calor de diferencas nos coeficientes
    matrix = np.array([
        [next(r['coeff_diffs'] for r in equiv if r['image'] == img and r['k'] == k)
         for k in K_FACTORS]
        for img in images
    ])
    vmax = max(1, int(matrix.max()))
    im = ax2.imshow(matrix, cmap='RdYlGn_r', vmin=0, vmax=vmax, aspect='auto')
    ax2.set_xticks(range(len(K_FACTORS)))
    ax2.set_xticklabels([f'k={k}' for k in K_FACTORS])
    ax2.set_yticks(range(len(images)))
    ax2.set_yticklabels(images, fontsize=9)
    ax2.set_title('Coeficientes diferentes: Loeffler vs Matrix\n(0 = identicos)',
                  fontsize=11)
    plt.colorbar(im, ax=ax2, label='N de coeficientes diferentes')
    for i in range(len(images)):
        for j in range(len(K_FACTORS)):
            ax2.text(j, i, str(matrix[i, j]),
                     ha='center', va='center', fontsize=11, fontweight='bold',
                     color='white' if matrix[i, j] > vmax / 2 else 'black')

    _legend_below(fig, ax1, ncol=4, fontsize=7)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(out_dir, 'loeffler_eq_matrix.png'), dpi=200)
    plt.close(fig)


# ─── Resumo no terminal ───────────────────────────────────────────────────────

def print_summary(records, equiv):
    # Tabela de medias por metodo e k
    print("\n" + "="*64)
    print("MEDIA DO DATASET - todos os metodos e k-factors")
    print("="*64)
    print(f"  {'metodo':<12}  {'k':>3}  {'PSNR':>8}  {'SSIM':>7}  {'bpp':>7}")
    print(f"  {'-'*50}")
    avg_psnr = _avg_by_method_k(records, 'psnr')
    avg_ssim = _avg_by_method_k(records, 'ssim')
    avg_bpp  = _avg_by_method_k(records, 'bpp')
    for m in METHODS:
        for k in K_FACTORS:
            print(f"  {m:<12}  {k:>3}  {avg_psnr[(m,k)]:>8.3f}"
                  f"  {avg_ssim[(m,k)]:>7.4f}  {avg_bpp[(m,k)]:>7.3f}")

    # Prova de equivalencia
    total_diffs = sum(r['coeff_diffs'] for r in equiv)
    max_delta   = max(r['delta_psnr']  for r in equiv)

    print("\n" + "="*64)
    print("PROVA: Loeffler = Matrix")
    print("="*64)
    print(f"  {'imagem':<28}  {'k':>3}  {'diffs':>7}  {'dPSNR':>10}")
    print(f"  {'-'*57}")
    for r in equiv:
        print(f"  {r['image']:<28}  {r['k']:>3}  {r['coeff_diffs']:>7}"
              f"  {r['delta_psnr']:>10.6f}")
    print(f"  {'-'*57}")
    print(f"  Total de coeficientes diferentes : {total_diffs}")
    print(f"  Delta PSNR maximo                : {max_delta:.6f} dB")
    if total_diffs == 0:
        print("  RESULTADO: Loeffler e Matrix sao MATEMATICAMENTE IDENTICOS.")
    else:
        print(f"  ATENCAO: {total_diffs} coeficientes diferentes.")
    print("="*64)


# ─── Entry point ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description='Comparacao dos metodos DCT: Loeffler, Matrix, RDCT e Silveira.')
    parser.add_argument('--imgs', default='imgs',
                        help='Diretorio com as imagens (padrao: imgs)')
    parser.add_argument('--out',  default='results/comparison',
                        help='Diretorio de saida (padrao: results/comparison)')
    args = parser.parse_args()

    imgs_dir = os.path.join(_HERE, args.imgs)
    out_dir  = os.path.join(_HERE, args.out)
    os.makedirs(out_dir, exist_ok=True)

    print(f"Imagens : {imgs_dir}")
    print(f"Saida   : {out_dir}")
    print(f"Metodos : {list(METHODS.keys())}")
    print(f"k       : {K_FACTORS}")

    records, equiv = run_comparison(imgs_dir)

    summary = aggregate(records)
    snapshot = build_presentation_snapshot(summary)

    metrics_csv = os.path.join(out_dir, 'metrics.csv')
    proof_csv = os.path.join(out_dir, 'loeffler_matrix_proof.csv')
    summary_csv = os.path.join(out_dir, 'summary_table.csv')
    presentation_csv = os.path.join(out_dir, 'matrix_loeffler_presentation.csv')
    note_txt = os.path.join(out_dir, 'matrix_loeffler_note.txt')

    save_metrics_csv(records, metrics_csv)
    save_equiv_csv(equiv, proof_csv)
    save_summary_csv(summary, summary_csv)
    save_presentation_csv(snapshot, presentation_csv)
    save_note(snapshot, note_txt)

    save_metrics_csv(records, os.path.join(out_dir, 'metrics_python.csv'))
    save_equiv_csv(equiv, os.path.join(out_dir, 'loeffler_matrix_proof_python.csv'))
    save_summary_csv(summary, os.path.join(out_dir, 'summary_table_python.csv'))
    save_presentation_csv(snapshot, os.path.join(out_dir, 'matrix_loeffler_presentation_python.csv'))
    save_note(snapshot, os.path.join(out_dir, 'matrix_loeffler_note_python.txt'))

    plot_psnr_vs_k(records, out_dir)
    plot_bpp_vs_k(records, out_dir)
    plot_ssim_vs_k(records, out_dir)
    plot_rd_curve(records, out_dir)
    plot_equivalence(equiv, out_dir)
    plot_time_metric_vs_k(records, 'comp_ms',
                          os.path.join(out_dir, 'compress_time_vs_k.png'),
                          'Tempo medio de compressao vs Fator k')
    plot_time_metric_vs_k(records, 'decomp_ms',
                          os.path.join(out_dir, 'decompress_time_vs_k.png'),
                          'Tempo medio de descompressao vs Fator k')
    plot_time_bar_chart(summary, os.path.join(out_dir, 'time_comparison_bar.png'))
    plot_matrix_loeffler(snapshot, os.path.join(out_dir, 'matrix_vs_loeffler.png'))

    print_summary(records, equiv)

    with open(os.path.join(out_dir, 'all_results_combined.csv'), 'w', newline='', encoding='utf-8') as f:
        f.write("=== METRICS (Por Imagem) ===\n")
        with open(metrics_csv, encoding='utf-8') as src:
            f.write(src.read())
        f.write("\n=== SUMMARY (Tabela Media) ===\n")
        with open(summary_csv, encoding='utf-8') as src:
            f.write(src.read())
        f.write("\n=== MATRIX VS LOEFFLER SNAPSHOT ===\n")
        with open(presentation_csv, encoding='utf-8') as src:
            f.write(src.read())

    print(f"\nArquivos gerados em {out_dir}/")
    print("  metrics.csv / metrics_python.csv")
    print("  summary_table.csv / summary_table_python.csv")
    print("  loeffler_matrix_proof.csv / loeffler_matrix_proof_python.csv")
    print("  matrix_loeffler_presentation.csv")
    print("  matrix_loeffler_note.txt")
    print("  psnr_vs_k.png")
    print("  bpp_vs_k.png")
    print("  ssim_vs_k.png")
    print("  rd_curve.png")
    print("  compress_time_vs_k.png")
    print("  decompress_time_vs_k.png")
    print("  time_comparison_bar.png")
    print("  matrix_vs_loeffler.png")
    print("  loeffler_eq_matrix.png")


if __name__ == '__main__':
    main()
