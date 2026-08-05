"""
compare_c.py - Benchmark do codec C int32.

Gera:
  - métricas brutas por imagem/método/k
  - tabela resumida média do dataset
  - gráficos agregados
  - snapshot de apresentação Matrix vs Loeffler

Fonte oficial de resultados: biblioteca C `libimage.so`.
"""

from __future__ import annotations

import argparse
import csv
import ctypes
import os
import shutil
import statistics
import time
from collections import defaultdict
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

import matplotlib
import numpy as np
from matplotlib.ticker import FormatStrFormatter
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio as compute_psnr
from skimage.metrics import structural_similarity as compute_ssim

matplotlib.use("Agg")
import matplotlib.pyplot as plt

_HERE = Path(__file__).resolve().parent
_LIB_PY = _HERE.parent / "libimage" / "python"
_LIB_SO = _HERE.parent / "libimage" / "bin" / "libimage.so"

import sys

sys.path.insert(0, str(_LIB_PY))
from libimage_wrapper import (
    LibImage,
    JPEG_COLORSPACE_RGB,
    JPEG_SUBSAMP_444,
    _JpegCompressed,
    _JpegImage,
)

K_FACTORS = [0.1, 0.2, 0.5, 0.8]
METHODS = ["loeffler", "matrix", "rdct", "silveira_j3", "silveira_j7"]
LABELS = {
    "loeffler": "Loeffler (1989)",
    "matrix": "Matrix (ref.)",
    "rdct": "RDCT (Cintra-Bayer, 2011)",
    "silveira_j3": "Silveira j=3 (2022)",
    "silveira_j7": "Silveira j=7 (2022)",
}
COLORS = {
    "loeffler": "#2c7fb8",
    "matrix": "#d95f0e",
    "rdct": "#238b45",
    "silveira_j3": "#9467bd",
    "silveira_j7": "#d62728",
}
MARKERS = {
    "loeffler": "o",
    "matrix": "s",
    "rdct": "^",
    "silveira_j3": "D",
    "silveira_j7": "P",
}

ROUND_DIGITS = {
    "psnr": Decimal("0.01"),
    "ssim": Decimal("0.01"),
    "bpp": Decimal("0.01"),
}
PLOT_LABEL_FORMAT = "%.1f"
IMAGE_EXT = ".png"
BENCH_WARMUPS = 3
BENCH_BATCHES = 7
BENCH_TARGET_BATCH_MS = 180.0
BENCH_MIN_ITERS = 3
BENCH_MAX_ITERS = 32


def round_half_up(value: float, digits_key: str) -> str:
    return str(Decimal(str(value)).quantize(ROUND_DIGITS[digits_key], rounding=ROUND_HALF_UP))


def _setup_rle_proto(lib_raw):
    lib_raw.jpeg_rle_encode.restype = ctypes.c_int32
    lib_raw.jpeg_rle_encode.argtypes = [
        ctypes.POINTER(ctypes.c_int32),
        ctypes.c_int32,
        ctypes.POINTER(ctypes.c_uint8),
        ctypes.c_int32,
    ]


def _rle_bytes(lib_raw, quantized_blocks: np.ndarray) -> int:
    nb = quantized_blocks.shape[0]
    flat = np.ascontiguousarray(quantized_blocks.flatten(), dtype=np.int32)
    buf_sz = nb * 191 + 1
    buf = (ctypes.c_uint8 * buf_sz)()
    n = lib_raw.jpeg_rle_encode(
        flat.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
        ctypes.c_int32(nb),
        buf,
        ctypes.c_int32(buf_sz),
    )
    if n < 0:
        raise RuntimeError(f"jpeg_rle_encode failed for {nb} blocks")
    return int(n)


def compute_bpp(lib_raw, comp: dict, width: int, height: int) -> float:
    total = _rle_bytes(lib_raw, comp["y_quantized"])
    if "cb_quantized" in comp and "cr_quantized" in comp:
        total += _rle_bytes(lib_raw, comp["cb_quantized"])
        total += _rle_bytes(lib_raw, comp["cr_quantized"])
    return (total * 8.0) / (width * height)


