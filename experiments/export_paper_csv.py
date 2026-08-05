#!/usr/bin/env python3
"""Export compact CSV files for LaTeX/PGFPlots paper figures.

Inputs are the benchmark summaries already produced by compare.py and
compare_c.py.  Outputs are intentionally small and stable, so the paper can
plot from CSV instead of manually inlining coordinates.
"""

from __future__ import annotations

import csv
from pathlib import Path

from PIL import Image


HERE = Path(__file__).resolve().parent
OUT = HERE / "paper_csv"
IMAGES_DIR = HERE / "imgs"
EMBEDDED_DATA_DIR = HERE / "a_dados_prototipo"

C_SUMMARY = HERE / "resultados" / "summary_table.csv"
PY_SUMMARY_CANDIDATES = [
    HERE / "results" / "comparison" / "summary_table_python.csv",
    HERE / "src_py" / "results" / "comparison" / "summary_table_python.csv",
    HERE / "results" / "comparison" / "summary_table.csv",
    HERE / "src_py" / "results" / "comparison" / "summary_table.csv",
    HERE / "results" / "comparison_small" / "summary_table_python.csv",
    HERE / "results" / "comparison_small" / "summary_table.csv",
]

SELECTED_METHODS = ["loeffler", "matrix", "rdct", "silveira_j3", "silveira_j7"]
METHOD_LABELS = {
    "loeffler": "Loeffler",
    "matrix": "Matrix",
    "rdct": "RDCT",
    "silveira_j3": "Silveira j=3",
    "silveira_j7": "Silveira j=7",
}
BAR_METHOD_LABELS = {
    "loeffler": "Loeffler",
    "matrix": "Matrix",
    "rdct": "RDCT",
    "silveira_j3": "Silv. j=3",
    "silveira_j7": "Silv. j=7",
}
K_VALUES = [0.1, 0.2, 0.5, 0.8]
METRICS = ["psnr", "ssim", "bpp", "compress_ms", "decompress_ms"]
EMBEDDED_METRICS = [
    *METRICS,
    "dct_kernel_ms",
    "idct_kernel_ms",
    "dct_kernel_calls",
    "idct_kernel_calls",
    "tx_ms",
    "total_core_ms",
    "compression_ratio",
]
TIME_BAR_METRICS = ["compress_ms", "decompress_ms", "tx_ms", "total_core_ms"]
IMAGE_SUFFIXES = {".bmp", ".png", ".jpg", ".jpeg"}
EMBEDDED_ROTATE_180_SCENES = {"Bancada", "painel de ferramentas"}


def read_summary(path: Path) -> dict[tuple[str, float], dict[str, float]]:
    rows: dict[tuple[str, float], dict[str, float]] = {}
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            method = row["method"]
            k = normalize_k(row["k"])
            normalized = {"method": method, "k": float(k)}
            for key, value in row.items():
                if key in ("method", "k"):
                    continue
                if value == "":
                    continue
                normalized[canonical_metric_name(key)] = float(value)
            rows[(method, k)] = normalized
    return rows


def find_python_summary() -> Path | None:
    for path in PY_SUMMARY_CANDIDATES:
        if path.exists():
            return path
    return None


def empty_summary(metrics: list[str] = METRICS) -> dict[tuple[str, float], dict[str, float]]:
    return {
        (method, k): {metric: "" for metric in metrics}
        for method in SELECTED_METHODS
        for k in K_VALUES
    }


def has_selected_data(data: dict[tuple[str, float], dict[str, float]]) -> bool:
    for method in SELECTED_METHODS:
        for k in K_VALUES:
            row = data.get((method, k))
            if row and row.get("psnr", "") != "":
                return True
    return False


