#!/usr/bin/env python3
"""Generate plots for embedded ALL runs saved as run*/run*_summary.csv."""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image


METHODS = ["loeffler", "matrix", "rdct", "silveira_j3", "silveira_j7"]
K_VALUES = [0.1, 0.2, 0.5, 0.8]
METHOD_LABELS = {
    "loeffler": "Loeffler",
    "matrix": "Matrix",
    "rdct": "RDCT",
    "silveira_j3": "Silveira j=3",
    "silveira_j7": "Silveira j=7",
}
METHOD_COLORS = {
    "loeffler": "#1f77b4",
    "matrix": "#ff7f0e",
    "rdct": "#2ca02c",
    "silveira_j3": "#9467bd",
    "silveira_j7": "#d62728",
}
METHOD_MARKERS = {
    "loeffler": "o",
    "matrix": "s",
    "rdct": "^",
    "silveira_j3": "D",
    "silveira_j7": "P",
}
NUMERIC_FIELDS = [
    "psnr",
    "ssim",
    "bpp",
    "compress_us",
    "decompress_us",
    "dct_kernel_us",
    "idct_kernel_us",
    "dct_kernel_calls",
    "idct_kernel_calls",
    "tx_us",
    "frame_bytes",
    "tx_raw_us",
    "raw_bytes",
    "compression_ratio",
]
PLOT_METRICS = [
    ("bpp", "bpp", "Bits por pixel"),
    ("psnr", "dB", "PSNR"),
    ("ssim", "SSIM", "SSIM"),
    ("compression_ratio", "raw/stream", "Razao de compressao"),
    ("compress_ms", "ms", "Tempo de compressao CAM"),
    ("decompress_ms", "ms", "Tempo de descompressao S3"),
    ("tx_ms", "ms", "Tempo de transmissao SPI"),
    ("dct_kernel_ms", "ms", "Kernel DCT CAM"),
    ("idct_kernel_ms", "ms", "Kernel IDCT S3"),
    ("total_core_ms", "ms", "Tempo core total"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "input_dir",
        nargs="?",
        default="experiments/a_dados_prototipo/novos",
        help="Directory containing run*/run*_summary.csv files.",
    )
    parser.add_argument(
        "--out",
        default=None,
        help="Output directory. Defaults to <input_dir>/plots.",
    )
    return parser.parse_args()


def parse_float(value: str | None) -> float | None:
    if value in (None, "", "N/A"):
        return None
    return float(value)


def fmt_k(k: float) -> str:
    return f"{k:g}"


def k_token(k: float) -> str:
    return fmt_k(k).replace(".", "p")


def read_text_metric(run_dir: Path, run_id: str, method: str, k: float, metric: str) -> float | None:
    path = run_dir / f"{run_id}_{method}_k{k_token(k)}.txt"
    if not path.exists():
        return None
    with path.open(encoding="utf-8") as f:
        for line in f:
            if line.startswith(f"{metric}:"):
                _, value = line.split(":", 1)
                return parse_float(value.strip())
    return None


def read_records(input_dir: Path) -> list[dict[str, float | str]]:
    records: list[dict[str, float | str]] = []
    for summary_path in sorted(input_dir.glob("run*/run*_summary.csv")):
        run_dir = summary_path.parent
        run_id = run_dir.name
        with summary_path.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                if row.get("method") == "raw":
                    continue
                if row.get("status") != "OK":
                    continue
                method = row["method"]
                if method not in METHODS:
                    continue
                k = round(float(row["k"]), 6)
                if k not in K_VALUES:
                    continue

                out: dict[str, float | str] = {
                    "run": run_id,
                    "image": row.get("image", ""),
                    "method": method,
                    "method_label": METHOD_LABELS.get(method, method),
                    "k": k,
                    "k_label": f"k={fmt_k(k)}",
                }
                for field in NUMERIC_FIELDS:
                    value = parse_float(row.get(field))
                    if value is not None:
                        out[field] = value

                for source, target in [
                    ("compress_us", "compress_ms"),
                    ("decompress_us", "decompress_ms"),
                    ("dct_kernel_us", "dct_kernel_ms"),
                    ("idct_kernel_us", "idct_kernel_ms"),
                    ("tx_us", "tx_ms"),
                    ("tx_raw_us", "tx_raw_ms"),
                ]:
                    if source in out:
                        out[target] = float(out[source]) / 1000.0

                total_core_us = read_text_metric(run_dir, run_id, method, k, "t_total_core_us")
                if total_core_us is not None:
                    out["total_core_ms"] = total_core_us / 1000.0
                records.append(out)
    return records


def group_stats(records: list[dict[str, float | str]]) -> list[dict[str, float | str]]:
    grouped: dict[tuple[str, float], list[dict[str, float | str]]] = defaultdict(list)
    for record in records:
        grouped[(str(record["method"]), float(record["k"]))].append(record)

    rows: list[dict[str, float | str]] = []
    metrics = [m[0] for m in PLOT_METRICS] + ["frame_bytes", "raw_bytes"]
    for method in METHODS:
        for k in K_VALUES:
            vals = grouped.get((method, k), [])
            if not vals:
                continue
            row: dict[str, float | str] = {
                "method": method,
                "method_label": METHOD_LABELS[method],
                "k": k,
                "k_label": f"k={fmt_k(k)}",
                "n": len(vals),
            }
            for metric in metrics:
                numbers = [float(v[metric]) for v in vals if metric in v]
                if not numbers:
                    continue
                arr = np.array(numbers, dtype=float)
                row[f"{metric}_mean"] = float(np.mean(arr))
                row[f"{metric}_std"] = float(np.std(arr, ddof=1)) if len(arr) > 1 else 0.0
                row[f"{metric}_min"] = float(np.min(arr))
                row[f"{metric}_max"] = float(np.max(arr))
            rows.append(row)
    return rows


def write_csv(path: Path, rows: list[dict[str, float | str]]) -> None:
    if not rows:
        return
    fields: list[str] = []
    for row in rows:
        for key in row.keys():
            if key not in fields:
                fields.append(key)
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def stats_by_method_k(stats: list[dict[str, float | str]]) -> dict[tuple[str, float], dict[str, float | str]]:
    return {(str(row["method"]), float(row["k"])): row for row in stats}


def plot_metric_vs_k(stats: list[dict[str, float | str]], metric: str, unit: str, title: str, out_path: Path) -> None:
    lookup = stats_by_method_k(stats)
    fig, ax = plt.subplots(figsize=(8, 5))
    for method in METHODS:
        xs: list[float] = []
        means: list[float] = []
        stds: list[float] = []
        for k in K_VALUES:
            row = lookup.get((method, k))
            if not row or f"{metric}_mean" not in row:
                continue
            xs.append(k)
            means.append(float(row[f"{metric}_mean"]))
            stds.append(float(row.get(f"{metric}_std", 0.0)))
        if not xs:
            continue
        ax.errorbar(
            xs,
            means,
            yerr=stds if len(xs) > 1 else None,
            marker=METHOD_MARKERS.get(method, "o"),
            color=METHOD_COLORS.get(method, "#444444"),
            linewidth=2,
            capsize=3,
            label=METHOD_LABELS.get(method, method),
        )
    ax.set_xlabel("Fator k")
    ax.set_ylabel(unit)
    ax.set_title(title)
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.set_xticks(K_VALUES)
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220)
    plt.close(fig)