def estimate_peak_codec_bytes(comp: dict, width: int, height: int,
                              colorspace: int = JPEG_COLORSPACE_RGB,
                              keep_coeffs: bool = False) -> int:
    channels = 1 if colorspace != JPEG_COLORSPACE_RGB else 3
    nb_y = int(comp["num_blocks"])
    nb_c = int(comp.get("num_blocks_chroma", 0))
    coeff_mult = 2 if keep_coeffs else 1
    quant_arrays = (nb_y + 2 * nb_c) * 64 * 4 * coeff_mult
    compressed_bytes = ctypes.sizeof(_JpegCompressed) + quant_arrays
    image_bytes = ctypes.sizeof(_JpegImage) + width * height * channels
    return max(compressed_bytes, image_bytes)


def _compress_once(lib: LibImage, arr: np.ndarray, width: int, height: int,
                   method: str, k: float) -> dict:
    return lib.compress(
        arr, width, height, k, method,
        colorspace=JPEG_COLORSPACE_RGB,
        subsampling=JPEG_SUBSAMP_444,
        skip_quantization=False,
        keep_coeffs=False,
    )


def _decompress_once(lib: LibImage, comp: dict, width: int, height: int,
                     method: str, k: float) -> np.ndarray:
    return lib.decompress(
        comp["y_quantized"],
        comp.get("cb_quantized"),
        comp.get("cr_quantized"),
        width,
        height,
        k,
        method,
        colorspace=JPEG_COLORSPACE_RGB,
        subsampling=JPEG_SUBSAMP_444,
        flags=0,
    )


def _bench_batch_ms(fn, iterations: int) -> float:
    t0 = time.perf_counter()
    for _ in range(iterations):
        fn()
    t1 = time.perf_counter()
    return ((t1 - t0) * 1000.0) / iterations


def _choose_batch_iters(single_ms: float) -> int:
    if single_ms <= 0.0:
        return BENCH_MIN_ITERS
    iters = int(round(BENCH_TARGET_BATCH_MS / single_ms))
    return max(BENCH_MIN_ITERS, min(BENCH_MAX_ITERS, iters))


def timed_roundtrip(lib: LibImage, arr: np.ndarray, width: int, height: int,
                    method: str, k: float) -> tuple[list[float], list[float]]:
    # Nota: tempos "vs k" variavam demais com a medicao anterior porque:
    # 1) cada ponto usava poucas amostras em torno da chamada Python/ctypes
    # 2) k era sempre executado na mesma ordem, favorecendo os ultimos casos
    # Aqui usamos batches maiores e agregamos por mediana de batches.
    def compress_op():
        _compress_once(lib, arr, width, height, method, k)

    reference_comp = _compress_once(lib, arr, width, height, method, k)

    def decompress_op():
        _decompress_once(lib, reference_comp, width, height, method, k)

    for _ in range(BENCH_WARMUPS):
        compress_op()
    for _ in range(BENCH_WARMUPS):
        decompress_op()

    comp_iters = _choose_batch_iters(_bench_batch_ms(compress_op, 1))
    decomp_iters = _choose_batch_iters(_bench_batch_ms(decompress_op, 1))

    comp_times = [_bench_batch_ms(compress_op, comp_iters) for _ in range(BENCH_BATCHES)]
    decomp_times = [_bench_batch_ms(decompress_op, decomp_iters) for _ in range(BENCH_BATCHES)]
    return comp_times, decomp_times


def process_one(lib: LibImage, lib_raw, arr: np.ndarray, method: str, k: float) -> dict:
    height, width = arr.shape[:2]

    comp_times, decomp_times = timed_roundtrip(lib, arr, width, height, method, k)
    comp = _compress_once(lib, arr, width, height, method, k)
    recon = _decompress_once(lib, comp, width, height, method, k)

    return {
        "psnr": float(compute_psnr(arr, recon, data_range=255)),
        "ssim": float(compute_ssim(arr, recon, channel_axis=-1, data_range=255, win_size=7)),
        "bpp": compute_bpp(lib_raw, comp, width, height),
        "compress_ms": statistics.median(comp_times),
        "decompress_ms": statistics.median(decomp_times),
        "memory_bytes": estimate_peak_codec_bytes(comp, width, height, keep_coeffs=False),
        "recon": recon,
        "y_quantized": comp["y_quantized"],
        "cb_quantized": comp.get("cb_quantized"),
        "cr_quantized": comp.get("cr_quantized"),
    }