def write_long(platform: str, data: dict[tuple[str, float], dict[str, float]],
               metrics: list[str] = METRICS) -> None:
    path = OUT / platform / f"{platform}_summary_selected.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["platform", "mean_scope", "method", "label", "k", "k_label", *metrics]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for method in SELECTED_METHODS:
            for k in K_VALUES:
                row = data.get((method, k), {})
                writer.writerow({
                    "platform": platform,
                    "mean_scope": "dataset_mean",
                    "method": method,
                    "label": METHOD_LABELS[method],
                    "k": k,
                    "k_label": f"k={fmt_k(k)}",
                    **{m: fmt(row.get(m, "")) for m in metrics},
                })


def write_metric_wide(platform: str, metric: str,
                      data: dict[tuple[str, float], dict[str, float]]) -> None:
    path = OUT / platform / f"{platform}_{metric}_vs_k.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["k", "k_label", *SELECTED_METHODS]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for k in K_VALUES:
            out = {"k": k, "k_label": f"k={fmt_k(k)}"}
            for method in SELECTED_METHODS:
                out[method] = fmt(data.get((method, k), {}).get(metric, ""))
            writer.writerow(out)


def write_rd_by_method(platform: str,
                       data: dict[tuple[str, float], dict[str, float]]) -> None:
    for method in SELECTED_METHODS:
        path = OUT / platform / f"{platform}_rd_{method}.csv"
        path.parent.mkdir(parents=True, exist_ok=True)
        fields = ["k", "k_label", "bpp", "psnr", "point_label"]
        with path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            for k in K_VALUES:
                row = data.get((method, k), {})
                writer.writerow({
                    "k": k,
                    "k_label": f"k={fmt_k(k)}",
                    "bpp": fmt(row.get("bpp", "")),
                    "psnr": fmt(row.get("psnr", "")),
                    "point_label": f"k={fmt_k(k)}",
                })


def write_time_bar_wide(platform: str, metric: str,
                        data: dict[tuple[str, float], dict[str, float]]) -> None:
    path = OUT / platform / f"{platform}_{metric}_bar.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    k_columns = [k_column_name(k) for k in K_VALUES]
    fields = ["method", "method_label", *k_columns]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for method in SELECTED_METHODS:
            out = {
                "method": method,
                "method_label": BAR_METHOD_LABELS[method],
            }
            for k in K_VALUES:
                out[k_column_name(k)] = fmt(data.get((method, k), {}).get(metric, ""))
            writer.writerow(out)


def write_gain(platform: str, data: dict[tuple[str, float], dict[str, float]]) -> None:
    path = OUT / platform / f"{platform}_silveira_j7_gain_vs_rdct.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["k", "k_label", "psnr_gain_db", "ssim_gain", "bpp_delta"]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for k in K_VALUES:
            j7 = data.get(("silveira_j7", k))
            rdct = data.get(("rdct", k))
            if not j7 or not rdct or j7.get("psnr", "") == "" or rdct.get("psnr", "") == "":
                writer.writerow({
                    "k": k,
                    "k_label": f"k={fmt_k(k)}",
                    "psnr_gain_db": "",
                    "ssim_gain": "",
                    "bpp_delta": "",
                })
                continue
            writer.writerow({
                "k": k,
                "k_label": f"k={fmt_k(k)}",
                "psnr_gain_db": fmt(j7["psnr"] - rdct["psnr"]),
                "ssim_gain": fmt(j7["ssim"] - rdct["ssim"]),
                "bpp_delta": fmt(j7["bpp"] - rdct["bpp"]),
            })


def dataset_image_paths() -> list[Path]:
    return sorted(
        path for path in IMAGES_DIR.iterdir()
        if path.suffix.lower() in IMAGE_SUFFIXES
    )


