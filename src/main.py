import os
import math
import time
import numpy as np
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio as psnr, structural_similarity as ssim

# ---------------- CONFIGURAÇÃO --------------
DCT_METHOD = 'loeffler'  # loeffler | matrix

INPUT_FILE = 'src/imagem.jpg'
OUTPUT_DIR = f'resultados_{DCT_METHOD}'
FATORES_K = [0.5, 1.0, 2.0, 5.0, 10.0]
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ----------------- CONSTANTES E TABELAS  -----------------
C1 = math.cos(math.pi / 16.0)
S1 = math.sin(math.pi / 16.0)
C3 = math.cos(3.0 * math.pi / 16.0)
S3 = math.sin(3.0 * math.pi / 16.0)
C6 = math.cos(6.0 * math.pi / 16.0)
S6 = math.sin(6.0 * math.pi / 16.0)

SQRT_2 = math.sqrt(2.0)

""" Q50_LUMA = np.ones((8,8))
Q50_CHROMA = np.ones((8,8)) """

Q50_LUMA = np.array([
    [16, 11, 10, 16, 24, 40, 51, 61], [12, 12, 14, 19, 26, 58, 60, 55],
    [14, 13, 16, 24, 40, 57, 69, 56], [14, 17, 22, 29, 51, 87, 80, 62],
    [18, 22, 37, 56, 68, 109, 103, 77], [24, 35, 55, 64, 81, 104, 113, 92],
    [49, 64, 78, 87, 103, 121, 120, 101], [72, 92, 95, 98, 112, 100, 103, 99]
], dtype=np.float64)

Q50_CHROMA = np.array([
    [17, 18, 24, 47, 99, 99, 99, 99], [18, 21, 26, 66, 99, 99, 99, 99],
    [24, 26, 56, 99, 99, 99, 99, 99], [47, 66, 99, 99, 99, 99, 99, 99],
    [99, 99, 99, 99, 99, 99, 99, 99], [99, 99, 99, 99, 99, 99, 99, 99],
    [99, 99, 99, 99, 99, 99, 99, 99], [99, 99, 99, 99, 99, 99, 99, 99]
], dtype=np.float64)


# ----------------- IMPLEMENTAÇÃO DO ALGORITMO DE LOEFFLER -----------------
def dct_loeffler_1d(x_in):
    x = np.asarray(x_in, dtype=np.float64).flatten()
    s07, d07 = x[0] + x[7], x[0] - x[7]
    s16, d16 = x[1] + x[6], x[1] - x[6]
    s25, d25 = x[2] + x[5], x[2] - x[5]
    s34, d34 = x[3] + x[4], x[3] - x[4]

    p0, p3 = s07 + s34, s07 - s34
    p1, p2 = s16 + s25, s16 - s25

    Z0 = (p0 + p1) / SQRT_2
    Z4 = (p0 - p1) / SQRT_2
    Z2 = C6 * p2 + S6 * p3
    Z6 = -S6 * p2 + C6 * p3

    y0, y1 = d07 + d34, d16 + d25
    y2, y3 = d16 - d25, d07 - d34

    Z1 = (C3 * y0 + C1 * y1 + S1 * y2 + S3 * y3) / SQRT_2
    Z3 = (S1 * y0 - C3 * y1 + S3 * y2 + C1 * y3) / SQRT_2
    Z5 = (C1 * y0 - S3 * y1 - C3 * y2 - S1 * y3) / SQRT_2
    Z7 = (-S3 * y0 + S1 * y1 - C1 * y2 + C3 * y3) / SQRT_2

    return np.array([Z0, Z1, Z2, Z3, Z4, Z5, Z6, Z7]) / 2.0