def plot_rate_distortion(stats: list[dict[str, float | str]], y_metric: str, y_label: str, out_path: Path) -> None:
    lookup = stats_by_method_k(stats)
    fig, ax = plt.subplots(figsize=(8, 5))
    for method in METHODS:
        xs: list[float] = []
        ys: list[float] = []
        labels: list[str] = []
        for k in K_VALUES:
            row = lookup.get((method, k))
            if not row or "bpp_mean" not in row or f"{y_metric}_mean" not in row:
                continue
            xs.append(float(row["bpp_mean"]))
            ys.append(float(row[f"{y_metric}_mean"]))
            labels.append(fmt_k(k))
        if not xs:
            continue
        ax.plot(
            xs,
            ys,
            marker=METHOD_MARKERS.get(method, "o"),
            color=METHOD_COLORS.get(method, "#444444"),
            linewidth=2,
            label=METHOD_LABELS.get(method, method),
        )
        for x, y, label in zip(xs, ys, labels):
            ax.annotate(f"k={label}", (x, y), xytext=(4, 3), textcoords="offset points", fontsize=7)
    ax.set_xlabel("bpp")
    ax.set_ylabel(y_label)
    ax.set_title(f"{y_label} vs bpp")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220)
    plt.close(fig)


def plot_bpp_heatmap(records: list[dict[str, float | str]], out_path: Path) -> None:
    runs = sorted({str(r["run"]) for r in records})
    labels = [f"{METHOD_LABELS[m]}\nk={fmt_k(k)}" for k in K_VALUES for m in METHODS]
    columns = [(m, k) for k in K_VALUES for m in METHODS]
    data = np.full((len(runs), len(columns)), np.nan)
    by_key = {(str(r["run"]), str(r["method"]), float(r["k"])): r for r in records}
    for i, run in enumerate(runs):
        for j, (method, k) in enumerate(columns):
            row = by_key.get((run, method, k))
            if row and "bpp" in row:
                data[i, j] = float(row["bpp"])

    fig_w = max(10, len(columns) * 0.55)
    fig, ax = plt.subplots(figsize=(fig_w, max(4, len(runs) * 0.55)))
    im = ax.imshow(data, aspect="auto", cmap="viridis")
    ax.set_xticks(range(len(columns)))
    ax.set_xticklabels(labels, rotation=70, ha="right", fontsize=7)
    ax.set_yticks(range(len(runs)))
    ax.set_yticklabels(runs)
    ax.set_title("bpp por run/metodo/k")
    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label("bpp")
    fig.tight_layout()
    fig.savefig(out_path, dpi=220)
    plt.close(fig)