def write_dataset_metadata() -> int:
    paths = dataset_image_paths()
    out_dir = OUT / "dataset"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "dataset_images.csv"
    fields = [
        "order",
        "image",
        "filename",
        "width",
        "height",
        "pixels",
        "megapixels",
        "aspect_ratio",
    ]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for order, img_path in enumerate(paths, start=1):
            with Image.open(img_path) as img:
                width, height = img.size
            pixels = width * height
            writer.writerow({
                "order": order,
                "image": img_path.stem,
                "filename": img_path.name,
                "width": width,
                "height": height,
                "pixels": pixels,
                "megapixels": f"{pixels / 1_000_000:.4f}",
                "aspect_ratio": f"{width / height:.4f}",
            })
    write_dataset_grid(paths, out_dir / "dataset_grid.png")
    return len(paths)


def write_dataset_grid(paths: list[Path], out_path: Path,
                       rotate_180_parents: set[str] | None = None) -> None:
    if not paths:
        return
    rotate_180_parents = rotate_180_parents or set()

    columns = 4
    pad = 6
    canvas_w = 1200
    rows = [paths[i:i + columns] for i in range(0, len(paths), columns)]

    row_layouts: list[tuple[list[Path], list[int], int]] = []
    canvas_h = pad
    for row_paths in rows:
        sizes = []
        aspects = []
        for img_path in row_paths:
            with Image.open(img_path) as img:
                width, height = img.size
            sizes.append((width, height))
            aspects.append(width / height)

        available_w = canvas_w - pad * (len(row_paths) + 1)
        row_h = max(1, int(round(available_w / sum(aspects))))
        widths = [max(1, int(round(row_h * aspect))) for aspect in aspects]
        widths[-1] += available_w - sum(widths)
        row_layouts.append((row_paths, widths, row_h))
        canvas_h += row_h + pad

    canvas = Image.new("RGB", (canvas_w, canvas_h), "white")
    y = pad
    for row_paths, widths, row_h in row_layouts:
        x = pad
        for img_path, width in zip(row_paths, widths):
            with Image.open(img_path) as img:
                rgb = img.convert("RGB").resize((width, row_h), Image.Resampling.LANCZOS)
            if img_path.parent.name in rotate_180_parents:
                rgb = rgb.rotate(180)
            canvas.paste(rgb, (x, y))
            x += width + pad
        y += row_h + pad

    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)


def embedded_capture_paths() -> list[Path]:
    if not EMBEDDED_DATA_DIR.exists():
        return []

    paths: list[Path] = []
    for scene_dir in sorted(path for path in EMBEDDED_DATA_DIR.iterdir() if path.is_dir()):
        preferred = scene_dir / "run0001_ref.bmp"
        fallback = scene_dir / "run0001_raw.bmp"
        if preferred.exists():
            paths.append(preferred)
        elif fallback.exists():
            paths.append(fallback)
    return paths


def write_embedded_capture_gallery() -> None:
    paths = embedded_capture_paths()
    if not paths:
        return

    out_dir = OUT / "embedded"
    out_dir.mkdir(parents=True, exist_ok=True)
    metadata_path = out_dir / "embedded_capture_images.csv"
    fields = ["order", "scene", "filename", "width", "height"]
    with metadata_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for order, img_path in enumerate(paths, start=1):
            with Image.open(img_path) as img:
                width, height = img.size
            writer.writerow({
                "order": order,
                "scene": img_path.parent.name,
                "filename": img_path.name,
                "width": width,
                "height": height,
            })

    write_dataset_grid(
        paths,
        out_dir / "embedded_capture_grid.png",
        rotate_180_parents=EMBEDDED_ROTATE_180_SCENES,
    )


def write_desktop_combined(c_data: dict[tuple[str, float], dict[str, float]],
                           py_data: dict[tuple[str, float], dict[str, float]]) -> None:
    out_dir = OUT / "desktop"
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "desktop_summary_selected.csv"
    fields = ["platform", "mean_scope", "method", "label", "k", "k_label", *METRICS]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for platform, data in (("python", py_data), ("c", c_data)):
            for method in SELECTED_METHODS:
                for k in K_VALUES:
                    row = data.get((method, k), {})
                    writer.writerow({
                        "platform": platform,
                        "mean_scope": "dataset_mean",
                        "method": method,
                        "label": METHOD_LABELS[method],
                        "k": k,
                        "k_label": f"k={fmt_k(k)}",
                        **{m: fmt(row.get(m, "")) for m in METRICS},
                    })

