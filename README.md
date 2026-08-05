# IC-JPEG - DCT Image Compression Library & Embedded Analysis

[![C11](https://img.shields.io/badge/C-C11-blue.svg)](https://en.cppreference.com/w/c/11)
[![Python 3.10+](https://img.shields.io/badge/Python-3.10%2B-blue.svg)](https://www.python.org/downloads/)
[![ESP-IDF 5.x](https://img.shields.io/badge/ESP--IDF-5.x-red.svg)](https://docs.espressif.com/projects/esp-idf/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

[English](#english) | [Português](#português)

## English

`IC-JPEG` is a portable C library and research environment for a simplified
JPEG-like image-compression pipeline. It provides exact and low-complexity DCT
implementations, reproducible desktop experiments, and a two-board embedded
prototype built with an ESP32-CAM and an ESP32-S3.

> **Portability:** `libimage` is the canonical codec implementation. Its public
> API is platform-independent, while ESP-IDF-specific allocation and profiling
> hooks are enabled only when building for ESP targets.

The project is maintained in two identical GitHub mirrors:

- [gbriellma/IC-JPEG](https://github.com/gbriellma/IC-JPEG)
- [vacoutinho/IC-GLima-JPEG-like](https://github.com/vacoutinho/IC-GLima-JPEG-like)

---

## 📂 Project Structure

```text
.
├── libimage/                         # Portable canonical C codec
│   ├── include/
│   │   ├── jpeg_codec.h              # Public API
│   │   └── internal.h                # Internal constants and helpers
│   ├── src/
│   │   ├── codec.c                   # Compression/decompression pipeline
│   │   ├── colorspace.c              # RGB <-> YCbCr conversion
│   │   ├── dct_loeffler.c            # Fast exact DCT, float internally
│   │   ├── dct_matrix.c              # Orthonormal matrix reference DCT
│   │   ├── dct_approx.c              # RDCT approximation
│   │   ├── dct_class.c               # Silveira j=3 and j=7
│   │   ├── dct_identity.c            # Validation-only identity transform
│   │   ├── quantization.c            # Quantization and normalization tables
│   │   ├── zigzag_rle.c              # Zig-zag scan and RLE
│   │   ├── frame.c                   # RLE payload and legacy codec frame
│   │   └── cobs.c                    # COBS helpers for framed links
│   ├── python/libimage_wrapper.py    # Python ctypes bridge
│   ├── example/                      # C usage examples
│   ├── Makefile                      # Desktop build
│   └── CMakeLists.txt                # ESP-IDF component build
├── experiments/
│   ├── compare.py                    # Python algebra/quality validation
│   ├── compare_c.py                  # Canonical C benchmark
│   ├── src_py/                       # Readable Python reference
│   ├── imgs/                         # Fixed eight-image dataset
│   └── run_all.sh                    # Reproducible benchmark workflow
├── prototype/
│   ├── cam/src/main.c                # ESP32-CAM capture and compression
│   ├── s3/src/                       # Receive, decode, metrics, TFT, and SD
│   ├── common/experiment_protocol.*  # CAM <-> S3 protocol contract
│   └── platformio.ini                # PlatformIO environments
└── docs/
    ├── DCT_ALGORITHMS.md             # Arithmetic and DCT definitions
    ├── FRAME_FORMAT.md               # SPI record and payload formats
    ├── ANALISE.md                    # Experimental methodology
    └── protocol.md                   # Embedded architecture
```

Generated binaries, benchmark outputs, historical captures, local AI files,
and managed dependencies are intentionally excluded from version control.

---

## 📦 C Library - `libimage`

### Features

| Property | Current implementation |
|---|---|
| Language | C11 |
| Public sample/coefficient type | `int32_t` |
| Exact transforms | Loeffler and Matrix, using `float` internally |
| Approximate transforms | RDCT, Silveira j=3, and Silveira j=7 using integer arithmetic |
| Transform-core multiplication | Multiplier-free for RDCT and Silveira kernels |
| Color spaces | RGB and grayscale |
| Chroma subsampling | 4:4:4, 4:2:2, and 4:2:0 |
| Quantization factors used in experiments | `k = {0.1, 0.2, 0.5, 0.8}` |
| Entropy representation | Zig-zag ordering and run-length encoding |
| Desktop outputs | Static and shared libraries |
| Embedded integration | ESP-IDF component |

### DCT Methods

| Enum | Method | Internal arithmetic | Experimental role |
|---|---|---|---|
| `JPEG_DCT_LOEFFLER` | Loeffler 1989 | `float` | Fast exact DCT |
| `JPEG_DCT_MATRIX` | Orthonormal matrix DCT-II | `float` | Mathematical reference |
| `JPEG_DCT_RDCT` | Cintra-Bayer RDCT | `int32_t` | Low-complexity approximation |
| `JPEG_DCT_SILVEIRA_J3` | Silveira et al., j=3 | `int32_t` | Low-cost class member |
| `JPEG_DCT_SILVEIRA_J7` | Silveira et al., j=7 | `int32_t` | Higher-quality class member |
| `JPEG_DCT_IDENTITY` | Identity | `int32_t` | Validation only |

The multiplier-free statement applies to the RDCT and Silveira transform
kernels. Quantization, normalization-table setup, color conversion, RLE, and
transport are measured separately when transform cost must be isolated.

### Build and Test

```bash
cd libimage
make
make test
./bin/test_validation
```

The build creates `bin/libimage.a`, `bin/libimage.so`, and the validation test
binary. These files are generated locally and are not stored in Git.

### Basic API

```c
#include "jpeg_codec.h"

jpeg_image_t input = {
    .width = width,
    .height = height,
    .colorspace = JPEG_COLORSPACE_RGB,
    .data = rgb_data,
};

jpeg_params_t params = {
    .quality_factor = 0.8f,
    .dct_method = JPEG_DCT_LOEFFLER,
    .subsampling = JPEG_SUBSAMP_444,
    .flags = 0,
};

jpeg_compressed_t *compressed = NULL;
jpeg_image_t *reconstructed = NULL;

if (jpeg_compress(&input, &params, &compressed) == JPEG_SUCCESS) {
    jpeg_decompress(compressed, &reconstructed);
}

jpeg_free_compressed(compressed);
jpeg_free_image(reconstructed);
```

Optional flags:

- `JPEG_FLAG_SKIP_QUANTIZATION` bypasses quantization for validation.
- `JPEG_FLAG_KEEP_COEFFS` preserves transform coefficients for analysis.

---

## 🐍 Python Reference and Desktop Benchmarks

Python is used as a readable algebraic and quality-validation reference. The C
implementation remains the authoritative source for bitrate, runtime, memory,
and deployment results.

Install dependencies and run the complete workflow:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cd experiments
bash run_all.sh
```

The workflow evaluates five methods over the fixed eight-image dataset and
four quantization factors. It calculates PSNR, SSIM, bpp, compression time,
decompression time, and codec-owned memory estimates.

Run only the canonical C benchmark with:

```bash
cd experiments
../.venv/bin/python compare_c.py --imgs imgs --out resultados
```

All generated CSV files, plots, and reconstructed images stay local. The
article-ready curated exports are kept under `docs/artigo/paper_csv/`.

---

## 📡 Embedded Prototype

The current prototype uses two boards:

```text
ESP32-CAM + OV2640                 ESP32-S3 + TFT + SD
RGB565 QVGA capture               SPI master
RGB565 -> RGB888                  receives and validates records
JPEG-like compression             JPEG-like decompression
SPI slave                         PSNR, SSIM, bpp, timing, and logging
```

Current link configuration:

| Parameter | Value |
|---|---|
| Master / slave | ESP32-S3 master / ESP32-CAM slave |
| SPI mode | 1 |
| SPI clock | 1 MHz |
| Logical payload chunk | 512 B |
| Image record header | 24 B |
| Separate telemetry record | 36 B |
| JPEG subsampling | 4:4:4 |
| Camera frame | RGB565, QVGA 320x240 |

In `ALL` mode, one physical capture is frozen and reused for every comparison:

```text
1 RAW reference + 5 JPEG methods * 4 k factors = 21 records
```

This prevents lighting, movement, and sensor noise from changing the input
between methods within the same run. The JPEG payload sent over SPI is:

```text
[record_header 24 B][Y_RLE...][Cb_RLE...][Cr_RLE...]
```

Timing and diagnostic fields are transferred afterward in the separate
telemetry record and are not counted as image bytes.

Build both firmware targets with:

```bash
cd prototype
../.venv/bin/pio run -e cam -e s3
```

See [prototype/README.md](prototype/README.md) for wiring, controls, build
details, SD outputs, and power recommendations.

---

## 🔧 Compression Pipeline

```text
RGB -> YCbCr -> 8x8 blocks -> DCT -> quantization -> zig-zag -> RLE
                                                               |
RGB <- YCbCr <- merge blocks <- IDCT <- dequantization <- RLE decode
```

The desktop and embedded experiments share the same canonical codec. The
embedded layer adds camera capture, record framing, SPI transfer, CRC checks,
metrics, display, and SD logging around that codec.

---

## 📚 Documentation and References

- [DCT algorithms and numeric regime](docs/DCT_ALGORITHMS.md)
- [Experimental analysis](docs/ANALISE.md)
- [Embedded architecture](docs/protocol.md)
- [SPI frame format](docs/FRAME_FORMAT.md)
- [Embedded prototype guide](prototype/README.md)

Main research references:

1. C. Loeffler, A. Ligtenberg, and G. S. Moschytz, "Practical fast 1-D DCT
   algorithms with 11 multiplications," ICASSP, 1989.
2. G. K. Wallace, "The JPEG still picture compression standard," IEEE
   Transactions on Consumer Electronics, 1992.
3. R. J. Cintra and F. M. Bayer, "A DCT approximation for image compression,"
   IEEE Signal Processing Letters, 2011.
4. P. A. Silveira et al., low-complexity DCT-like transform class, 2022.

## 📄 License

Distributed under the [MIT License](LICENSE).

---

## Português

`IC-JPEG` é uma biblioteca C portátil e um ambiente de pesquisa para um
pipeline simplificado de compressão de imagens semelhante ao JPEG. O projeto
reúne implementações DCT exatas e de baixa complexidade, experimentos
reproduzíveis em computador e um protótipo embarcado com ESP32-CAM e ESP32-S3.

> **Portabilidade:** `libimage` é a implementação canônica do codec. Sua API
> pública é independente de plataforma; os recursos específicos do ESP-IDF
> para alocação e medição são ativados apenas em builds para ESP.

O projeto é mantido em dois espelhos idênticos no GitHub:

- [gbriellma/IC-JPEG](https://github.com/gbriellma/IC-JPEG)
- [vacoutinho/IC-GLima-JPEG-like](https://github.com/vacoutinho/IC-GLima-JPEG-like)

---

## 📂 Estrutura do Projeto

```text
.
├── libimage/                         # Codec C portátil e canônico
│   ├── include/jpeg_codec.h          # API pública
│   ├── include/internal.h            # Constantes e utilitários internos
│   ├── src/                          # Codec, DCTs, quantização, RLE e frames
│   ├── python/libimage_wrapper.py    # Ponte Python/ctypes
│   ├── example/                      # Exemplos de uso em C
│   ├── Makefile                      # Build para computador
│   └── CMakeLists.txt                # Componente ESP-IDF
├── experiments/
│   ├── compare.py                    # Validação algébrica em Python
│   ├── compare_c.py                  # Benchmark C canônico
│   ├── src_py/                       # Referência legível em Python
│   ├── imgs/                         # Dataset fixo de oito imagens
│   └── run_all.sh                    # Fluxo reproduzível completo
├── prototype/
│   ├── cam/src/main.c                # Captura e compressão na ESP32-CAM
│   ├── s3/src/                       # Recepção, decode, métricas, TFT e SD
│   ├── common/experiment_protocol.*  # Contrato CAM <-> S3
│   └── platformio.ini                # Ambientes PlatformIO
└── docs/                             # Algoritmos, análise e protocolo
```

Binários, resultados regeneráveis, capturas históricas, arquivos locais de IA
e dependências gerenciadas não são armazenados no Git.

---

## 📦 Biblioteca C - `libimage`

### Recursos

| Propriedade | Implementação atual |
|---|---|
| Linguagem | C11 |
| Tipo público de amostras e coeficientes | `int32_t` |
| Transformadas exatas | Loeffler e Matrix, internamente em `float` |
| Transformadas aproximadas | RDCT, Silveira j=3 e Silveira j=7 em aritmética inteira |
| Multiplicação no núcleo da transformada | Núcleos RDCT e Silveira sem multiplicadores gerais |
| Espaços de cor | RGB e escala de cinza |
| Subamostragem de croma | 4:4:4, 4:2:2 e 4:2:0 |
| Fatores experimentais de quantização | `k = {0.1, 0.2, 0.5, 0.8}` |
| Representação entrópica | Ordem zig-zag e codificação RLE |
| Saídas no computador | Bibliotecas estática e compartilhada |
| Integração embarcada | Componente ESP-IDF |

### Métodos DCT

| Enum | Método | Aritmética interna | Papel experimental |
|---|---|---|---|
| `JPEG_DCT_LOEFFLER` | Loeffler 1989 | `float` | DCT exata rápida |
| `JPEG_DCT_MATRIX` | DCT-II matricial ortonormal | `float` | Referência matemática |
| `JPEG_DCT_RDCT` | RDCT de Cintra-Bayer | `int32_t` | Aproximação de baixa complexidade |
| `JPEG_DCT_SILVEIRA_J3` | Silveira et al., j=3 | `int32_t` | Instância de menor custo |
| `JPEG_DCT_SILVEIRA_J7` | Silveira et al., j=7 | `int32_t` | Instância de maior qualidade |
| `JPEG_DCT_IDENTITY` | Identidade | `int32_t` | Somente validação |

A afirmação de ausência de multiplicadores se aplica aos núcleos RDCT e
Silveira. Quantização, preparo das tabelas de normalização, conversão de cor,
RLE e transporte são medidos separadamente quando o objetivo é isolar o custo
da transformada.

### Build e Testes

```bash
cd libimage
make
make test
./bin/test_validation
```

O build cria `bin/libimage.a`, `bin/libimage.so` e o executável de validação.
Esses artefatos são gerados localmente e não são armazenados no Git.

### API Básica

```c
#include "jpeg_codec.h"

jpeg_image_t entrada = {
    .width = largura,
    .height = altura,
    .colorspace = JPEG_COLORSPACE_RGB,
    .data = dados_rgb,
};

jpeg_params_t parametros = {
    .quality_factor = 0.8f,
    .dct_method = JPEG_DCT_LOEFFLER,
    .subsampling = JPEG_SUBSAMP_444,
    .flags = 0,
};

jpeg_compressed_t *comprimida = NULL;
jpeg_image_t *reconstruida = NULL;

if (jpeg_compress(&entrada, &parametros, &comprimida) == JPEG_SUCCESS) {
    jpeg_decompress(comprimida, &reconstruida);
}

jpeg_free_compressed(comprimida);
jpeg_free_image(reconstruida);
```

Flags opcionais:

- `JPEG_FLAG_SKIP_QUANTIZATION` ignora a quantização para validação.
- `JPEG_FLAG_KEEP_COEFFS` preserva os coeficientes para análise.

---

## 🐍 Referência Python e Benchmarks no Computador

O Python funciona como referência legível para validação algébrica e de
qualidade. A implementação C é a fonte oficial para bitrate, tempo, memória e
resultados de implantação.

Instale as dependências e execute o fluxo completo:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cd experiments
bash run_all.sh
```

O fluxo percorre cinco métodos, o dataset fixo de oito imagens e os quatro
fatores de quantização. São calculados PSNR, SSIM, bpp, tempos de compressão e
descompressão e uma estimativa da memória pertencente ao codec.

Para executar apenas o benchmark C canônico:

```bash
cd experiments
../.venv/bin/python compare_c.py --imgs imgs --out resultados
```

CSVs, gráficos e imagens reconstruídas permanecem locais. As exportações
curadas necessárias para o artigo ficam em `docs/artigo/paper_csv/`.

---

## 📡 Protótipo Embarcado

O protótipo atual usa duas placas:

```text
ESP32-CAM + OV2640                 ESP32-S3 + TFT + SD
captura RGB565 QVGA               master SPI
conversão RGB565 -> RGB888        recebe e valida registros
compressão JPEG-like              descompressão JPEG-like
slave SPI                         PSNR, SSIM, bpp, tempos e gravação
```

Configuração atual do enlace:

| Parâmetro | Valor |
|---|---|
| Master / slave | ESP32-S3 master / ESP32-CAM slave |
| Modo SPI | 1 |
| Clock SPI | 1 MHz |
| Chunk lógico de payload | 512 B |
| Header do registro de imagem | 24 B |
| Registro separado de telemetria | 36 B |
| Subamostragem JPEG | 4:4:4 |
| Captura da câmera | RGB565, QVGA 320x240 |

No modo `ALL`, uma captura física é congelada e reutilizada em toda a
comparação:

```text
1 referência RAW + 5 métodos JPEG * 4 fatores k = 21 registros
```

Isso impede que iluminação, movimento e ruído do sensor alterem a entrada
entre métodos do mesmo ensaio. O payload JPEG enviado por SPI é:

```text
[record_header 24 B][Y_RLE...][Cb_RLE...][Cr_RLE...]
```

Tempos e diagnósticos são transferidos depois em um registro separado de
telemetria e não são contabilizados como bytes da imagem.

Compile os dois firmwares com:

```bash
cd prototype
../.venv/bin/pio run -e cam -e s3
```

Consulte [prototype/README.md](prototype/README.md) para pinagem, controles,
build, saídas no SD e recomendações de alimentação.

---

## 🔧 Pipeline de Compressão

```text
RGB -> YCbCr -> blocos 8x8 -> DCT -> quantização -> zig-zag -> RLE
                                                                |
RGB <- YCbCr <- união dos blocos <- IDCT <- dequantização <- decode RLE
```

Os experimentos no computador e no protótipo usam o mesmo codec canônico. A
camada embarcada acrescenta captura, framing dos registros, transporte SPI,
CRC, métricas, display e gravação no SD.

---

## 📚 Documentação e Referências

- [Algoritmos DCT e regime numérico](docs/DCT_ALGORITHMS.md)
- [Análise experimental](docs/ANALISE.md)
- [Arquitetura embarcada](docs/protocol.md)
- [Formato dos frames SPI](docs/FRAME_FORMAT.md)
- [Guia do protótipo](prototype/README.md)

Referências de pesquisa principais:

1. C. Loeffler, A. Ligtenberg e G. S. Moschytz, "Practical fast 1-D DCT
   algorithms with 11 multiplications," ICASSP, 1989.
2. G. K. Wallace, "The JPEG still picture compression standard," IEEE
   Transactions on Consumer Electronics, 1992.
3. R. J. Cintra e F. M. Bayer, "A DCT approximation for image compression,"
   IEEE Signal Processing Letters, 2011.
4. P. A. Silveira et al., classe de transformadas DCT-like de baixa
   complexidade, 2022.

## 📄 Licença

Distribuído sob a [Licença MIT](LICENSE).