def save_rgb_image(arr: np.ndarray, path: Path):
    Image.fromarray(np.ascontiguousarray(arr, dtype=np.uint8), mode="RGB").save(path)


def iter_cases_balanced(image_idx: int):
    method_shift = image_idx % len(METHODS)
    ordered_methods = METHODS[method_shift:] + METHODS[:method_shift]
    for method_pos, method in enumerate(ordered_methods):
        k_shift = (image_idx + method_pos) % len(K_FACTORS)
        ordered_k = K_FACTORS[k_shift:] + K_FACTORS[:k_shift]
        for k in ordered_k:
            yield method, k


def render_inspection_grid(image_name: str, original: np.ndarray,
                           image_cases: dict[tuple[str, int], dict],
                           out_path: Path):
    fig, axes = plt.subplots(len(K_FACTORS), len(METHODS) + 1,
                             figsize=(3.4 * (len(METHODS) + 1), 3.2 * len(K_FACTORS)))
    if len(K_FACTORS) == 1:
        axes = np.array([axes])

    for row_idx, k in enumerate(K_FACTORS):
        panels = [("ref", {"recon": original})]
        for method in METHODS:
            panels.append((method, image_cases[(method, k)]))

        for col_idx, (method, data) in enumerate(panels):
            ax = axes[row_idx, col_idx]
            ax.imshow(data["recon"])
            ax.set_xticks([])
            ax.set_yticks([])

            if row_idx == 0:
                if method == "ref":
                    ax.set_title("Referencia", fontsize=10, pad=8)
                else:
                    ax.set_title(LABELS[method], fontsize=10, pad=8)

            if col_idx == 0:
                ax.set_ylabel(f"k={k}", rotation=90, fontsize=10, labelpad=10)

            if method == "ref":
                caption = f"Ref\n{original.shape[1]}x{original.shape[0]}"
            else:
                caption = (
                    f"PSNR {round_half_up(data['psnr'], 'psnr')}\n"
                    f"SSIM {round_half_up(data['ssim'], 'ssim')}\n"
                    f"bpp {round_half_up(data['bpp'], 'bpp')}"
                )
            ax.text(
                0.02, 0.02, caption,
                transform=ax.transAxes,
                fontsize=8,
                color="white",
                va="bottom",
                ha="left",
                bbox={"boxstyle": "round,pad=0.2", "facecolor": "black", "alpha": 0.65, "edgecolor": "none"},
            )

    fig.suptitle(f"{image_name} - Grid de inspecao visual", y=0.995, fontsize=13)
    fig.text(0.5, 0.006, "Referencia = imagem RGB original do dataset.", ha="center", fontsize=8, color="#444444")
    fig.tight_layout(rect=(0, 0.02, 1, 0.98))
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def aggregate(records: list[dict]) -> dict[tuple[str, int], dict[str, float]]:
    grouped = defaultdict(list)
    for rec in records:
        grouped[(rec["method"], rec["k"])].append(rec)

    summary = {}
    for key, rows in grouped.items():
        summary[key] = {
            "psnr": float(np.mean([r["psnr"] for r in rows])),
            "ssim": float(np.mean([r["ssim"] for r in rows])),
            "bpp": float(np.mean([r["bpp"] for r in rows])),
            "compress_ms": float(np.mean([r["compress_ms"] for r in rows])),
            "decompress_ms": float(np.mean([r["decompress_ms"] for r in rows])),
            "memory_bytes": float(np.mean([r["memory_bytes"] for r in rows])),
        }
    return summary


def build_presentation_snapshot(summary: dict[tuple[str, float], dict[str, float]]) -> list[dict]:
    rows = []
    for k in K_FACTORS:
        m = summary[("matrix", k)]
        row = {
            "k": k,
            "matrix_psnr": m["psnr"],
            "matrix_ssim": m["ssim"],
            "matrix_bpp": m["bpp"],
        }
        for method in ["loeffler", "rdct", "silveira_j3", "silveira_j7"]:
            x = summary[(method, k)]
            row[f"{method}_psnr"] = x["psnr"]
            row[f"{method}_ssim"] = x["ssim"]
            row[f"{method}_bpp"] = x["bpp"]
        rows.append(row)
    return rows