def raw_saturation(input_dir: Path) -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    for path in sorted(input_dir.glob("run*/run*_raw.bmp")):
        with Image.open(path) as img:
            arr = np.asarray(img.convert("RGB"), dtype=np.uint8)
        any_hi = np.any(arr >= 250, axis=2)
        all_hi = np.all(arr >= 250, axis=2)
        gray = np.mean(arr.astype(float), axis=2)
        rows.append({
            "run": path.parent.name,
            "raw_file": path.name,
            "any_channel_ge_250_pct": float(np.mean(any_hi) * 100.0),
            "all_channels_ge_250_pct": float(np.mean(all_hi) * 100.0),
            "gray_ge_230_pct": float(np.mean(gray >= 230.0) * 100.0),
            "mean_luma_0_255": float(np.mean(gray)),
        })
    return rows


def plot_saturation(rows: list[dict[str, float | str]], out_path: Path) -> None:
    if not rows:
        return
    runs = [str(r["run"]) for r in rows]
    all_hi = [float(r["all_channels_ge_250_pct"]) for r in rows]
    any_hi = [float(r["any_channel_ge_250_pct"]) for r in rows]
    gray_hi = [float(r["gray_ge_230_pct"]) for r in rows]
    x = np.arange(len(runs))
    width = 0.26
    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.bar(x - width, any_hi, width, label="algum canal >=250", color="#1f77b4")
    ax.bar(x, all_hi, width, label="todos canais >=250", color="#d62728")
    ax.bar(x + width, gray_hi, width, label="media RGB >=230", color="#2ca02c")
    ax.set_xticks(x)
    ax.set_xticklabels(runs)
    ax.set_ylabel("% pixels")
    ax.set_title("Saturacao nas referencias RAW")
    ax.grid(True, axis="y", linestyle="--", alpha=0.4)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220)
    plt.close(fig)


