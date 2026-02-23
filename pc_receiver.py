#!/usr/bin/env python3
"""
IC-JPEG — PC-side receiver for compressed data from ESP32-CAM.

Método B: captura e comprime no ESP32, transmite coeficientes quantizados,
descomprime no PC **usando a mesma libimage em C** (via ctypes).

Resultados são salvos automaticamente em  resultados/<método>/

Modo normal (captura da câmera):
    python pc_receiver.py --ip 10.0.0.196 --method loeffler --quality 2.0
    python pc_receiver.py --ip 10.0.0.196 --method approx --quality 3.0
    python pc_receiver.py --ip 10.0.0.196 --method loeffler --compare
    python pc_receiver.py --ip 10.0.0.196 --all-methods --all-qualities

Modo imagem (envia imagem conhecida → compara ESP32 vs PC):
    python pc_receiver.py --image imgs/monarch.png --method loeffler
    python pc_receiver.py --image imgs/fruits.png --all-methods --all-qualities
"""

import argparse
import sys
import time
import io
from datetime import datetime
from pathlib import Path

import numpy as np
import requests
from PIL import Image

# ---------------------------------------------------------------------------
#  Load the C libimage wrapper (same code that runs on ESP32)
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
RESULTS_DIR = SCRIPT_DIR / "resultados"
sys.path.insert(0, str(SCRIPT_DIR / "libimage" / "python"))

from libimage_wrapper import LibImage  # noqa: E402

_lib = LibImage(SCRIPT_DIR / "libimage" / "bin" / "libimage.so")

# ---------------------------------------------------------------------------
#  Constants from C (zigzag for bitrate estimation)
# ---------------------------------------------------------------------------

# Standard JPEG zigzag scan order (matching metrics.c on ESP32)
# ZIGZAG_SCAN[scan_position] = flat_index in 8×8 block
ZIGZAG_SCAN = np.array([
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
], dtype=np.int32)


# ---------------------------------------------------------------------------
#  Metrics
# ---------------------------------------------------------------------------

def calc_psnr(orig: np.ndarray, recon: np.ndarray) -> float:
    mse = np.mean((orig.astype(np.float64) - recon.astype(np.float64)) ** 2)
    if mse < 1e-10:
        return 100.0
    return 10.0 * np.log10(255.0 ** 2 / mse)


def calc_bitrate(y_q: np.ndarray, cb_q: np.ndarray, cr_q: np.ndarray) -> float:
    """Estimate bitrate (bpp) using last non-zero zigzag position."""
    total_bits = 0.0
    total_blocks = 0
    for channel_blocks in [y_q, cb_q, cr_q]:
        for b in range(channel_blocks.shape[0]):
            block = channel_blocks[b]
            last_nz = -1
            for i in range(63, -1, -1):
                if block[ZIGZAG_SCAN[i]] != 0:
                    last_nz = i
                    break
            if last_nz >= 0:
                total_bits += (last_nz + 1) * 8.0
            total_blocks += 1
    total_pixels = total_blocks * 64
    return total_bits / total_pixels if total_pixels > 0 else 0.0


# ---------------------------------------------------------------------------
#  Fetch compressed data from ESP32
# ---------------------------------------------------------------------------