def save_metrics_csv(records: list[dict], path: Path):
    fields = [
        "image", "method", "k", "psnr", "ssim", "bpp",
        "compress_ms", "decompress_ms", "memory_bytes",
        "coeff_diffs_vs_matrix", "delta_psnr_vs_matrix",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in records:
            w.writerow({
                "image": r["image"],
                "method": r["method"],
                "k": r["k"],
                "psnr": f"{r['psnr']:.8f}",
                "ssim": f"{r['ssim']:.8f}",
                "bpp": f"{r['bpp']:.8f}",
                "compress_ms": f"{r['compress_ms']:.6f}",
                "decompress_ms": f"{r['decompress_ms']:.6f}",
                "memory_bytes": int(r["memory_bytes"]),
                "coeff_diffs_vs_matrix": r.get("coeff_diffs_vs_matrix", ""),
                "delta_psnr_vs_matrix": f"{r.get('delta_psnr_vs_matrix', 0.0):.8f}",
            })


def save_summary_csv(summary: dict[tuple[str, float], dict[str, float]], path: Path):
    fields = ["method", "k", "psnr", "ssim", "bpp", "compress_ms", "decompress_ms", "memory_bytes"]
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for method in METHODS:
            for k in K_FACTORS:
                row = summary[(method, k)]
                w.writerow({
                    "method": method,
                    "k": k,
                    "psnr": f"{row['psnr']:.8f}",
                    "ssim": f"{row['ssim']:.8f}",
                    "bpp": f"{row['bpp']:.8f}",
                    "compress_ms": f"{row['compress_ms']:.6f}",
                    "decompress_ms": f"{row['decompress_ms']:.6f}",
                    "memory_bytes": int(round(row["memory_bytes"])),
                })


def save_presentation_csv(snapshot: list[dict], path: Path):
    fields = ["k", "matrix_psnr", "matrix_ssim", "matrix_bpp"]
    for method in ["loeffler", "rdct", "silveira_j3", "silveira_j7"]:
        fields.extend([f"{method}_psnr", f"{method}_ssim", f"{method}_bpp"])
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in snapshot:
            w.writerow(row)


def save_note(snapshot: list[dict], path: Path):
    with path.open("w", encoding="utf-8") as f:
        f.write("Snapshot de apresentacao: Matrix como referencia.\n")
        f.write("Metodos comparados: Loeffler, RDCT, Silveira j=3, Silveira j=7.\n\n")
        for row in snapshot:
            f.write(
                f"k={row['k']}: matrix PSNR={row['matrix_psnr']:.4f}, "
                f"silveira_j7 PSNR={row['silveira_j7_psnr']:.4f}\n"
            )


def _plot_metric(summary, metric, ylabel, title, out_path):
    fig, ax = plt.subplots(figsize=(7, 5))
    for method in METHODS:
        ax.plot(
            K_FACTORS,
            [summary[(method, k)][metric] for k in K_FACTORS],
            marker=MARKERS[method],
            color=COLORS[method],
            linewidth=2,
            label=LABELS[method],
        )
    ax.set_xticks(K_FACTORS)
    ax.set_xlabel("quality_factor (k)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.yaxis.set_major_formatter(FormatStrFormatter(PLOT_LABEL_FORMAT))
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_rd_curve(summary, out_path):
    fig, ax = plt.subplots(figsize=(7, 5))
    for method in METHODS:
        xs = [summary[(method, k)]["bpp"] for k in K_FACTORS]
        ys = [summary[(method, k)]["psnr"] for k in K_FACTORS]
        ax.plot(xs, ys, marker=MARKERS[method], color=COLORS[method], linewidth=2, label=LABELS[method])
        for k, x, y in zip(K_FACTORS, xs, ys):
            ax.annotate(f"k={k}", (x, y), textcoords="offset points", xytext=(4, 3), fontsize=8)
    ax.set_xlabel("bpp")
    ax.set_ylabel("PSNR (dB)")
    ax.set_title("Curva taxa-distorcao (int32 C)")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.xaxis.set_major_formatter(FormatStrFormatter(PLOT_LABEL_FORMAT))
    ax.yaxis.set_major_formatter(FormatStrFormatter(PLOT_LABEL_FORMAT))
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_matrix_loeffler(snapshot: list[dict], out_path: Path):
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.5))
    metrics = [
        ("PSNR", "psnr", "dB"),
        ("SSIM", "ssim", ""),
        ("bpp", "bpp", ""),
    ]

    for ax, (label, key_suffix, unit) in zip(axes, metrics):
        lo_vals = [float(row[f"loeffler_{key_suffix}"]) for row in snapshot]
        ma_vals = [float(row[f"matrix_{key_suffix}"]) for row in snapshot]
        ax.plot(K_FACTORS, lo_vals, marker="o", color=COLORS["loeffler"], linewidth=2, label="Loeffler")
        ax.plot(K_FACTORS, ma_vals, marker="s", color=COLORS["matrix"], linewidth=2, label="Matrix")
        ax.set_xticks(K_FACTORS)
        ax.set_xlabel("quality_factor (k)")
        ax.set_title(label)
        ax.grid(True, linestyle="--", alpha=0.4)
        ax.yaxis.set_major_formatter(FormatStrFormatter(PLOT_LABEL_FORMAT))
        if unit:
            ax.set_ylabel(unit)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, -0.02), ncol=2, fontsize=8)
    fig.suptitle("Matrix vs Loeffler - valores de apresentacao (int32)", y=0.98)
    fig.tight_layout(rect=(0, 0.08, 1, 0.94))
    fig.savefig(out_path, dpi=180, bbox_inches="tight")
    plt.close(fig)