def write_embedded_placeholders() -> None:
    platform = "embedded"
    empty_data = empty_summary(EMBEDDED_METRICS)
    write_long(platform, empty_data, EMBEDDED_METRICS)
    for metric in EMBEDDED_METRICS:
        write_metric_wide(platform, metric, empty_data)
    write_rd_by_method(platform, empty_data)
    for metric in TIME_BAR_METRICS:
        write_time_bar_wide(platform, metric, empty_data)

def embedded_prototype_records() -> list[dict[str, float | str]]:
    if not EMBEDDED_DATA_DIR.exists():
        return []

    records: list[dict[str, float | str]] = []
    for summary_path in sorted(EMBEDDED_DATA_DIR.glob("*/*_summary.csv")):
        with summary_path.open(newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                method = row.get("method", "")
                if method not in SELECTED_METHODS:
                    continue
                if row.get("status") != "OK":
                    continue
                k = normalize_k(row.get("k", ""))
                if k not in K_VALUES:
                    continue

                values = {
                    "psnr": parse_float(row.get("psnr")),
                    "ssim": parse_float(row.get("ssim")),
                    "bpp": parse_float(row.get("bpp")),
                    "compress_ms": parse_float(row.get("compress_us"), scale=1000.0),
                    "decompress_ms": parse_float(row.get("decompress_us"), scale=1000.0),
                    "dct_kernel_ms": parse_float(row.get("dct_kernel_us"), scale=1000.0),
                    "idct_kernel_ms": parse_float(row.get("idct_kernel_us"), scale=1000.0),
                    "dct_kernel_calls": parse_float(row.get("dct_kernel_calls")),
                    "idct_kernel_calls": parse_float(row.get("idct_kernel_calls")),
                    "tx_ms": parse_float(row.get("tx_us"), scale=1000.0),
                    "compression_ratio": parse_float(row.get("compression_ratio")),
                }
                total_us = read_embedded_text_metric(summary_path.parent, method, k, "t_total_core_us")
                if total_us is not None:
                    values["total_core_ms"] = total_us / 1000.0
                else:
                    parts = [values.get("compress_ms"), values.get("decompress_ms"), values.get("tx_ms")]
                    if all(part is not None for part in parts):
                        values["total_core_ms"] = sum(part for part in parts if part is not None)

                records.append({
                    "scene": summary_path.parent.name,
                    "method": method,
                    "k": k,
                    **{metric: value for metric, value in values.items() if value is not None},
                })

    return records


def read_embedded_prototype_data() -> dict[tuple[str, float], dict[str, float]]:
    records = embedded_prototype_records()
    if not records:
        return empty_summary(EMBEDDED_METRICS)

    accum: dict[tuple[str, float], dict[str, list[float]]] = {}
    for record in records:
        key = (str(record["method"]), float(record["k"]))
        bucket = accum.setdefault(key, {metric: [] for metric in EMBEDDED_METRICS})
        for metric in EMBEDDED_METRICS:
            value = record.get(metric)
            if value is not None:
                bucket[metric].append(float(value))

    data = empty_summary(EMBEDDED_METRICS)
    for key, metrics in accum.items():
        data[key] = {
            metric: (sum(values) / len(values)) if values else ""
            for metric, values in metrics.items()
        }
    return data


def write_embedded_scene_records(records: list[dict[str, float | str]]) -> None:
    path = OUT / "embedded" / "embedded_scene_records.csv"
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["scene", "method", "k", *EMBEDDED_METRICS]
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for record in records:
            writer.writerow({
                "scene": record.get("scene", ""),
                "method": record.get("method", ""),
                "k": fmt(record.get("k", "")),
                **{metric: fmt(record.get(metric, "")) for metric in EMBEDDED_METRICS},
            })


def write_embedded_outputs(data: dict[tuple[str, float], dict[str, float]]) -> None:
    platform = "embedded"
    write_embedded_capture_gallery()
    write_embedded_scene_records(embedded_prototype_records())
    write_long(platform, data, EMBEDDED_METRICS)
    write_gain(platform, data)
    write_rd_by_method(platform, data)
    for metric in EMBEDDED_METRICS:
        write_metric_wide(platform, metric, data)
    for metric in TIME_BAR_METRICS:
        write_time_bar_wide(platform, metric, data)

def fmt(value) -> str:
    if value == "" or value is None:
        return ""
    return f"{float(value):.8f}"


def parse_float(value, scale: float = 1.0) -> float | None:
    if value in (None, "", "N/A"):
        return None
    return float(value) / scale


def read_embedded_text_metric(scene_dir: Path, method: str, k: float, metric: str) -> float | None:
    token = k_file_token(k)
    for path in sorted(scene_dir.glob(f"run*_{method}_k{token}.txt")):
        with path.open(encoding="utf-8") as f:
            for line in f:
                if not line.startswith(f"{metric}:"):
                    continue
                _, value = line.split(":", 1)
                return parse_float(value.strip())
    return None


def canonical_metric_name(name: str) -> str:
    aliases = {
        "comp_ms": "compress_ms",
        "decomp_ms": "decompress_ms",
    }
    return aliases.get(name, name)


def fmt_k(value: float) -> str:
    return f"{float(value):g}"


def k_file_token(value: float) -> str:
    return fmt_k(value).replace(".", "p")


def k_column_name(value: float) -> str:
    return f"k{k_file_token(value)}"


def normalize_k(value) -> float:
    return round(float(value), 6)


def main() -> None:
    c_data = read_summary(C_SUMMARY)
    py_path = find_python_summary()
    py_data = read_summary(py_path) if py_path else empty_summary()
    if py_path and not has_selected_data(py_data):
        py_path = None
        py_data = empty_summary()

    OUT.mkdir(parents=True, exist_ok=True)
    dataset_count = write_dataset_metadata()
    for platform, data in (("c", c_data), ("python", py_data)):
        write_long(platform, data)
        write_gain(platform, data)
        write_rd_by_method(platform, data)
        for metric in METRICS:
            write_metric_wide(platform, metric, data)
        if platform == "c":
            write_time_bar_wide(platform, "compress_ms", data)
            write_time_bar_wide(platform, "decompress_ms", data)

    write_desktop_combined(c_data, py_data)
    embedded_data = read_embedded_prototype_data()
    if has_selected_data(embedded_data):
        write_embedded_outputs(embedded_data)
        embedded_source = EMBEDDED_DATA_DIR.relative_to(HERE.parent)
    else:
        write_embedded_placeholders()
        embedded_source = "schema-only placeholder"

    index = OUT / "README.md"
    index.write_text(
        "# Paper CSV exports\n\n"
        f"- C source: `{C_SUMMARY.relative_to(HERE.parent)}`\n"
        f"- Python source: `{py_path.relative_to(HERE.parent) if py_path else 'not generated; schema-only placeholder'}`\n"
        "- Selected methods: loeffler, matrix, rdct, silveira_j3, silveira_j7\n"
        "- Metrics: psnr, ssim, bpp, compress_ms, decompress_ms\n"
        f"- Embedded source: `{embedded_source}`\n"
        f"- Dataset mean: {dataset_count} images, ordered lexicographically as "
        "`compare.py` and `compare_c.py` process them\n"
        "- Dataset files: `dataset/dataset_images.csv` and `dataset/dataset_grid.png`\n\n"
        "Use the `*_vs_k.csv` files directly with PGFPlots for curves against "
        "`k`. Use the `*_rd_<method>.csv` files for PSNR vs bpp curves. Use "
        "the `*_bar.csv` files for horizontal timing bars grouped by k.\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
