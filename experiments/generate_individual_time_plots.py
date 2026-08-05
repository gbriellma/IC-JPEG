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

LABELS = {
    'loeffler': 'Loeffler (1989)',
    'matrix': 'Matrix (ref.)',
    'rdct': 'RDCT (Cintra-Bayer)',
    'silveira_j3': 'Silveira j=3',
    'silveira_j7': 'Silveira j=7',
}

COLORS = {
    'loeffler': '#2c7fb8',
    'matrix': '#d95f0e',
    'rdct': '#238b45',
    'silveira_j3': '#9467bd',
    'silveira_j7': '#d62728',
}


def ordered_methods(methods):
    seen = set(methods)
    return [m for m in PREFERRED_METHODS if m in seen] + [m for m in methods if m not in PREFERRED_METHODS]


def _read_rows(path):
    with open(path, 'r', encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f))


def load_time_summary(path):
    rows = _read_rows(path)
    if not rows:
        return {}, [], []

    fields = set(rows[0].keys())
    summary = {}

    if 'compress_ms' in fields and 'decompress_ms' in fields:
        for row in rows:
            method = row['method']
            k = float(row['k'])
            summary[(method, k)] = {
                'compress_ms': float(row['compress_ms']),
                'decompress_ms': float(row['decompress_ms']),
            }
    elif 'comp_ms' in fields and 'decomp_ms' in fields:
        grouped = defaultdict(list)
        for row in rows:
            grouped[(row['method'], float(row['k']))].append(row)
        for key, vals in grouped.items():
            summary[key] = {
                'compress_ms': float(np.mean([float(v['comp_ms']) for v in vals])),
                'decompress_ms': float(np.mean([float(v['decomp_ms']) for v in vals])),
            }
    else:
        raise RuntimeError(f'CSV sem colunas de tempo conhecidas: {path}')

    methods = ordered_methods(sorted({method for method, _ in summary}))
    k_factors = sorted({k for _, k in summary})
    return summary, methods, k_factors


def plot_single_bar_chart(summary, k_factors, methods, metric_key, out_path, title):
    k_labels = [f'k={k}' for k in k_factors]
    y = np.arange(len(k_labels))
    height = min(0.8 / max(len(methods), 1), 0.25)

    fig, ax = plt.subplots(figsize=(8, 5))
    for idx, method in enumerate(methods):
        times = [summary[(method, k)][metric_key] for k in k_factors]
        offset = (idx - (len(methods) - 1) / 2.0) * height
        ax.barh(y + offset, times, height, label=LABELS.get(method, method), color=COLORS.get(method, '#444444'))

    ax.set_xlabel('ms')
    ax.set_ylabel('quality_factor (k)')
    ax.set_title(title)
    ax.set_yticks(y)
    ax.set_yticklabels(k_labels)
    ax.invert_yaxis()
    ax.grid(True, axis='x', linestyle='--', alpha=0.4)
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc='lower center', bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)

    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(out_path, dpi=180)
    print(f'Plot salvo em {out_path}')
    plt.close(fig)


def first_existing(paths):
    for path in paths:
        if os.path.exists(path):
            return path
    return None


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))

    py_path = first_existing([
        os.path.join(script_dir, 'results/comparison/summary_table_python.csv'),
        os.path.join(script_dir, 'results/comparison/metrics_python.csv'),
        os.path.join(script_dir, 'results/comparison/metrics.csv'),
    ])
    c_path = first_existing([
        os.path.join(script_dir, 'resultados/summary_table_c.csv'),
        os.path.join(script_dir, 'resultados/summary_table.csv'),
        os.path.join(script_dir, 'resultados/metrics_c.csv'),
        os.path.join(script_dir, 'resultados/metrics.csv'),
    ])

    if py_path:
        py_summary, py_methods, py_k = load_time_summary(py_path)
        plot_single_bar_chart(
            py_summary, py_k, py_methods, 'compress_ms',
            os.path.join(script_dir, 'results/comparison/compress_time_vs_k_bar.png'),
            'Python - Tempo medio de compressao',
        )
        plot_single_bar_chart(
            py_summary, py_k, py_methods, 'decompress_ms',
            os.path.join(script_dir, 'results/comparison/decompress_time_vs_k_bar.png'),
            'Python - Tempo medio de descompressao',
        )

    if c_path:
        c_summary, c_methods, c_k = load_time_summary(c_path)
        plot_single_bar_chart(
            c_summary, c_k, c_methods, 'compress_ms',
            os.path.join(script_dir, 'resultados/compress_time_vs_k_bar.png'),
            'C (int32) - Tempo medio de compressao',
        )
        plot_single_bar_chart(
            c_summary, c_k, c_methods, 'decompress_ms',
            os.path.join(script_dir, 'resultados/decompress_time_vs_k_bar.png'),
            'C (int32) - Tempo medio de descompressao',
        )


if __name__ == '__main__':
    main()