def make_capture_grid(input_dir: Path, out_path: Path) -> None:
    paths = sorted(input_dir.glob("run*/run*_raw.bmp"))
    if not paths:
        return

    cols = 3
    cell_w = 320
    cell_h = 240
    rows = int(np.ceil(len(paths) / cols))
    canvas = Image.new("RGB", (cols * cell_w, rows * cell_h), "black")

    resample = Image.Resampling.LANCZOS
    for idx, path in enumerate(paths):
        row = idx // cols
        col = idx % cols
        x = col * cell_w
        y = row * cell_h

        # All prototype captures are QVGA. Resize defensively so every cell
        # remains fully occupied without labels, padding, or white margins.
        with Image.open(path) as img:
            frame = img.convert("RGB").resize((cell_w, cell_h), resample)
        canvas.paste(frame, (x, y))

    canvas.save(out_path)


def mean_metric_by_method(
    lookup: dict[tuple[str, float], dict[str, float | str]],
    method: str,
    metric: str,
) -> float:
    values = [
        float(lookup[(method, k)][f"{metric}_mean"])
        for k in K_VALUES
        if (method, k) in lookup and f"{metric}_mean" in lookup[(method, k)]
    ]
    return float(np.mean(values)) if values else 0.0


def write_readme(out_dir: Path, input_dir: Path, records: list[dict[str, float | str]]) -> None:
    readme = out_dir / "README.md"
    readme.write_text(
        "# Plots dos novos experimentos ALL\n\n"
        f"Fonte: `{input_dir}`\n\n"
        f"Registros JPEG OK usados: {len(records)}.\n\n"
        "Arquivos principais:\n\n"
        "- `records_all.csv`: todos os registros JPEG OK normalizados.\n"
        "- `summary_by_method_k.csv`: media/desvio por metodo e k.\n"
        "- `*_vs_k.png`: curvas medias com desvio padrao entre runs.\n"
        "- `rate_distortion_psnr_bpp.png`: PSNR medio contra bpp.\n"
        "- `rate_distortion_ssim_bpp.png`: SSIM medio contra bpp.\n"
        "- `bpp_by_run_heatmap.png`: bpp por run/metodo/k.\n"
        "- `raw_saturation_by_run.png`: pixels saturados nas referencias RAW.\n"
        "- `raw_capture_grid.png`: grade visual das referencias RAW.\n",
        encoding="utf-8",
    )