def fetch_compressed(base_url: str, method: str, quality: float, timeout: int = 30):
    """
    GET /capture_compressed  →  binary body (int16 coefficients) + headers.

    Binary body layout:
        [Y  int16[num_blocks*64]]
        [Cb int16[num_blocks*64]]
        [Cr int16[num_blocks*64]]

    Only bitrate is computed on ESP32.  PSNR is computed on PC after
    decompression.

    Returns dict with:
        width, height, method, quality, num_blocks,
        compress_time_us, bitrate (from ESP32),
        y_quantized, cb_quantized, cr_quantized (N×64 int32),
        transfer_time_s, compressed_bytes
    """
    url = f"{base_url}/capture_compressed?method={method}&quality={quality}"
    print(f"  Fetching {url} …")

    t0 = time.time()
    resp = requests.get(url, timeout=timeout)
    transfer_time = time.time() - t0
    resp.raise_for_status()

    w   = int(resp.headers["X-Width"])
    h   = int(resp.headers["X-Height"])
    m   = resp.headers["X-Method"]
    q   = float(resp.headers["X-Quality"])
    nb  = int(resp.headers["X-Num-Blocks"])
    ct  = int(resp.headers["X-Compress-Time-Us"])
    bitrate = float(resp.headers.get("X-Bitrate", 0))

    data = resp.content
    ch_size_16 = nb * 64 * 2            # bytes per channel (int16)

    expected = 3 * ch_size_16
    if len(data) != expected:
        raise ValueError(
            f"Body size mismatch: got {len(data)}, expected {expected} "
            f"(w={w}, h={h}, nb={nb})"
        )

    # Unpack int16 → int32 (libimage expects int32 arrays)
    off = 0
    y_q  = np.frombuffer(data[off:off+ch_size_16], dtype=np.int16).astype(np.int32).reshape(nb, 64).copy()
    off += ch_size_16
    cb_q = np.frombuffer(data[off:off+ch_size_16], dtype=np.int16).astype(np.int32).reshape(nb, 64).copy()
    off += ch_size_16
    cr_q = np.frombuffer(data[off:off+ch_size_16], dtype=np.int16).astype(np.int32).reshape(nb, 64).copy()

    return {
        "width": w, "height": h, "method": m, "quality": q,
        "num_blocks": nb,
        "compress_time_us": ct, "bitrate": bitrate,
        "y_quantized": y_q, "cb_quantized": cb_q, "cr_quantized": cr_q,
        "transfer_time_s": transfer_time,
        "compressed_bytes": expected,
    }


# ---------------------------------------------------------------------------
#  Decompress on PC using C libimage
# ---------------------------------------------------------------------------

def decompress_on_pc(pkt: dict) -> dict:
    """
    Dequantize + IDCT + YCbCr→RGB using the C library (bit-identical to ESP32).

    Returns dict with:
        recon_rgb (H×W×3 uint8), decompress_time_s (float)
    """
    t0 = time.time()

    recon = _lib.decompress(
        pkt["y_quantized"], pkt["cb_quantized"], pkt["cr_quantized"],
        pkt["width"], pkt["height"],
        pkt["quality"], pkt["method"],
    )

    decompress_time = time.time() - t0
    return {"recon_rgb": recon, "decompress_time_s": decompress_time}


# ---------------------------------------------------------------------------
#  Fetch Method A (on-device BMP) for comparison
# ---------------------------------------------------------------------------

def fetch_method_a(base_url: str, method: str, quality: float, timeout: int = 60):
    """GET /capture → BMP with metrics in headers."""
    url = f"{base_url}/capture?method={method}&quality={quality}"
    print(f"  Fetching {url} …")

    t0 = time.time()
    resp = requests.get(url, timeout=timeout)
    total_time = time.time() - t0
    resp.raise_for_status()

    img = Image.open(io.BytesIO(resp.content)).convert("RGB")

    return {
        "image": np.array(img),
        "psnr":  float(resp.headers.get("X-PSNR", 0)),
        "bitrate": float(resp.headers.get("X-Bitrate", 0)),
        "compress_time_us": int(resp.headers.get("X-Compress-Time-Us", 0)),
        "decompress_time_us": int(resp.headers.get("X-Decompress-Time-Us", 0)),
        "method": resp.headers.get("X-Method", method),
        "quality": float(resp.headers.get("X-Quality", quality)),
        "total_time_s": total_time,
        "bmp_bytes": len(resp.content),
    }


# ---------------------------------------------------------------------------
#  Load image from disk
# ---------------------------------------------------------------------------

def load_image(path: str, width: int = 320, height: int = 240) -> np.ndarray:
    """Load an image, resize to (width, height), return RGB888 uint8 array."""
    img = Image.open(path).convert("RGB").resize((width, height), Image.LANCZOS)
    return np.array(img, dtype=np.uint8)


