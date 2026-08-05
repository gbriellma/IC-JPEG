import os
import csv
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


PREFERRED_METHODS = [
    'loeffler', 'matrix', 'rdct', 'silveira_j3', 'silveira_j7',
]

MARKERS = {
    'loeffler': 'o',
    'matrix': 's',
    'rdct': '^',
    'silveira_j3': 'D',
    'silveira_j7': 'P',
}

COLORS = {
    'loeffler': '#2c7fb8',
    'matrix': '#d95f0e',
    'rdct': '#238b45',
    'silveira_j3': '#9467bd',
    'silveira_j7': '#d62728',
}

LABELS = {
    'loeffler': 'Loeffler (1989)',
    'matrix': 'Matrix (ref.)',
    'rdct': 'RDCT (Cintra-Bayer)',
    'silveira_j3': 'Silveira j=3',
    'silveira_j7': 'Silveira j=7',
}


def ordered_methods(methods):
    seen = set(methods)
    return [m for m in PREFERRED_METHODS if m in seen] + [m for m in methods if m not in PREFERRED_METHODS]


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    csv_candidates = [
        os.path.join(script_dir, 'results/comparison/metrics_python.csv'),
        os.path.join(script_dir, 'results/comparison/metrics.csv'),
    ]
    csv_file = next((p for p in csv_candidates if os.path.exists(p)), None)
    if csv_file is None:
        print('Nenhum CSV de metricas Python encontrado em results/comparison; pulando plots Python.')
        return

    output_dir = os.path.join(script_dir, 'results/comparison')

    records = []
    with open(csv_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            records.append({
                'method': row['method'],
                'k': float(row['k']),
                'psnr': float(row['psnr']),
                'comp_ms': float(row['comp_ms']),
                'decomp_ms': float(row['decomp_ms']),
            })

    sums = defaultdict(list)
    for r in records:
        sums[(r['method'], r['k'])].append(r)

    methods = ordered_methods(sorted({r['method'] for r in records}))
    k_factors = sorted({r['k'] for r in records})

    def get_avg(method, k, key):
        return float(np.mean([x[key] for x in sums[(method, k)]]))

    fig, ax = plt.subplots(figsize=(8, 5))
    for m in methods:
        x = [get_avg(m, k, 'comp_ms') for k in k_factors]
        y = [get_avg(m, k, 'psnr') for k in k_factors]
        ax.plot(x, y, marker=MARKERS.get(m, 'o'), color=COLORS.get(m, '#444444'),
                label=LABELS.get(m, m), linewidth=2)
        for i, k in enumerate(k_factors):
            ax.annotate(f'k={k}', (x[i], y[i]), xytext=(5, 3), textcoords='offset points', fontsize=8)
    ax.set_xlabel('Tempo Medio de Compressao (ms)', fontsize=11)
    ax.set_ylabel('PSNR Medio (dB)', fontsize=11)
    ax.set_title('PSNR vs Tempo de Compressao', fontsize=12)
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc='lower center', bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    ax.grid(True, linestyle='--', alpha=0.5)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(output_dir, 'psnr_vs_comp_time.png'), dpi=200)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 5))
    for m in methods:
        x = [get_avg(m, k, 'decomp_ms') for k in k_factors]
        y = [get_avg(m, k, 'psnr') for k in k_factors]
        ax.plot(x, y, marker=MARKERS.get(m, 'o'), color=COLORS.get(m, '#444444'),
                label=LABELS.get(m, m), linewidth=2)
        for i, k in enumerate(k_factors):
            ax.annotate(f'k={k}', (x[i], y[i]), xytext=(5, 3), textcoords='offset points', fontsize=8)
    ax.set_xlabel('Tempo Medio de Descompressao (ms)', fontsize=11)
    ax.set_ylabel('PSNR Medio (dB)', fontsize=11)
    ax.set_title('PSNR vs Tempo de Descompressao', fontsize=12)
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc='lower center', bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    ax.grid(True, linestyle='--', alpha=0.5)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(os.path.join(output_dir, 'psnr_vs_decomp_time.png'), dpi=200)
    plt.close(fig)

    print(f'Graficos gerados em: {output_dir}')
    print(' - psnr_vs_comp_time.png')
    print(' - psnr_vs_decomp_time.png')


if __name__ == '__main__':
    main()