def write_paper_data(out_dir: Path, input_dir: Path, records: list[dict[str, float | str]],
                     stats: list[dict[str, float | str]],
                     saturation_rows: list[dict[str, float | str]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    paper_metrics = [
        "psnr",
        "ssim",
        "bpp",
        "compress_ms",
        "decompress_ms",
        "dct_kernel_ms",
        "idct_kernel_ms",
        "dct_kernel_calls",
        "idct_kernel_calls",
        "tx_ms",
        "total_core_ms",
        "compression_ratio",
        "frame_bytes",
        "raw_bytes",
    ]

    selected_rows: list[dict[str, float | str]] = []
    for row in stats:
        out: dict[str, float | str] = {
            "platform": "embedded",
            "mean_scope": "prototype_runs_mean",
            "method": row["method"],
            "label": row["method_label"],
            "k": row["k"],
            "k_label": row["k_label"],
            "n": row["n"],
        }
        for metric in paper_metrics:
            if f"{metric}_mean" in row:
                out[metric] = row[f"{metric}_mean"]
            if f"{metric}_std" in row:
                out[f"{metric}_std"] = row[f"{metric}_std"]
        selected_rows.append(out)
    write_csv(out_dir / "embedded_summary_selected.csv", selected_rows)

    lookup = stats_by_method_k(stats)
    for metric in paper_metrics:
        wide_rows: list[dict[str, float | str]] = []
        for k in K_VALUES:
            wide: dict[str, float | str] = {"k": k, "k_label": f"k={fmt_k(k)}"}
            for method in METHODS:
                row = lookup.get((method, k))
                wide[method] = row.get(f"{metric}_mean", "") if row else ""
            wide_rows.append(wide)
        write_csv(out_dir / f"embedded_{metric}_vs_k.csv", wide_rows)

    for method in METHODS:
        rd_rows: list[dict[str, float | str]] = []
        for k in K_VALUES:
            row = lookup.get((method, k), {})
            rd_rows.append({
                "k": k,
                "k_label": f"k={fmt_k(k)}",
                "bpp": row.get("bpp_mean", ""),
                "psnr": row.get("psnr_mean", ""),
                "ssim": row.get("ssim_mean", ""),
                "point_label": f"k={fmt_k(k)}",
            })
        write_csv(out_dir / f"embedded_rd_{method}.csv", rd_rows)

    gain_rows: list[dict[str, float | str]] = []
    for k in K_VALUES:
        j7 = lookup.get(("silveira_j7", k), {})
        rdct = lookup.get(("rdct", k), {})
        gain_rows.append({
            "k": k,
            "k_label": f"k={fmt_k(k)}",
            "psnr_gain_db": (
                float(j7["psnr_mean"]) - float(rdct["psnr_mean"])
                if "psnr_mean" in j7 and "psnr_mean" in rdct else ""
            ),
            "ssim_gain": (
                float(j7["ssim_mean"]) - float(rdct["ssim_mean"])
                if "ssim_mean" in j7 and "ssim_mean" in rdct else ""
            ),
            "bpp_delta": (
                float(j7["bpp_mean"]) - float(rdct["bpp_mean"])
                if "bpp_mean" in j7 and "bpp_mean" in rdct else ""
            ),
        })
    write_csv(out_dir / "embedded_silveira_j7_gain_vs_rdct.csv", gain_rows)

    for metric in [
        "compress_ms",
        "decompress_ms",
        "dct_kernel_ms",
        "idct_kernel_ms",
        "tx_ms",
        "total_core_ms",
    ]:
        bar_rows: list[dict[str, float | str]] = []
        for method in METHODS:
            out: dict[str, float | str] = {
                "method": method,
                "method_label": METHOD_LABELS[method],
            }
            for k in K_VALUES:
                row = lookup.get((method, k))
                out[f"k{k_token(k)}"] = row.get(f"{metric}_mean", "") if row else ""
            bar_rows.append(out)
        write_csv(out_dir / f"embedded_{metric}_bar.csv", bar_rows)

    breakdown_rows: list[dict[str, float | str]] = []
    for method in METHODS:
        compress_total = mean_metric_by_method(lookup, method, "compress_ms")
        compress_kernel = mean_metric_by_method(lookup, method, "dct_kernel_ms")
        decompress_total = mean_metric_by_method(lookup, method, "decompress_ms")
        decompress_kernel = mean_metric_by_method(lookup, method, "idct_kernel_ms")
        breakdown_rows.append({
            "method": method,
            "method_label": METHOD_LABELS[method],
            "compress_dct_ms": compress_kernel,
            "compress_other_ms": max(0.0, compress_total - compress_kernel),
            "compress_total_ms": compress_total,
            "decompress_idct_ms": decompress_kernel,
            "decompress_other_ms": max(0.0, decompress_total - decompress_kernel),
            "decompress_total_ms": decompress_total,
        })
    write_csv(out_dir / "embedded_time_breakdown_bar.csv", breakdown_rows)

    scene_rows: list[dict[str, float | str]] = []
    for record in records:
        scene_rows.append({
            "run": record.get("run", ""),
            "image": record.get("image", ""),
            "method": record.get("method", ""),
            "method_label": record.get("method_label", ""),
            "k": record.get("k", ""),
            "k_label": record.get("k_label", ""),
            **{metric: record.get(metric, "") for metric in paper_metrics},
            "tx_raw_ms": record.get("tx_raw_ms", ""),
        })
    write_csv(out_dir / "embedded_scene_records.csv", scene_rows)

    transmission_rows = [
        {
            "run": record.get("run", ""),
            "method": record.get("method", ""),
            "k": record.get("k", ""),
            "bpp": record.get("bpp", ""),
            "frame_bytes": record.get("frame_bytes", ""),
            "raw_bytes": record.get("raw_bytes", ""),
            "compression_ratio": record.get("compression_ratio", ""),
            "tx_ms": record.get("tx_ms", ""),
            "tx_raw_ms": record.get("tx_raw_ms", ""),
        }
        for record in records
    ]
    write_csv(out_dir / "embedded_transmission_table.csv", transmission_rows)
    write_csv(out_dir / "embedded_raw_saturation_by_run.csv", saturation_rows)

    capture_rows: list[dict[str, float | str]] = []
    for order, raw_path in enumerate(sorted(input_dir.glob("run*/run*_raw.bmp")), start=1):
        with Image.open(raw_path) as img:
            width, height = img.size
        capture_rows.append({
            "order": order,
            "run": raw_path.parent.name,
            "filename": raw_path.name,
            "width": width,
            "height": height,
            "pixels": width * height,
        })
    write_csv(out_dir / "embedded_capture_images.csv", capture_rows)
    make_capture_grid(input_dir, out_dir / "embedded_capture_grid.png")

def plot_records_metric_vs_k(records: list[dict[str, float | str]], metric: str,
                             unit: str, title: str, out_path: Path) -> None:
    by_key = {(str(r["method"]), float(r["k"])): r for r in records}
    fig, ax = plt.subplots(figsize=(8, 5))
    for method in METHODS:
        xs: list[float] = []
        ys: list[float] = []
        for k in K_VALUES:
            row = by_key.get((method, k))
            if row and metric in row:
                xs.append(k)
                ys.append(float(row[metric]))
        if not xs:
            continue
        ax.plot(
            xs,
            ys,
            marker=METHOD_MARKERS.get(method, "o"),
            color=METHOD_COLORS.get(method, "#444444"),
            linewidth=2,
            label=METHOD_LABELS.get(method, method),
        )
    ax.set_xlabel("Fator k")
    ax.set_ylabel(unit)
    ax.set_title(title)
    ax.set_xticks(K_VALUES)
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220)
    plt.close(fig)


def plot_records_rate_distortion(records: list[dict[str, float | str]],
                                 y_metric: str, y_label: str, out_path: Path) -> None:
    by_key = {(str(r["method"]), float(r["k"])): r for r in records}
    fig, ax = plt.subplots(figsize=(8, 5))
    for method in METHODS:
        xs: list[float] = []
        ys: list[float] = []
        labels: list[str] = []
        for k in K_VALUES:
            row = by_key.get((method, k))
            if row and "bpp" in row and y_metric in row:
                xs.append(float(row["bpp"]))
                ys.append(float(row[y_metric]))
                labels.append(fmt_k(k))
        if not xs:
            continue
        ax.plot(
            xs,
            ys,
            marker=METHOD_MARKERS.get(method, "o"),
            color=METHOD_COLORS.get(method, "#444444"),
            linewidth=2,
            label=METHOD_LABELS.get(method, method),
        )
        for x, y, label in zip(xs, ys, labels):
            ax.annotate(f"k={label}", (x, y), xytext=(4, 3), textcoords="offset points", fontsize=7)
    ax.set_xlabel("bpp")
    ax.set_ylabel(y_label)
    ax.set_title(f"{y_label} vs bpp")
    ax.grid(True, linestyle="--", alpha=0.4)
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_path, dpi=220)
    plt.close(fig)