# ---------------------------------------------------------------------------
#  Send image to ESP32 POST endpoints
# ---------------------------------------------------------------------------

def send_image_process(base_url: str, rgb: np.ndarray,
                       method: str, quality: float,
                       timeout: int = 60) -> dict:
    """
    POST /process — send RGB888, receive BMP (Method A on user image).

    Returns dict with:
        recon_rgb (H×W×3 uint8), psnr, bitrate,
        compress_time_us, decompress_time_us, method, quality,
        total_time_s, bmp_bytes
    """
    h, w = rgb.shape[:2]
    url = (f"{base_url}/process?method={method}&quality={quality}"
           f"&width={w}&height={h}")
    body = np.ascontiguousarray(rgb).tobytes()

    print(f"  POST {url} ({len(body):,} B) …")
    t0 = time.time()
    resp = requests.post(url, data=body,
                         headers={"Content-Type": "application/octet-stream"},
                         timeout=timeout)
    total_time = time.time() - t0
    resp.raise_for_status()

    img = Image.open(io.BytesIO(resp.content)).convert("RGB")

    return {
        "recon_rgb": np.array(img),
        "psnr":  float(resp.headers.get("X-PSNR", 0)),
        "bitrate": float(resp.headers.get("X-Bitrate", 0)),
        "compress_time_us": int(resp.headers.get("X-Compress-Time-Us", 0)),
        "decompress_time_us": int(resp.headers.get("X-Decompress-Time-Us", 0)),
        "method": resp.headers.get("X-Method", method),
        "quality": float(resp.headers.get("X-Quality", quality)),
        "total_time_s": total_time,
        "bmp_bytes": len(resp.content),
    }


def send_image_compressed(base_url: str, rgb: np.ndarray,
                          method: str, quality: float,
                          timeout: int = 60) -> dict:
    """
    POST /process_compressed — send RGB888, receive int16 coefficients.

    Returns dict with same fields as fetch_compressed().
    """
    h, w = rgb.shape[:2]
    url = (f"{base_url}/process_compressed?method={method}&quality={quality}"
           f"&width={w}&height={h}")
    body = np.ascontiguousarray(rgb).tobytes()

    print(f"  POST {url} ({len(body):,} B) …")
    t0 = time.time()
    resp = requests.post(url, data=body,
                         headers={"Content-Type": "application/octet-stream"},
                         timeout=timeout)
    transfer_time = time.time() - t0
    resp.raise_for_status()

    w2  = int(resp.headers["X-Width"])
    h2  = int(resp.headers["X-Height"])
    m   = resp.headers["X-Method"]
    q   = float(resp.headers["X-Quality"])
    nb  = int(resp.headers["X-Num-Blocks"])
    ct  = int(resp.headers["X-Compress-Time-Us"])
    bitrate = float(resp.headers.get("X-Bitrate", 0))

    data = resp.content
    ch_size_16 = nb * 64 * 2

    off = 0
    y_q  = np.frombuffer(data[off:off+ch_size_16], dtype=np.int16).astype(np.int32).reshape(nb, 64).copy()
    off += ch_size_16
    cb_q = np.frombuffer(data[off:off+ch_size_16], dtype=np.int16).astype(np.int32).reshape(nb, 64).copy()
    off += ch_size_16
    cr_q = np.frombuffer(data[off:off+ch_size_16], dtype=np.int16).astype(np.int32).reshape(nb, 64).copy()

    return {
        "width": w2, "height": h2, "method": m, "quality": q,
        "num_blocks": nb,
        "compress_time_us": ct, "bitrate": bitrate,
        "y_quantized": y_q, "cb_quantized": cb_q, "cr_quantized": cr_q,
        "transfer_time_s": transfer_time,
        "compressed_bytes": 3 * ch_size_16,
    }


# ---------------------------------------------------------------------------
#  Process image locally with C libimage (ground truth)
# ---------------------------------------------------------------------------