def idct_loeffler_1d(X_in):
    Z = np.asarray(X_in, dtype=np.float64).flatten() * 2.0
    Z0, Z1, Z2, Z3, Z4, Z5, Z6, Z7 = Z
    t0, t4 = Z0 * SQRT_2, Z4 * SQRT_2
    p0 = 0.5 * (t0 + t4)
    p1 = 0.5 * (t0 - t4)
    p2 = C6 * Z2 - S6 * Z6
    p3 = S6 * Z2 + C6 * Z6
    s07 = 0.5 * (p0 + p3)
    s34 = 0.5 * (p0 - p3)
    s16 = 0.5 * (p1 + p2)
    s25 = 0.5 * (p1 - p2)
    w1, w3, w5, w7 = Z1, Z3, Z5, Z7
    y0 = (C3 * w1 + S1 * w3 + C1 * w5 - S3 * w7) / SQRT_2
    y1 = (C1 * w1 - C3 * w3 - S3 * w5 + S1 * w7) / SQRT_2
    y2 = (S1 * w1 + S3 * w3 - C3 * w5 - C1 * w7) / SQRT_2
    y3 = (S3 * w1 + C1 * w3 - S1 * w5 + C3 * w7) / SQRT_2
    d07 = 0.5 * (y0 + y3)
    d34 = 0.5 * (y0 - y3)
    d16 = 0.5 * (y1 + y2)
    d25 = 0.5 * (y1 - y2)
    x0, x7 = 0.5 * (s07 + d07), 0.5 * (s07 - d07)
    x1, x6 = 0.5 * (s16 + d16), 0.5 * (s16 - d16)
    x2, x5 = 0.5 * (s25 + d25), 0.5 * (s25 - d25)
    x3, x4 = 0.5 * (s34 + d34), 0.5 * (s34 - d34)
    return np.array([x0, x1, x2, x3, x4, x5, x6, x7])


# ----------------- IMPLEMENTAÇÃO DCT MATRICIAL -----------------
def dct_matrix_1d(x):
    N = 8
    X = np.zeros(N, dtype=np.float64)
    for k in range(N):
        alpha = 1.0 / SQRT_2 if k == 0 else 1.0
        soma = sum(x[n] * math.cos(math.pi * k * (2 * n + 1) / (2 * N)) for n in range(N))
        X[k] = alpha * soma
    return X * math.sqrt(2.0 / N)


def idct_matrix_1d(X):
    N = 8
    x = np.zeros(N, dtype=np.float64)
    for n in range(N):
        soma = 0.0
        for k in range(N):
            alpha = 1.0 / SQRT_2 if k == 0 else 1.0
            soma += alpha * X[k] * math.cos(math.pi * k * (2 * n + 1) / (2 * N))
        x[n] = soma
    return x * math.sqrt(2.0 / N)


# ----------------- SELEÇÃO DO MÉTODO -----------------
if DCT_METHOD == 'loeffler':
    dct_1d, idct_1d = dct_loeffler_1d, idct_loeffler_1d
elif DCT_METHOD == 'matrix':
    dct_1d, idct_1d = dct_matrix_1d, idct_matrix_1d
else:
    raise ValueError("Método deve ser 'loeffler' ou 'matrix'")


# ----------------- DCT/IDCT 2D -----------------
def dct_2d(bloco):
    tmp = np.array([dct_1d(row) for row in bloco])
    return np.array([dct_1d(col) for col in tmp.T]).T


def idct_2d(dct_bloco):
    tmp = np.array([idct_1d(row) for row in dct_bloco])
    return np.array([idct_1d(col) for col in tmp.T]).T