def plot_time_bar_chart(summary: dict[tuple[str, int], dict[str, float]], out_path: Path):
    labels = [f"k={k}" for k in K_FACTORS]
    y = np.arange(len(labels))
    height = min(0.8 / max(len(METHODS), 1), 0.25)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))

    for idx, method in enumerate(METHODS):
        comp_times = [summary[(method, k)]["compress_ms"] for k in K_FACTORS]
        decomp_times = [summary[(method, k)]["decompress_ms"] for k in K_FACTORS]
        offset = (idx - (len(METHODS) - 1) / 2.0) * height

        ax1.barh(y + offset, comp_times, height, label=LABELS[method], color=COLORS[method])
        ax2.barh(y + offset, decomp_times, height, label=LABELS[method], color=COLORS[method])

    ax1.set_xlabel("ms")
    ax1.set_ylabel("quality_factor (k)")
    ax1.set_title("Tempo medio de compressao")
    ax1.set_yticks(y)
    ax1.set_yticklabels(labels)
    ax1.invert_yaxis()
    ax1.grid(True, axis='x', linestyle="--", alpha=0.4)
    ax2.set_xlabel("ms")
    ax2.set_ylabel("quality_factor (k)")
    ax2.set_title("Tempo medio de descompressao")
    ax2.set_yticks(y)
    ax2.set_yticklabels(labels)
    ax2.invert_yaxis()
    ax2.grid(True, axis='x', linestyle="--", alpha=0.4)
    handles, labels = ax1.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    fig.tight_layout(rect=(0, 0.09, 1, 1))
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def plot_time_metric_vs_k(summary: dict[tuple[str, int], dict[str, float]], metric: str,
                          out_path: Path, title: str):
    labels = [f"k={k}" for k in K_FACTORS]
    y = np.arange(len(labels))
    height = min(0.8 / max(len(METHODS), 1), 0.25)

    fig, ax = plt.subplots(figsize=(9, 5))
    for idx, method in enumerate(METHODS):
        xs = [summary[(method, k)][metric] for k in K_FACTORS]
        offset = (idx - (len(METHODS) - 1) / 2.0) * height
        ax.barh(y + offset, xs, height, label=LABELS[method], color=COLORS[method])

    ax.set_xlabel("Tempo medio (ms)")
    ax.set_ylabel("quality_factor (k)")
    ax.set_yticks(y)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_title(title)
    ax.grid(True, axis='x', linestyle="--", alpha=0.4)
    handles, labels = ax.get_legend_handles_labels()
    if handles:
        fig.legend(handles, labels, loc="lower center", bbox_to_anchor=(0.5, -0.02), ncol=4, fontsize=8)
    fig.tight_layout(rect=(0, 0.08, 1, 1))
    fig.savefig(out_path, dpi=180)
    plt.close(fig)