def process_local(rgb: np.ndarray, method: str, quality: float) -> dict:
    """
    Full compress → decompress on PC using the same C libimage.

    Returns dict with:
        recon_rgb, y_quantized, cb_quantized, cr_quantized,
        psnr, bitrate, compress_time_s, decompress_time_s
    """
    h, w = rgb.shape[:2]

    t0 = time.time()
    comp = _lib.compress(rgb, w, h, quality, method)
    t_compress = time.time() - t0

    t0 = time.time()
    recon = _lib.decompress(
        comp["y_quantized"], comp["cb_quantized"], comp["cr_quantized"],
        w, h, quality, method,
    )
    t_decompress = time.time() - t0

    psnr = calc_psnr(rgb, recon)
    br   = calc_bitrate(comp["y_quantized"], comp["cb_quantized"],
                        comp["cr_quantized"])

    return {
        "recon_rgb": recon,
        "y_quantized":  comp["y_quantized"],
        "cb_quantized": comp["cb_quantized"],
        "cr_quantized": comp["cr_quantized"],
        "psnr": psnr,
        "bitrate": br,
        "compress_time_s": t_compress,
        "decompress_time_s": t_decompress,
    }


# ---------------------------------------------------------------------------
#  Process --image: send to ESP32, process locally, compare
# ---------------------------------------------------------------------------

