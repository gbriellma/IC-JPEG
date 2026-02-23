# IC-JPEG — DCT Image Compression Library & Analysis

[![C](https://img.shields.io/badge/C-portable-blue.svg)]()
[![Python 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)](https://www.python.org/downloads/)
[![ESP-IDF 5.5](https://img.shields.io/badge/ESP--IDF-5.5-red.svg)](https://docs.espressif.com/projects/esp-idf/)

A portable C library for DCT-based image compression (JPEG-like pipeline), with four DCT implementations for comparative analysis. Includes a bit-identical Python reimplementation for research, and example firmware tested on ESP32-CAM.

> **Note:** The `libimage` library is **generic and portable** — it compiles on any platform with a C compiler. The ESP32-CAM is just the platform where it was tested in an embedded environment.

---

## 📂 Project Structure

```
.
├── libimage/                  # 📦 Portable C library
│   ├── include/
│   │   ├── jpeg_codec.h       #   Public API
│   │   └── internal.h         #   Constants (auto-detects ESP_PLATFORM)
│   ├── src/
│   │   ├── codec.c            #   Compress/decompress pipeline
│   │   ├── colorspace.c       #   RGB ↔ YCbCr (BT.601)
│   │   ├── dct_loeffler.c     #   Loeffler DCT (11 mults, IDCT deferred-division)
│   │   ├── dct_matrix.c       #   Matrix DCT (64 mults, reference)
│   │   ├── dct_approx.c       #   Cintra-Bayer 2011 (0 mults)
│   │   ├── dct_identity.c     #   Identity (passthrough)
│   │   ├── quantization.c     #   Q50 tables & quantize/dequantize
│   │   └── utils.c            #   8×8 block utilities
│   ├── example/               #   Usage examples (process_images.c)
│   ├── bin/                   #   Build artifacts (.a, .so)
│   ├── Makefile               #   PC build (libimage.a + libimage.so)
│   └── CMakeLists.txt         #   ESP-IDF build (component)
│
├── src_py/                    # 🐍 Python implementation (research/analysis)
│   ├── constantes.py          #   Constants identical to C (SCALE=2²⁰)
│   ├── dct.py                 #   4 DCTs in pure Python (bit-identical to C)
│   ├── pipeline.py            #   Compress/decompress pipeline
│   ├── main.py                #   Batch processing
│   └── plots.py               #   Metrics and plots
│
├── src/                       # 📡 ESP32-CAM firmware (tested platform)
│   ├── main.c, webserver.c    #   See src/README.md for details
│   ├── wifi.c, metrics.c      #
│   └── README.md              #   Firmware documentation
│
├── pc_receiver.py             # 🖥️ PC receiver (ESP32 capture + --image mode)
├── compare_methods.py         # 📊 3 DCT methods comparison (Python)
├── platformio.ini             # PlatformIO config (ESP32-CAM)
└── requirements.txt           # Python dependencies
```

---

## 📦 C Library — `libimage`

### Features

| | |
|---|---|
| **Language** | C, zero external dependencies |
| **Platforms** | PC (Linux/macOS/Windows), ESP32, any C-compatible embedded system |
| **Arithmetic** | Fixed-point integer, SCALE = 2²⁰ = 1,048,576, `int64_t` intermediaries |
| **DCT Methods** | Loeffler (11 mults), Matrix (64 mults), Approx (0 mults), Identity |
| **PC Build** | `make all` → `bin/libimage.a` + `bin/libimage.so` |
| **ESP-IDF Build** | Component via `CMakeLists.txt` (auto-detects `ESP_PLATFORM`) |

### API

```c
#include "jpeg_codec.h"

// Configure parameters
jpeg_params_t params = {
    .quality_factor = 2.0,              // 1.0 = high quality, 8.0 = low
    .dct_method     = JPEG_DCT_LOEFFLER,// MATRIX, APPROX, IDENTITY
    .skip_quantization = 0
};

// Compress
jpeg_compressed_t *comp = NULL;
jpeg_compress(&image, &params, &comp);

// Decompress
jpeg_image_t *recon = NULL;
jpeg_decompress(comp, &recon);

// Cleanup
jpeg_free_compressed(comp);
jpeg_free_image(recon);
```

### DCT Methods

| Enum | Method | Multiplications | Accuracy | Reference |
|------|--------|:--------------:|----------|-----------|
| `JPEG_DCT_LOEFFLER` | Loeffler 1989 | 11 / 1D | Exact | Loeffler et al. ICASSP 1989 |
| `JPEG_DCT_MATRIX` | Direct DCT-II | 64 / 1D | Exact (reference) | — |
| `JPEG_DCT_APPROX` | Cintra-Bayer | 0 / 1D | Approximate | Cintra & Bayer, IEEE SPL 2011 |
| `JPEG_DCT_IDENTITY` | Passthrough | 0 | N/A | — |

### Embedded Portability

The code automatically detects the platform via `#ifdef ESP_PLATFORM`:

| Feature | PC (`calloc`) | ESP32 (`ESP_PLATFORM`) |
|---------|---------------|------------------------|
| Allocation | `calloc()` | `heap_caps_calloc()` (PSRAM) |
| Watchdog | None | `vTaskDelay()` every N blocks |
| Build | Makefile | CMakeLists.txt (ESP-IDF component) |

To port to another embedded platform, just add the `src/` and `include/` files to your build system.

### Build

```bash
cd libimage
make all          # libimage.a + libimage.so
make test         # bin/test_validation
```

---

## 🐍 Python Implementation — `src_py/`

**Bit-identical** reimplementation of the C code, in pure Python with integer arithmetic.
All fixed-point constants (SCALE = 2²⁰), Q50 tables, and DCT algorithms replicate
*exactly* the C code, including C-style truncated division and `div_round`.

**Verified:** Python vs C produce **0 differences** across 1,179,648 pixels (monarch 320×240, k=2.0).

```bash
pip install -r requirements.txt
cd src_py
python main.py --method loeffler           # Uses C libimage via ctypes (default)
python main.py --method loeffler --pure-python  # Uses pure Python
python ../compare_methods.py               # Compares Loeffler vs Matrix vs Approx
```

---

## 🔬 DCT Methods Comparison

> **Results obtained via Python implementation** (`compare_methods.py` and `src_py/main.py`),
> using integer arithmetic identical to C with SCALE = 2²⁰.

### Performance Characteristics

| Method | PSNR | Complexity | Accuracy | Notes |
|--------|------|:----------:|----------|-------|
| **Loeffler** | ⭐⭐⭐⭐⭐ | 11 mults | Exact | Best cost-benefit |
| **Matrix** | ⭐⭐⭐⭐⭐ | 64 mults | Exact (ref.) | Slower, same quality |
| **Approximate** | ⭐⭐⭐⭐ | 0 mults | Approx. | ~1.5–2 dB below exact |
| **Identity** | N/A | 0 | N/A | Baseline (no transform) |

### Verified Equivalence — Loeffler ≡ Matrix

With SCALE = 2²⁰ precision, both exact methods produce:

- **0 differences** in quantized coefficients (tested on 1,179,648 coefficients, k=1, 2, 4)
- **Delta PSNR ≈ 0.0000 dB** in end-to-end reconstruction
- **≤ 221 pixel diffs** in 1.18M (0.019%), all ±1 from IDCT rounding
- **Python ≡ C**: 0 differences in full pipeline (same image, same k)

### Implementation Details

- **Loeffler (1989)**: Butterfly structure, 11 mults per 1D-DCT. IDCT uses *deferred-division* (zero intermediate divisions on the even path, one on the odd path, final `div_round` per output).
- **Matrix**: Direct formula `X[k] = c(k) · Σ x[n]·cos(πk(2n+1)/16)`. One sum of products with `div_round` per output.
- **Approximate (Cintra-Bayer 2011)**: T matrix with values {-1, 0, 1}. Zero multiplications. Norm correction in quantization tables.
- **Identity**: `memcpy` passthrough. Skips quantization. Isolates RGB↔YCbCr error (≈ 43.9 dB).

---

## 📡 Benchmarks — PC vs ESP32-CAM

> Image: **monarch** (320×240), k=2.0. ESP32-CAM AI Thinker (240 MHz, 8 MB PSRAM).
> PC: Intel, Linux. Results via `pc_receiver.py --image`.
> Detailed firmware in [`src/README.md`](src/README.md).

| Method | PSNR (dB) | Bitrate (bpp) | Compression | PC compress | ESP32 compress | PC decompress | ESP32 decompress |
|--------|:---------:|:-------------:|:-----------:|:-----------:|:--------------:|:-------------:|:----------------:|
| **Loeffler** | 27.88 | 0.713 | 33.7:1 | 5.4 ms | 2.582 s | 2.0 ms | 1.567 s |
| **Matrix** | 27.88 | 0.713 | 33.7:1 | 6.5 ms | 2.841 s | 2.4 ms | 1.892 s |
| **Approx** | 26.09 | 0.743 | 32.3:1 | 3.0 ms | 2.189 s | 1.1 ms | 1.144 s |
| **Identity** | 43.89 | 7.998 | 3.0:1 | 1.8 ms | 2.089 s | 0.7 ms | 1.010 s |

**Notes:**
- Loeffler and Matrix produce **identical PSNR** (27.88 dB) — equal quantized coefficients
- Approx is **~1.8 dB below** exact methods, but faster on ESP32
- Identity confirms minimal pipeline error (43.89 dB = RGB↔YCbCr conversion only)
- **Method B** (coefficients transmitted, decompress on PC): decompress in ~0.002s instead of ~1.5s

---

## 🔧 Compression Pipeline

```
RGB → YCbCr (BT.601) → 8×8 Blocks → 2D DCT → Quantization (Q50 × k)
                                                       ↓
RGB ← YCbCr → Merge blocks ← 2D IDCT ← Dequantization ←┘
```

All methods share:
- Fixed-point integer arithmetic (SCALE = 2²⁰)
- Standard JPEG Q50 quantization tables (Wallace 1992)
- Same pipeline: YCbCr → 8×8 blocks → DCT → Quantization → IDCT → RGB

---

## 📚 References

1. **Loeffler, C., Ligtenberg, A., & Moschytz, G. S. (1989)**
   "Practical fast 1-D DCT algorithms with 11 multiplications"
   *Proc. ICASSP*

2. **Wallace, G. K. (1992)**
   "The JPEG still picture compression standard"
   *IEEE Trans. Consumer Electronics, 38(1)*

3. **Cintra, R. J., & Bayer, F. M. (2011)**
   "A DCT approximation for image compression"
   *IEEE Signal Processing Letters, 18(10), 579-583*

## 📄 License

MIT License — see [LICENSE](LICENSE).

---

**Comparative analysis of DCT algorithms in image compression — portable library for any system, tested on ESP32-CAM.**

---
---

### 🇧🇷 Versão em Português

# IC-JPEG — Biblioteca e Análise de Compressão de Imagens com DCT

[![C](https://img.shields.io/badge/C-portable-blue.svg)]()
[![Python 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)](https://www.python.org/downloads/)
[![ESP-IDF 5.5](https://img.shields.io/badge/ESP--IDF-5.5-red.svg)](https://docs.espressif.com/projects/esp-idf/)

Biblioteca C portátil para compressão de imagens baseada em DCT (pipeline similar ao JPEG), com quatro implementações de DCT para análise comparativa. Inclui reimplementação Python bit-idêntica para pesquisa, e firmware de exemplo testado em ESP32-CAM.

> **Nota:** A biblioteca `libimage` é **genérica e portátil** — compila em qualquer plataforma com compilador C. O ESP32-CAM é apenas a plataforma onde foi testada em ambiente embarcado.

---

## 📂 Estrutura do Projeto

```
.
├── libimage/                  # 📦 Biblioteca C portátil
│   ├── include/
│   │   ├── jpeg_codec.h       #   API pública
│   │   └── internal.h         #   Constantes (auto-detecta ESP_PLATFORM)
│   ├── src/
│   │   ├── codec.c            #   Pipeline compress/decompress
│   │   ├── colorspace.c       #   RGB ↔ YCbCr (BT.601)
│   │   ├── dct_loeffler.c     #   Loeffler DCT (11 mults, IDCT deferred-division)
│   │   ├── dct_matrix.c       #   Matrix DCT (64 mults, referência)
│   │   ├── dct_approx.c       #   Cintra-Bayer 2011 (0 mults)
│   │   ├── dct_identity.c     #   Identidade (passthrough)
│   │   ├── quantization.c     #   Tabelas Q50 & quantize/dequantize
│   │   └── utils.c            #   Utilitários de blocos 8×8
│   ├── example/               #   Exemplos de uso (process_images.c)
│   ├── bin/                   #   Artefatos compilados (.a, .so)
│   ├── Makefile               #   Build PC (libimage.a + libimage.so)
│   └── CMakeLists.txt         #   Build ESP-IDF (componente)
│
├── src_py/                    # 🐍 Implementação Python (pesquisa/análise)
│   ├── constantes.py          #   Constantes idênticas ao C (SCALE=2²⁰)
│   ├── dct.py                 #   4 DCTs em Python puro (bit-idênticas ao C)
│   ├── pipeline.py            #   Pipeline compress/decompress
│   ├── main.py                #   Processamento em lote
│   └── plots.py               #   Métricas e gráficos
│
├── src/                       # 📡 Firmware ESP32-CAM (plataforma testada)
│   ├── main.c, webserver.c    #   Ver src/README.md para detalhes
│   ├── wifi.c, metrics.c      #
│   └── README.md              #   Documentação do firmware
│
├── pc_receiver.py             # 🖥️ Receptor PC (captura ESP32 + modo --image)
├── compare_methods.py         # 📊 Comparação dos 3 métodos DCT (Python)
├── platformio.ini             # Config PlatformIO (ESP32-CAM)
└── requirements.txt           # Dependências Python
```

---

## 📦 Biblioteca C — `libimage`

### Características

| | |
|---|---|
| **Linguagem** | C, zero dependências externas |
| **Plataformas** | PC (Linux/macOS/Windows), ESP32, qualquer embarcado com compilador C |
| **Aritmética** | Ponto fixo inteiro, SCALE = 2²⁰ = 1.048.576, intermediários `int64_t` |
| **Métodos DCT** | Loeffler (11 mults), Matrix (64 mults), Approx (0 mults), Identity |
| **Build PC** | `make all` → `bin/libimage.a` + `bin/libimage.so` |
| **Build ESP-IDF** | Componente via `CMakeLists.txt` (auto-detecta `ESP_PLATFORM`) |

### API

```c
#include "jpeg_codec.h"

// Configurar parâmetros
jpeg_params_t params = {
    .quality_factor = 2.0,              // 1.0 = alta qualidade, 8.0 = baixa
    .dct_method     = JPEG_DCT_LOEFFLER,// MATRIX, APPROX, IDENTITY
    .skip_quantization = 0
};

// Comprimir
jpeg_compressed_t *comp = NULL;
jpeg_compress(&image, &params, &comp);

// Descomprimir
jpeg_image_t *recon = NULL;
jpeg_decompress(comp, &recon);

// Limpar
jpeg_free_compressed(comp);
jpeg_free_image(recon);
```

### Métodos DCT

| Enum | Método | Multiplicações | Precisão | Referência |
|------|--------|:--------------:|----------|------------|
| `JPEG_DCT_LOEFFLER` | Loeffler 1989 | 11 / 1D | Exata | Loeffler et al. ICASSP 1989 |
| `JPEG_DCT_MATRIX` | DCT-II direta | 64 / 1D | Exata (referência) | — |
| `JPEG_DCT_APPROX` | Cintra-Bayer | 0 / 1D | Aproximada | Cintra & Bayer, IEEE SPL 2011 |
| `JPEG_DCT_IDENTITY` | Passthrough | 0 | N/A | — |

### Portabilidade Embarcada

O código detecta automaticamente a plataforma via `#ifdef ESP_PLATFORM`:

| Recurso | PC (`calloc`) | ESP32 (`ESP_PLATFORM`) |
|---------|---------------|------------------------|
| Alocação | `calloc()` | `heap_caps_calloc()` (PSRAM) |
| Watchdog | Nenhum | `vTaskDelay()` a cada N blocos |
| Build | Makefile | CMakeLists.txt (componente ESP-IDF) |

Para portar para outra plataforma embarcada, basta adicionar os arquivos `src/` e `include/` ao seu build system.

### Compilar

```bash
cd libimage
make all          # libimage.a + libimage.so
make test         # bin/test_validation
```

---

## 🐍 Implementação Python — `src_py/`

Reimplementação **bit-idêntica** ao código C, em Python puro com aritmética inteira.
Todas as constantes de ponto fixo (SCALE = 2²⁰), tabelas Q50, e algoritmos DCT replicam
*exatamente* o código C, incluindo divisão truncada C-style e `div_round`.

**Verificado:** Python vs C produzem **0 diferenças** em 1.179.648 pixels (monarch 320×240, k=2.0).

```bash
pip install -r requirements.txt
cd src_py
python main.py --method loeffler           # Usa C libimage via ctypes (padrão)
python main.py --method loeffler --pure-python  # Usa Python puro
python ../compare_methods.py               # Compara Loeffler vs Matrix vs Approx
```

---

## 🔬 Comparação dos Métodos DCT

> **Resultados obtidos via implementação Python** (`compare_methods.py` e `src_py/main.py`),
> usando aritmética inteira idêntica ao C com SCALE = 2²⁰.

### Características de Performance

| Método | PSNR | Complexidade | Precisão | Observação |
|--------|------|:------------:|----------|------------|
| **Loeffler** | ⭐⭐⭐⭐⭐ | 11 mults | Exata | Melhor custo-benefício |
| **Matricial** | ⭐⭐⭐⭐⭐ | 64 mults | Exata (ref.) | Mais lenta, mesma qualidade |
| **Aproximada** | ⭐⭐⭐⭐ | 0 mults | Aprox. | ~1.5–2 dB menor que exatas |
| **Identidade** | N/A | 0 | N/A | Baseline (sem transformada) |

### Equivalência Verificada — Loeffler ≡ Matrix

Com precisão SCALE = 2²⁰, ambos os métodos exatos produzem:

- **0 diferenças** nos coeficientes quantizados (testado em 1.179.648 coeficientes, k=1, 2, 4)
- **Delta PSNR ≈ 0.0000 dB** na reconstrução end-to-end
- **≤ 221 diffs de pixel** em 1.18M (0,019%), todos ±1 do arredondamento da IDCT
- **Python ≡ C**: 0 diferenças em pipeline completo (mesma imagem, mesmo k)

### Detalhes de Implementação

- **Loeffler (1989)**: Estrutura butterfly, 11 mults por 1D-DCT. IDCT usa *deferred-division* (zero divisões intermediárias no caminho par, uma no ímpar, `div_round` final por saída).
- **Matricial**: Fórmula direta `X[k] = c(k) · Σ x[n]·cos(πk(2n+1)/16)`. Uma soma de produtos com `div_round` por saída.
- **Aproximada (Cintra-Bayer 2011)**: Matriz T com valores {-1, 0, 1}. Zero multiplicações. Correção de norma nas tabelas de quantização.
- **Identidade**: `memcpy` passthrough. Pula quantização. Isola erro RGB↔YCbCr (≈ 43.9 dB).

---

## 📡 Benchmarks — PC vs ESP32-CAM

> Imagem: **monarch** (320×240), k=2.0. ESP32-CAM AI Thinker (240 MHz, 8 MB PSRAM).
> PC: Intel, Linux. Resultados via `pc_receiver.py --image`.
> Firmware detalhado em [`src/README.md`](src/README.md).

| Método | PSNR (dB) | Bitrate (bpp) | Compressão | PC compress | ESP32 compress | PC decompress | ESP32 decompress |
|--------|:---------:|:-------------:|:----------:|:-----------:|:--------------:|:-------------:|:----------------:|
| **Loeffler** | 27.88 | 0.713 | 33.7:1 | 5.4 ms | 2.582 s | 2.0 ms | 1.567 s |
| **Matrix** | 27.88 | 0.713 | 33.7:1 | 6.5 ms | 2.841 s | 2.4 ms | 1.892 s |
| **Approx** | 26.09 | 0.743 | 32.3:1 | 3.0 ms | 2.189 s | 1.1 ms | 1.144 s |
| **Identity** | 43.89 | 7.998 | 3.0:1 | 1.8 ms | 2.089 s | 0.7 ms | 1.010 s |

**Observações:**
- Loeffler e Matrix produzem **PSNR idêntico** (27.88 dB) — coeficientes quantizados iguais
- Approx é **~1.8 dB abaixo** das exatas, porém mais rápida no ESP32
- Identity confirma erro mínimo do pipeline (43.89 dB = apenas conversão RGB↔YCbCr)
- **Método B** (coeficientes transmitidos, decompress no PC): decompress em ~0.002s em vez de ~1.5s

---

## 🔧 Pipeline de Compressão

```
RGB → YCbCr (BT.601) → Blocos 8×8 → DCT 2D → Quantização (Q50 × k)
                                                       ↓
RGB ← YCbCr → Merge blocos ← IDCT 2D ← Dequantização ←┘
```

Todos os métodos compartilham:
- Aritmética inteira de ponto fixo (SCALE = 2²⁰)
- Tabelas de quantização Q50 padrão JPEG (Wallace 1992)
- Mesmo pipeline: YCbCr → blocos 8×8 → DCT → Quantização → IDCT → RGB

---

## 📚 Referências

1. **Loeffler, C., Ligtenberg, A., & Moschytz, G. S. (1989)**
   "Practical fast 1-D DCT algorithms with 11 multiplications"
   *Proc. ICASSP*

2. **Wallace, G. K. (1992)**
   "The JPEG still picture compression standard"
   *IEEE Trans. Consumer Electronics, 38(1)*

3. **Cintra, R. J., & Bayer, F. M. (2011)**
   "A DCT approximation for image compression"
   *IEEE Signal Processing Letters, 18(10), 579-583*

## 📄 Licença

MIT License — veja [LICENSE](LICENSE).

---

**Análise comparativa de algoritmos DCT em compressão de imagens — biblioteca portátil para qualquer sistema, testada em ESP32-CAM.**