# ----------------- PIPELINE DE PROCESSAMENTO -----------------
def dividir_blocos(canal):
    h, w = canal.shape
    pad_h = (8 - h % 8) % 8
    pad_w = (8 - w % 8) % 8
    padded_canal = np.pad(canal, ((0, pad_h), (0, pad_w)), mode='edge')
    H, W = padded_canal.shape
    blocos = padded_canal.reshape(H // 8, 8, W // 8, 8).swapaxes(1, 2).reshape(-1, 8, 8)
    return blocos, (H, W), (h, w)


def juntar_blocos(blocos, padded_shape, original_shape):
    H, W = padded_shape
    h, w = original_shape
    canal_completo = blocos.reshape(H // 8, W // 8, 8, 8).swapaxes(1, 2).reshape(H, W)
    return canal_completo[:h, :w]


def clamp(arr):
    return np.clip(arr, 0, 255).astype(np.uint8)


def processar_canal(canal, qtable, k):
    canal_shifted = canal.astype(np.float64) - 128.0
    blocos, padded_shape, original_shape = dividir_blocos(canal_shifted)
    qtable_scaled = qtable * float(k)
    blocos_processados = []
    for bloco in blocos:
        dct_bloco = dct_2d(bloco)
        q_bloco = np.round(dct_bloco / qtable_scaled)
        deq_bloco = q_bloco * qtable_scaled
        idct_bloco = idct_2d(deq_bloco)
        blocos_processados.append(idct_bloco)
    canal_reconstruido = juntar_blocos(np.array(blocos_processados), padded_shape, original_shape) + 128.0
    return canal_reconstruido


# ----------------- CONVERSÃO DE COR -----------------
def rgb_to_ycbcr(r, g, b):
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128.0 + (-0.168736 * r - 0.331264 * g + 0.5 * b)
    cr = 128.0 + (0.5 * r - 0.418688 * g - 0.081312 * b)
    return y, cb, cr


def ycbcr_to_rgb(y, cb, cr):
    r = y + 1.402 * (cr - 128.0)
    g = y - 0.344136 * (cb - 128.0) - 0.714136 * (cr - 128.0)
    b = y + 1.772 * (cb - 128.0)
    return clamp(np.stack([r, g, b], axis=-1))


def calcular_metricas(original, reconstruida):
    psnr_val = psnr(original, reconstruida, data_range=255)
    ssim_val = ssim(original, reconstruida, data_range=255, channel_axis=-1, win_size=7)
    return psnr_val, ssim_val


# ----------------- MAIN -----------------
def main():
    try:
        img_original = Image.open(INPUT_FILE).convert('RGB')
        img_rgb_original = np.array(img_original, dtype=np.uint8)
        print(f"Usando método DCT: '{DCT_METHOD}'")
        print(f"Imagem '{INPUT_FILE}' ({img_rgb_original.shape[1]}x{img_rgb_original.shape[0]}) carregada.")
        print("Iniciando pipeline de compressão com fatores k...")
    except FileNotFoundError:
        print(f"\nERRO: Arquivo '{INPUT_FILE}' não encontrado.")
        return

    r, g, b = img_rgb_original[:, :, 0], img_rgb_original[:, :, 1], img_rgb_original[:, :, 2]
    y, cb, cr = rgb_to_ycbcr(r, g, b)

    resultados = []
    tempo_total_processamento = 0

    for k in FATORES_K:
        print(f"Processando com fator k = {k}...")
        start_time = time.perf_counter()

        y_reconstruido = processar_canal(y, Q50_LUMA, k)
        cb_reconstruido = processar_canal(cb, Q50_CHROMA, k)
        cr_reconstruido = processar_canal(cr, Q50_CHROMA, k)

        end_time = time.perf_counter()
        tempo_decorrido = (end_time - start_time) * 1000
        tempo_total_processamento += tempo_decorrido

        img_rgb_reconstruida = ycbcr_to_rgb(y_reconstruido, cb_reconstruido, cr_reconstruido)

        nome_saida = os.path.join(OUTPUT_DIR, f'resultado_k={k}.png')
        Image.fromarray(img_rgb_reconstruida).save(nome_saida)

        psnr_val, ssim_val = calcular_metricas(img_rgb_original, img_rgb_reconstruida)
        resultados.append((k, psnr_val, ssim_val, tempo_decorrido))

    print("\n--- RESULTADOS FINAIS ---")
    print("┌─────────┬───────────┬─────────┬──────────────┐")
    print("│ Fator k │ PSNR (dB) │  SSIM   │ Tempo (ms)   │")
    print("├─────────┼───────────┼─────────┼──────────────┤")
    for k, p, s, t in resultados:
        print(f"│ {k:>6.1f}  │  {p:7.2f}  │ {s:.4f}  │ {t:>12.2f} │")
    print("└─────────┴───────────┴─────────┴──────────────┘")
    print(f"\nTempo total de processamento: {tempo_total_processamento:.2f} ms")
    print(f"Processamento concluído. Imagens salvas em '{OUTPUT_DIR}/'")


if __name__ == '__main__':
    main()
