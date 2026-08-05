#!/usr/bin/env python3
"""Gera três cenas em colunas e os métodos em linhas."""
import csv
from pathlib import Path
from PIL import Image, ImageOps

HERE = Path(__file__).parent
DATA_DIR = HERE / "a_dados_prototipo"
OUT_PATH = HERE.parent / "docs/artigo/paper_csv/embedded/embedded_capture_grid.png"
CSV_PATH = OUT_PATH.with_name("embedded_capture_images.csv")

SELECTED_RUNS = ["run0004", "run0005", "run0009"]
SELECTED_IMAGES = [
    ("original", "ref"),
    ("Loeffler", "loeffler_k0p8"),
    ("Silveira j=7", "silveira_j7_k0p8"),
]

CANVAS_W = 960
CANVAS_H = 720
THUMB_W = 320
THUMB_H = 240

paths = []
for kind, suffix in SELECTED_IMAGES:
    for run_name in SELECTED_RUNS:
        path = DATA_DIR / run_name / f"{run_name}_{suffix}.bmp"
        if not path.exists():
            raise SystemExit(f"Arquivo não encontrado: {path}")
        paths.append((run_name, kind, path))

canvas = Image.new("RGB", (CANVAS_W, CANVAS_H), "white")

for idx, (_, _, img_path) in enumerate(paths):
    row, col = divmod(idx, 3)
    x = col * THUMB_W
    y = row * THUMB_H
    with Image.open(img_path) as img:
        thumb = ImageOps.fit(
            img.convert("RGB"),
            (THUMB_W, THUMB_H),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )
    canvas.paste(thumb, (x, y))

OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
canvas.save(OUT_PATH)

with CSV_PATH.open("w", newline="", encoding="utf-8") as csv_file:
    writer = csv.DictWriter(
        csv_file,
        fieldnames=["order", "run", "kind", "filename", "width", "height", "pixels"],
    )
    writer.writeheader()
    for order, (run_name, kind, img_path) in enumerate(paths, start=1):
        writer.writerow({
            "order": order,
            "run": run_name,
            "kind": kind,
            "filename": img_path.name,
            "width": 320,
            "height": 240,
            "pixels": 320 * 240,
        })

print(f"Saved {OUT_PATH}  ({CANVAS_W}x{CANVAS_H}, {len(paths)} imagens)")