def save_raw_reference(input_dir: Path, run_name: str, out_path: Path) -> None:
    raw_path = input_dir / run_name / f"{run_name}_raw.bmp"
    if not raw_path.exists():
        return
    with Image.open(raw_path) as img:
        img.convert("RGB").save(out_path)


def write_individual_run_outputs(out_dir: Path, input_dir: Path,
                                 records: list[dict[str, float | str]]) -> None:
    by_run: dict[str, list[dict[str, float | str]]] = defaultdict(list)
    for record in records:
        by_run[str(record["run"])].append(record)

    for run_name in sorted(by_run):
        run_dir = out_dir / run_name
        run_dir.mkdir(parents=True, exist_ok=True)
        run_records = sorted(by_run[run_name], key=lambda r: (float(r["k"]), METHODS.index(str(r["method"]))))
        write_csv(run_dir / f"{run_name}_records.csv", run_records)

        for metric, unit, title in PLOT_METRICS:
            plot_records_metric_vs_k(
                run_records,
                metric,
                unit,
                f"{run_name} - {title}",
                run_dir / f"{metric}_vs_k.png",
            )

        plot_records_rate_distortion(
            run_records,
            "psnr",
            "PSNR (dB)",
            run_dir / "rate_distortion_psnr_bpp.png",
        )
        plot_records_rate_distortion(
            run_records,
            "ssim",
            "SSIM",
            run_dir / "rate_distortion_ssim_bpp.png",
        )
        save_raw_reference(input_dir, run_name, run_dir / "raw_reference.png")
        (run_dir / "README.md").write_text(
            f"# Plots individuais - {run_name}\n\n"
            f"Fonte: `{input_dir / run_name}`\n\n"
            "Cada PNG usa apenas os 20 registros JPEG OK deste run, todos derivados da mesma referencia RAW.\n",
            encoding="utf-8",
        )


