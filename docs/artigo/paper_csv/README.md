# Paper CSV exports

## Prism directory layout

The paths in `IEEE-conference-template-062824.tex` assume:

```text
IEEE-conference-template-062824.tex
paper_csv/
  c/
  python/
  embedded/
  desktop/
  dataset/
imgs/
```

Therefore, paths must be relative to the `.tex` file:

- CSV files: `paper_csv/c/...`, `paper_csv/python/...`, or
  `paper_csv/embedded/...`.
- Generated capture grid: `paper_csv/embedded/embedded_capture_grid.png`.
- External article images: `imgs/<filename>`.
- Do not use `../paper_csv/...`.

## Data sources

- C source: `experiments/resultados/summary_table.csv`.
- Python source: `experiments/results/comparison/summary_table_python.csv`.
- Embedded source: `experiments/a_dados_prototipo`.
- Selected methods: Loeffler, Matrix, RDCT, Silveira j=3, and Silveira j=7.
- Desktop dataset: eight images listed in `dataset/dataset_images.csv`.
- Embedded dataset: six complete ALL runs and 120 compressed records.

Python is used only to validate algebraic consistency and reconstruction
quality. The C implementation supplies bitrate, rate-distortion, and desktop
runtime results.

## Figures in the article

### 1. Reconstruction quality: C and Python

Label: `fig:c_quality`.

Two panels show PSNR and SSIM against the quantization factor `k`. Thick
marked curves are the C implementation and thin curves are the Python
reference. The near overlap validates the C implementation, while the
separation between methods shows the reconstruction-quality trade-off.

### 2. Rate-distortion and bitrate: C

Label: `fig:rd`.

The first panel plots PSNR against bpp and shows how much reconstruction
quality each method obtains for its transmitted size. The second panel shows
bpp against `k`, making the effect of stronger quantization explicit. Together
they support the compression-efficiency discussion.

### 3. Desktop computational cost: C

Label: `fig:time`.

Horizontal bars compare compression and decompression time for every method
and `k`. This figure demonstrates the lower full-codec runtime of the
approximate transforms and the higher cost of the direct Matrix DCT.

### 4. Embedded capture set

Label: `fig:embedded_captures`.

The image grid shows the six real camera inputs used by the embedded
experiments. It establishes that the prototype results come from multiple
real captures rather than a single desktop image.

### 5. Embedded reconstruction quality

Label: `fig:embedded_quality`.

Three panels show mean PSNR, SSIM, and bpp against `k` over the six ALL runs.
The first two verify whether the quality ordering observed on desktop is
preserved on the ESP32-CAM and ESP32-S3 prototype. The bpp panel connects
that quality to transmitted image size and makes the rate-quality trade-off
visible in the same figure.

### 6. Embedded computational cost

Label: `fig:embedded_time_compute`.

Horizontal bars compare compression time on the ESP32-CAM and decompression
time on the ESP32-S3. They show the real prototype cost of the complete codec
and explain why the isolated DCT/IDCT advantage is smaller after fixed
pipeline stages are included.

### 7. Embedded DCT/IDCT and pipeline decomposition

Label: `fig:embedded_time_breakdown`.

Stacked horizontal bars divide mean compression and decompression time into
the DCT/IDCT computation and all remaining codec stages. The nearly equal
gray segments reveal the fixed pipeline cost, while the blue segments expose
the large arithmetic difference between exact and approximate transforms.

## CSV naming

- `*_vs_k.csv`: curves against `k`.
- `*_rd_<method>.csv`: rate-distortion curves.
- `*_bar.csv`: horizontal bars grouped by `k`.
- `*_summary_selected.csv`: compact method/`k` summaries.
- `embedded_scene_records.csv`: all 120 embedded records used in the means.