def run(imgs_dir: Path, out_dir: Path):
    lib = LibImage(_LIB_SO)
    lib_raw = lib._lib
    _setup_rle_proto(lib_raw)
    recon_dir = out_dir / "reconstructed_images"
    grid_dir = out_dir / "inspection_grids"

    img_files = sorted(p for p in imgs_dir.iterdir() if p.suffix.lower() in {".bmp", ".png", ".jpg", ".jpeg"})
    if not img_files:
        raise SystemExit(f"Nenhuma imagem em {imgs_dir}")

    out_dir.mkdir(parents=True, exist_ok=True)
    recon_dir.mkdir(parents=True, exist_ok=True)
    grid_dir.mkdir(parents=True, exist_ok=True)

    records = []
    coeff_by_case = {}

    print(f"libimage: {lib.version()}")
    print(f"dataset : {imgs_dir}")
    print(f"saida   : {out_dir}")
    print("pipeline: RGB / 4:4:4 / flags=0 / codec C int32")

    for img_idx, img_path in enumerate(img_files):
        name = img_path.stem
        arr = np.array(Image.open(img_path).convert("RGB"), dtype=np.uint8)
        height, width = arr.shape[:2]
        image_out_dir = recon_dir / name
        image_out_dir.mkdir(parents=True, exist_ok=True)
        save_rgb_image(arr, image_out_dir / f"{name}__reference{IMAGE_EXT}")
        image_cases = {}
        print(f"\n{name} ({width}x{height})")
        print(f"{'metodo':<10} {'k':>2} {'PSNR':>10} {'SSIM':>10} {'bpp':>10} {'comp ms':>10} {'decomp ms':>10} {'mem':>10}")
        for method, k in iter_cases_balanced(img_idx):
            res = process_one(lib, lib_raw, arr, method, float(k))
            coeff_by_case[(name, method, k)] = (
                res["y_quantized"],
                res.get("cb_quantized"),
                res.get("cr_quantized"),
            )
            image_cases[(method, k)] = res
            save_rgb_image(
                res["recon"],
                image_out_dir / f"{name}__k{k}__{method}{IMAGE_EXT}",
            )
            record = {
                "image": name,
                "method": method,
                "k": k,
                "psnr": res["psnr"],
                "ssim": res["ssim"],
                "bpp": res["bpp"],
                "compress_ms": res["compress_ms"],
                "decompress_ms": res["decompress_ms"],
                "memory_bytes": res["memory_bytes"],
            }
            records.append(record)

        for method in METHODS:
            for k in K_FACTORS:
                res = image_cases[(method, k)]
                print(f"{method:<10} {k:>2} {res['psnr']:>10.6f} {res['ssim']:>10.6f} {res['bpp']:>10.6f} "
                      f"{res['compress_ms']:>10.4f} {res['decompress_ms']:>10.4f} {int(res['memory_bytes']):>10}")

        render_inspection_grid(
            name,
            arr,
            image_cases,
            grid_dir / f"{name}__inspection_grid{IMAGE_EXT}",
        )

    for record in records:
        if record["method"] != "matrix":
            y_m, cb_m, cr_m = coeff_by_case[(record["image"], "matrix", record["k"])]
            y_x, cb_x, cr_x = coeff_by_case[(record["image"], record["method"], record["k"])]
            diffs = int(np.sum(y_x != y_m))
            if cb_x is not None and cb_m is not None:
                diffs += int(np.sum(cb_x != cb_m))
                diffs += int(np.sum(cr_x != cr_m))
            record["coeff_diffs_vs_matrix"] = diffs
            matrix_psnr = next(
                r["psnr"] for r in records
                if r["image"] == record["image"] and r["method"] == "matrix" and r["k"] == record["k"]
            )
            record["delta_psnr_vs_matrix"] = abs(record["psnr"] - matrix_psnr)
        else:
            record["coeff_diffs_vs_matrix"] = 0
            record["delta_psnr_vs_matrix"] = 0.0

    summary = aggregate(records)
    snapshot = build_presentation_snapshot(summary)

    out_dir.mkdir(parents=True, exist_ok=True)
    save_metrics_csv(records, out_dir / "metrics.csv")
    save_summary_csv(summary, out_dir / "summary_table.csv")
    save_presentation_csv(snapshot, out_dir / "matrix_loeffler_presentation.csv")
    save_note(snapshot, out_dir / "matrix_loeffler_note.txt")

    shutil.copyfile(out_dir / "metrics.csv", out_dir / "metrics_c.csv")
    shutil.copyfile(out_dir / "summary_table.csv", out_dir / "summary_table_c.csv")
    shutil.copyfile(out_dir / "matrix_loeffler_presentation.csv", out_dir / "matrix_loeffler_presentation_c.csv")
    shutil.copyfile(out_dir / "matrix_loeffler_note.txt", out_dir / "matrix_loeffler_note_c.txt")

    _plot_metric(summary, "psnr", "PSNR (dB)", "PSNR medio vs quality_factor", out_dir / "psnr_vs_k.png")
    _plot_metric(summary, "ssim", "SSIM", "SSIM medio vs quality_factor", out_dir / "ssim_vs_k.png")
    _plot_metric(summary, "bpp", "bpp", "Bits por pixel medio vs quality_factor", out_dir / "bpp_vs_k.png")
    plot_time_metric_vs_k(summary, "compress_ms", out_dir / "compress_time_vs_k.png",
                          "Tempo medio de compressao vs quality_factor")
    plot_time_metric_vs_k(summary, "decompress_ms", out_dir / "decompress_time_vs_k.png",
                          "Tempo medio de descompressao vs quality_factor")
    _plot_metric(summary, "memory_bytes", "bytes", "Pico de memoria dinamica do codec", out_dir / "memory_vs_k.png")
    plot_rd_curve(summary, out_dir / "rd_curve.png")
    plot_matrix_loeffler(snapshot, out_dir / "matrix_vs_loeffler.png")
    plot_time_bar_chart(summary, out_dir / "time_comparison_bar.png")
    shutil.copyfile(out_dir / "matrix_vs_loeffler.png", out_dir / "matrix_vs_loeffler_c.png")
    shutil.copyfile(out_dir / "time_comparison_bar.png", out_dir / "time_comparison_bar_c.png")

    with (out_dir / "all_results_combined.csv").open("w", newline="", encoding="utf-8") as f:
        f.write("=== METRICS (Por Imagem) ===\n")
        f.write(open(out_dir / "metrics.csv", encoding="utf-8").read())
        f.write("\n=== SUMMARY (Tabela Media) ===\n")
        f.write(open(out_dir / "summary_table.csv", encoding="utf-8").read())
        f.write("\n=== MATRIX VS LOEFFLER SNAPSHOT ===\n")
        f.write(open(out_dir / "matrix_loeffler_presentation.csv", encoding="utf-8").read())

    print("\nTabela media do dataset")
    print(f"{'metodo':<10} {'k':>2} {'PSNR':>10} {'SSIM':>10} {'bpp':>10} {'comp ms':>10} {'decomp':>10} {'mem':>10}")
    for method in METHODS:
        for k in K_FACTORS:
            row = summary[(method, k)]
            print(f"{method:<10} {k:>2} {row['psnr']:>10.6f} {row['ssim']:>10.6f} {row['bpp']:>10.6f} "
                  f"{row['compress_ms']:>10.4f} {row['decompress_ms']:>10.4f} {int(row['memory_bytes']):>10}")

    print("\nMatrix vs Loeffler (apresentacao)")
    for row in snapshot:
        print(f"k={row['k']}: PSNR {row['loeffler_psnr']:.4f} / {row['matrix_psnr']:.4f} | "
              f"SSIM {row['loeffler_ssim']:.4f} / {row['matrix_ssim']:.4f} | "
              f"bpp {row['loeffler_bpp']:.4f} / {row['matrix_bpp']:.4f}")


def main():
    parser = argparse.ArgumentParser(description="Benchmark C int32 do IC-JPEG")
    parser.add_argument("--imgs", default="imgs")
    parser.add_argument("--out", default="resultados")
    args = parser.parse_args()

    run(_HERE / args.imgs, _HERE / args.out)


if __name__ == "__main__":
    main()