def write_aggregate_outputs(out_dir: Path, input_dir: Path, records: list[dict[str, float | str]],
                            stats: list[dict[str, float | str]],
                            saturation_rows: list[dict[str, float | str]]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(out_dir / "records_all.csv", records)
    write_csv(out_dir / "summary_by_method_k.csv", stats)
    write_csv(out_dir / "raw_saturation_by_run.csv", saturation_rows)

    for metric, unit, title in PLOT_METRICS:
        plot_metric_vs_k(stats, metric, unit, title, out_dir / f"{metric}_vs_k.png")

    plot_rate_distortion(stats, "psnr", "PSNR (dB)", out_dir / "rate_distortion_psnr_bpp.png")
    plot_rate_distortion(stats, "ssim", "SSIM", out_dir / "rate_distortion_ssim_bpp.png")
    plot_bpp_heatmap(records, out_dir / "bpp_by_run_heatmap.png")
    plot_saturation(saturation_rows, out_dir / "raw_saturation_by_run.png")
    make_capture_grid(input_dir, out_dir / "raw_capture_grid.png")
    write_readme(out_dir, input_dir, records)


def main() -> None:
    args = parse_args()
    input_dir = Path(args.input_dir).resolve()
    out_dir = Path(args.out).resolve() if args.out else input_dir / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    records = read_records(input_dir)
    if not records:
        raise SystemExit(f"Nenhum registro JPEG OK encontrado em {input_dir}")
    stats = group_stats(records)
    sat = raw_saturation(input_dir)

    write_aggregate_outputs(out_dir / "aggregate", input_dir, records, stats, sat)
    write_paper_data(out_dir / "paper_data", input_dir, records, stats, sat)
    write_individual_run_outputs(out_dir / "individual", input_dir, records)

    # Compatibilidade: mantem tambem os arquivos principais diretamente em plots/.
    write_csv(out_dir / "records_all.csv", records)
    write_csv(out_dir / "summary_by_method_k.csv", stats)

    for metric, unit, title in PLOT_METRICS:
        plot_metric_vs_k(stats, metric, unit, title, out_dir / f"{metric}_vs_k.png")

    plot_rate_distortion(stats, "psnr", "PSNR (dB)", out_dir / "rate_distortion_psnr_bpp.png")
    plot_rate_distortion(stats, "ssim", "SSIM", out_dir / "rate_distortion_ssim_bpp.png")
    plot_bpp_heatmap(records, out_dir / "bpp_by_run_heatmap.png")

    write_csv(out_dir / "raw_saturation_by_run.csv", sat)
    plot_saturation(sat, out_dir / "raw_saturation_by_run.png")
    make_capture_grid(input_dir, out_dir / "raw_capture_grid.png")
    write_readme(out_dir, input_dir, records)

    print(f"Plots e dados do paper gerados em: {out_dir}")


if __name__ == "__main__":
    main()