def process_image_mode(base_url: str, image_path: str,
                       method: str, quality: float, timeout: int):
    """Send a known image to ESP32 + process locally → compare."""

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    rgb = load_image(image_path)
    h, w = rgb.shape[:2]
    img_name = Path(image_path).stem

    print(f"\n  Imagem:    {image_path}")
    print(f"  Dimensões: {w}×{h}")
    print(f"  Método:    {method}")
    print(f"  Qualidade: k={quality}")

    # --- 1. Process locally (ground truth) ---
    print(f"\n▶ Processando localmente (C libimage) …")
    local = process_local(rgb, method, quality)

    # --- 2. Send to ESP32 /process (Method A style) ---
    print(f"\n▶ Enviando para ESP32 /process (Método A) …")
    esp_a = send_image_process(base_url, rgb, method, quality, timeout)

    # --- 3. Send to ESP32 /process_compressed (Method B style) ---
    print(f"\n▶ Enviando para ESP32 /process_compressed (Método B) …")
    esp_b_pkt = send_image_compressed(base_url, rgb, method, quality, timeout)

    t0 = time.time()
    esp_b_recon = _lib.decompress(
        esp_b_pkt["y_quantized"], esp_b_pkt["cb_quantized"],
        esp_b_pkt["cr_quantized"],
        w, h, quality, method,
    )
    esp_b_decompress_pc_s = time.time() - t0

    # Compute PSNR on PC (Method B has no decompress on ESP32)
    esp_b_psnr = calc_psnr(rgb, esp_b_recon)

    # --- 4. Print comparison table ---
    raw_size = w * h * 3
    raw_bpp  = 24.0  # 8 bits × 3 channels, uncompressed

    print()
    print("=" * 85)
    print(f"  COMPARAÇÃO — {method.upper()} k={quality} — {img_name} ({w}×{h})")
    print("=" * 85)

    print(f"\n  {'Métrica':<28} {'Original':>12} {'PC (local)':>12} "
          f"{'ESP32 (A)':>12} {'ESP32 (B)':>12}")
    print(f"  {'-'*28} {'-'*12} {'-'*12} {'-'*12} {'-'*12}")

    print(f"  {'PSNR (dB)':<28} {'∞':>12} {local['psnr']:>12.2f} "
          f"{esp_a['psnr']:>12.2f} {esp_b_psnr:>12.2f}")
    print(f"  {'Bitrate (bpp)':<28} {raw_bpp:>12.3f} {local['bitrate']:>12.3f} "
          f"{esp_a['bitrate']:>12.3f} {esp_b_pkt['bitrate']:>12.3f}")
    ratio_local = raw_bpp / local['bitrate'] if local['bitrate'] > 0 else 0
    ratio_a     = raw_bpp / esp_a['bitrate'] if esp_a['bitrate'] > 0 else 0
    ratio_b     = raw_bpp / esp_b_pkt['bitrate'] if esp_b_pkt['bitrate'] > 0 else 0
    print(f"  {'Razão de compressão':<28} {'1.0:1':>12} "
          f"{ratio_local:>11.1f}:1 {ratio_a:>11.1f}:1 {ratio_b:>11.1f}:1")
    print(f"  {'Payload (bytes)':<28} {raw_size:>12,} "
          f"{'—':>12} "
          f"{esp_a['bmp_bytes']:>12,} "
          f"{esp_b_pkt['compressed_bytes']:>12,}")
    print(f"  {'Compress (s)':<28} {'—':>12} {local['compress_time_s']:>12.4f} "
          f"{esp_a['compress_time_us']/1e6:>12.4f} "
          f"{esp_b_pkt['compress_time_us']/1e6:>12.4f}")
    print(f"  {'Decompress (s)':<28} {'—':>12} {local['decompress_time_s']:>12.4f} "
          f"{esp_a['decompress_time_us']/1e6:>12.4f} "
          f"{esp_b_decompress_pc_s:>12.4f}")

    print("=" * 85)

    # --- 6. Save results ---
    out_dir = RESULTS_DIR / "image_test" / method
    out_dir.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    prefix = f"{img_name}_{method}_q{quality}_{timestamp}"

    Image.fromarray(rgb).save(out_dir / f"{prefix}_original.png")
    Image.fromarray(local["recon_rgb"]).save(out_dir / f"{prefix}_pc_local.png")
    Image.fromarray(esp_a["recon_rgb"]).save(out_dir / f"{prefix}_esp32_A.png")
    Image.fromarray(esp_b_recon).save(out_dir / f"{prefix}_esp32_B_pc.png")

    # Comparison plot
    fig, axes = plt.subplots(1, 4, figsize=(20, 5))
    axes[0].imshow(rgb)
    axes[0].set_title(f"Original\n{raw_bpp:.0f} bpp | {raw_size:,} B", fontsize=9)
    axes[1].imshow(local["recon_rgb"])
    axes[1].set_title(
        f"PC (C libimage)\nPSNR {local['psnr']:.2f} dB | {local['bitrate']:.3f} bpp",
        fontsize=9)
    axes[2].imshow(esp_a["recon_rgb"])
    axes[2].set_title(
        f"ESP32 Método A\nPSNR {esp_a['psnr']:.2f} dB | {esp_a['bitrate']:.3f} bpp",
        fontsize=9)
    axes[3].imshow(esp_b_recon)
    axes[3].set_title(
        f"ESP32 Método B→PC\nPSNR {esp_b_psnr:.2f} dB | {esp_b_pkt['bitrate']:.3f} bpp",
        fontsize=9)
    for ax in axes:
        ax.axis("off")

    fig.suptitle(f"{method.upper()} — k={quality} — {img_name}",
                 fontsize=13, fontweight="bold")
    plt.tight_layout()
    plot_path = out_dir / f"{prefix}_comparacao.png"
    fig.savefig(plot_path, dpi=150, bbox_inches="tight")
    plt.close(fig)

    print(f"\n  ✓ Resultados salvos em: {out_dir}/")
    print(f"  ✓ Comparação: {plot_path}")

# ---------------------------------------------------------------------------

def save_results(pkt: dict, pc_result: dict, method_a: dict = None):
    """Save images and comparison plot to resultados/<method>/."""
    import matplotlib
    matplotlib.use("Agg")  # non-interactive backend — no window needed
    import matplotlib.pyplot as plt

    method = pkt["method"]
    quality = pkt["quality"]
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    # Create output directory
    out_dir = RESULTS_DIR / method
    out_dir.mkdir(parents=True, exist_ok=True)

    recon = pc_result["recon_rgb"]

    # Bitrate from ESP32 (no PSNR — no decompress on ESP32 for Method B)
    br_b = pkt["bitrate"]

    prefix = f"{method}_q{quality}_{timestamp}"

    # Save reconstructed image
    Image.fromarray(recon).save(out_dir / f"{prefix}_metodoB_pc.png")
    print(f"\n  ✓ Reconstruct salvo em: {out_dir / f'{prefix}_metodoB_pc.png'}")

    if method_a is not None:
        Image.fromarray(method_a["image"]).save(
            out_dir / f"{prefix}_metodoA_esp32.png")
        print(f"  ✓ Método A salvo em:    "
              f"{out_dir / f'{prefix}_metodoA_esp32.png'}")

    # Build comparison plot
    if method_a is not None:
        fig, axes = plt.subplots(1, 2, figsize=(10, 5))
        axes[0].imshow(method_a["image"])
        axes[0].set_title(
            f"Método A (ESP32)\n"
            f"PSNR {method_a['psnr']:.2f} dB | {method_a['bitrate']:.3f} bpp",
            fontsize=10)
        axes[1].imshow(recon)
        axes[1].set_title(
            f"Método B (PC/C)\n"
            f"{br_b:.3f} bpp",
            fontsize=10)
        for ax in axes:
            ax.axis("off")
    else:
        fig, axes = plt.subplots(1, 1, figsize=(5, 5))
        axes.imshow(recon)
        axes.set_title(
            f"Método B — {method} q={quality}\n"
            f"{br_b:.3f} bpp",
            fontsize=10)
        axes.axis("off")

    fig.suptitle(f"{method.upper()} — k={quality}", fontsize=13, fontweight="bold")
    plt.tight_layout()

    plot_path = out_dir / f"{prefix}_comparacao.png"
    fig.savefig(plot_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  ✓ Comparação salva em:  {plot_path}")

    return {
        "bitrate_b": br_b,
        "out_dir": out_dir,
        "prefix": prefix,
    }


# ---------------------------------------------------------------------------
#  Print metrics
# ---------------------------------------------------------------------------

def print_metrics(pkt: dict, pc_result: dict, br_b: float,
                  method_a: dict = None):
    print()
    print("=" * 65)
    print(f"  Método B  (compress ESP32 → transmit → decompress PC [C lib])")
    print("-" * 65)
    print(f"  Método:    {pkt['method']}")
    print(f"  Qualidade: k={pkt['quality']}")
    print(f"  Imagem:    {pkt['width']}×{pkt['height']}")
    print(f"  Bitrate:   {br_b:.3f} bpp")
    print(f"  Compress   (ESP32): {pkt['compress_time_us'] / 1e6:.3f} s")
    print(f"  Transfer   (WiFi):  {pkt['transfer_time_s']:.3f} s")
    print(f"  Decompress (PC/C):  {pc_result['decompress_time_s']:.3f} s")
    total_b = (pkt['compress_time_us'] / 1e6 + pkt['transfer_time_s']
               + pc_result['decompress_time_s'])
    print(f"  Latência total:     {total_b:.3f} s")
    print(f"  Payload:            {pkt['compressed_bytes']:,} B "
          f"(3ch × int16)")
    print("=" * 65)

    if method_a is not None:
        print()
        print("=" * 65)
        print(f"  Método A  (tudo no ESP32 → BMP via HTTP)")
        print("-" * 65)
        print(f"  Método:    {method_a['method']}")
        print(f"  Qualidade: k={method_a['quality']}")
        print(f"  PSNR:      {method_a['psnr']:.2f} dB")
        print(f"  Bitrate:   {method_a['bitrate']:.3f} bpp")
        t_enc_a = method_a['compress_time_us'] / 1e6
        t_dec_a = method_a['decompress_time_us'] / 1e6
        t_total_a = method_a['total_time_s']
        t_xfer_a = t_total_a - t_enc_a - t_dec_a
        print(f"  Compress   (ESP32): {t_enc_a:.3f} s")
        print(f"  Decompress (ESP32): {t_dec_a:.3f} s")
        print(f"  Transfer   (WiFi):  {t_xfer_a:.3f} s")
        print(f"  Latência total:     {t_total_a:.3f} s")
        print(f"  BMP size:           {method_a['bmp_bytes']:,} B")
        print("=" * 65)


# ---------------------------------------------------------------------------
#  Process one capture
# ---------------------------------------------------------------------------

def process_one(base_url: str, method: str, quality: float,
                compare: bool, timeout: int):
    """Fetch, decompress, save, print metrics."""
    print(f"\n▶ Método B: {method} q={quality}")
    pkt = fetch_compressed(base_url, method, quality, timeout)
    pc_result = decompress_on_pc(pkt)

    method_a = None
    if compare:
        print(f"\n▶ Método A: {method} q={quality}")
        method_a = fetch_method_a(base_url, method, quality, timeout)

    res = save_results(pkt, pc_result, method_a)
    print_metrics(pkt, pc_result, res["bitrate_b"], method_a)

    return res


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="IC-JPEG — PC receiver (decompression via C libimage)\n"
                    "Resultados salvos em resultados/<método>/\n\n"
                    "Modos:\n"
                    "  (padrão)   Captura da câmera + Método B\n"
                    "  --image    Envia imagem conhecida → ESP32+PC, compara tudo",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--ip", default="10.0.0.196",
                        help="ESP32-CAM IP address (default: 10.0.0.196)")
    parser.add_argument("--method", default="loeffler",
                        choices=["loeffler", "matrix", "approx", "identity"],
                        help="DCT method (default: loeffler)")
    parser.add_argument("--quality", type=float, default=2.0,
                        help="Quality factor k (1.0–8.0, default: 2.0)")
    parser.add_argument("--compare", action="store_true",
                        help="Also fetch Method A (on-device) for comparison")
    parser.add_argument("--all-qualities", action="store_true",
                        help="Run for k = 1.0, 2.0, 3.0, 4.0")
    parser.add_argument("--all-methods", action="store_true",
                        help="Run for loeffler, matrix, approx")
    parser.add_argument("--image", type=str, default=None,
                        help="Path to a known image file to send to ESP32 "
                             "(activates image-test mode)")
    parser.add_argument("--timeout", type=int, default=30,
                        help="HTTP timeout in seconds (default: 30)")
    args = parser.parse_args()

    base_url = f"http://{args.ip}"

    print(f"\n  libimage version: {_lib.version()}")
    print(f"  Resultados → {RESULTS_DIR}/\n")

    methods = ["loeffler", "matrix", "approx", "identity"] if args.all_methods else [args.method]
    qualities = [1.0, 2.0, 3.0, 4.0] if args.all_qualities else [args.quality]

    # ---- Image-test mode: send known image ----
    if args.image:
        if not Path(args.image).exists():
            print(f"  ✗ Arquivo não encontrado: {args.image}")
            sys.exit(1)

        for m in methods:
            for q in qualities:
                try:
                    process_image_mode(base_url, args.image,
                                       m, q, args.timeout)
                except requests.exceptions.RequestException as e:
                    print(f"\n  ✗ Erro de conexão ({m} q={q}): {e}")
                except Exception as e:
                    print(f"\n  ✗ Erro ({m} q={q}): {e}")
                    import traceback
                    traceback.print_exc()

        print(f"\n{'='*85}")
        print(f"  Resultados em: {RESULTS_DIR}/image_test/")
        print(f"{'='*85}\n")
        return

    # ---- Normal mode: capture from camera ----
    for m in methods:
        for q in qualities:
            try:
                process_one(base_url, m, q, args.compare, args.timeout)
            except requests.exceptions.RequestException as e:
                print(f"\n  ✗ Erro de conexão ({m} q={q}): {e}")
            except Exception as e:
                print(f"\n  ✗ Erro ({m} q={q}): {e}")
                import traceback
                traceback.print_exc()

    print(f"\n{'='*65}")
    print(f"  Todos os resultados em: {RESULTS_DIR}/")
    print(f"{'='*65}\n")


if __name__ == "__main__":
    main()
