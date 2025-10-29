# DCT Image Compression & Analysis Tool

[![Python 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)](https://www.python.org/downloads/)

A Python tool to compress images using a DCT-based pipeline (similar to JPEG) and analyze the trade-offs between compression level, quality, and performance across three different DCT implementations.

This project implements and compares three DCT methods based on research papers, including color space transformation, quantization, and comprehensive quality metrics analysis. All implementations use **pure Python with integer arithmetic** for educational purposes and fair comparison.

## ✨ Key Features

-   **JPEG-like Pipeline**: Implements the core compression steps: RGB to YCbCr conversion, 8x8 block processing, DCT, Quantization (Q50 standard tables), and the inverse operations.
-   **Three DCT Implementations** (all in pure Python):
    -   **Loeffler Fast DCT**: Optimized algorithm with only 11 multiplications (Loeffler et al. 1989)
    -   **Matrix DCT**: Pure mathematical implementation using direct DCT-II formula
    -   **Approximate DCT**: Cintra-Bayer 2011 low-complexity approximation with integer-only matrix (values {-1, 0, 1})
-   **Dual Comparison Modes**: 
    -   **K-Factor Mode**: Compare methods across different compression factors [2.0, 5.0, 10.0, 15.0]
    -   **Bitrate Mode**: Compare methods at target bitrates [0.1, 0.25, 0.5, 1.0 bpp]
-   **📊 Comprehensive Analysis**: Automatically calculates and plots key metrics:
    -   PSNR (Peak Signal-to-Noise Ratio)
    -   SSIM (Structural Similarity Index)
    -   Processing Time (ms)
    -   Bitrate estimation (bpp - bits per pixel)
    -   Rate-Distortion Efficiency (PSNR/bpp)
-   **Method Comparison**: Built-in comparison tool (`compare_methods.py`) that evaluates all three DCT methods on the same dataset.
-   **Rich Visualizations**: Generates multiple plots for each image, including quality vs. compression factor, performance trade-offs, and dataset-wide summary boxplots.
-   **Organized Output**: Saves compressed images, plots, and numerical results into structured output directories.

## 🔧 How It Works

The compression process follows these main steps for each image:
1.  **Color Space Conversion**: The input RGB image is converted to the YCbCr color space. This separates luminance (Y) from chrominance (Cb, Cr), which is more efficient for compression.
2.  **Block Splitting**: Each channel (Y, Cb, Cr) is divided into 8x8 pixel blocks.
3.  **Discrete Cosine Transform (DCT)**: A 2D DCT is applied to each block, converting spatial pixel values into frequency coefficients.
4.  **Quantization**: Frequency coefficients are quantized using standard JPEG tables, scaled by a configurable factor `k`. This is the main lossy step where information is discarded.
5.  **Reconstruction**: The inverse process (de-quantization, inverse DCT, merging blocks, and YCbCr to RGB conversion) is applied to reconstruct the image.

## 📂 File Structure

```
.
├── src/
│   ├── imgs/              # Place your images here
│   ├── constantes.py      # Constants, quantization tables (Q50_LUMA, Q50_CHROMA)
│   ├── dct.py             # Three DCT implementations (Loeffler, Matrix, Approximate)
│   ├── main.py            # Main script - processes images with selected DCT method
│   ├── pipeline.py        # Core compression/decompression pipeline
│   └── plots.py           # Metrics calculation and visualization
├── compare_methods.py     # Compare all three DCT methods side-by-side
├── results_{method}/      # Compressed images output (created at runtime)
├── plots_{method}/        # Analysis plots and metrics (created at runtime)
└── requirements.txt       # Project dependencies
```

## 🚀 Getting Started

### Prerequisites

-   Python 3.8 or higher (for Python implementation)
-   `pip` and `venv`
-   C++ compiler (g++ or clang) for C++ implementation

## 📋 Usage

### Single Method Analysis

1.  **Add Images**: Place the images you want to process inside the `src/imgs/` directory.

2.  **Configure**: Open `src/main.py` to adjust parameters:
    ```python
    DCT_METHOD = 'loeffler'  # Options: 'loeffler', 'matrix', 'approximate'
    K_FACTORS = [2.0, 5.0, 10.0, 15.0]  # Compression factors
    ```

3.  **Run the script:**
    ```sh
    python src/main.py
    ```

## 📈 Output

After running, the script will generate directories based on the selected method:

-   `results_{method}/`: Compressed output images, organized by original image name
-   `plots_{method}/`: Analysis plots and text files with numerical results
    - Individual image plots (PSNR, SSIM, Bitrate vs k-factor)
    - Dataset summary boxplots
    - Detailed metrics in `results.txt` files

## 🔬 DCT Methods Comparison

### Performance Characteristics

| Method | Quality (PSNR) | Speed | Complexity | Accuracy |
|--------|---------------|-------|------------|----------|
| **Approximate** | ⭐⭐⭐⭐ (24.38 dB) | ⚡⚡⚡⚡ Fastest (~2.0s) | 0 multiplications | Approximate |
| **Loeffler** | ⭐⭐⭐⭐⭐ (26.06 dB) | ⚡⚡⚡ Fast (~2.8s) | 11 multiplications | Exact |
| **Matrix** | ⭐⭐⭐⭐⭐ (25.90 dB) | ⚡ Slowest (~15.4s) | 64 multiplications | Exact |

*Benchmark results using pure Python implementation with k=10.0*

### Implementation Details

- **Approximate (Cintra-Bayer 2011)**: 
  - Uses integer-only T matrix with values {-1, 0, 1}
  - Only normalization by 1/√8 required (no S matrix)
  - Zero multiplications in matrix operations
  - **26% faster than Loeffler** in Python implementation
  - ~1.5-2 dB PSNR reduction compared to exact methods
  
- **Loeffler (1989)**: 
  - 11 multiplications per 1D-DCT
  - Butterfly structure with optimized data flow
  - Numerically exact DCT-II
  - Good balance between speed and quality
  
- **Matrix**: 
  - Direct DCT-II formula: `Y[k] = Σ x[n]·cos(π·k·(2n+1)/(2N))`
  - 64 multiplications per 8x8 block
  - Reference implementation for validation
  - Slowest but most straightforward

All methods use:
- **Integer arithmetic** (scale factor 1000) for consistency
- Standard JPEG **Q50 quantization tables** (Wallace 1992)
- Same pipeline: YCbCr conversion → 8x8 blocks → DCT → Quantization → IDCT → RGB

## 📚 References

This project is based on the following research papers:

1. **Loeffler, C., Ligtenberg, A., & Moschytz, G. S. (1989)**  
   "Practical fast 1-D DCT algorithms with 11 multiplications"  
   *Proceedings of the International Conference on Acoustics, Speech, and Signal Processing*  
   - Butterfly structure with optimal multiplication count

2. **Wallace, G. K. (1992)**  
   "The JPEG still picture compression standard"  
   *IEEE Transactions on Consumer Electronics, 38(1)*  
   - Standard Q50 quantization tables used in this implementation

3. **Cintra, R. J., & Bayer, F. M. (2011)**  
   "A DCT approximation for image compression"  
   *IEEE Signal Processing Letters, 18(10), 579-583*  
   - Integer-only T matrix approximation with {-1, 0, 1} values
   - Normalization by 1/√8 only (no separate S matrix needed)

## 📄 License

MIT License - see [LICENSE](LICENSE) file for details.

---

**Developed for comparative analysis of DCT algorithms in image compression.**

---
---

### 🇧🇷 Versão em Português

# Ferramenta de Compressão e Análise de Imagens com DCT

[![Python 3.8+](https://img.shields.io/badge/python-3.8+-blue.svg)](https://www.python.org/downloads/)

Uma ferramenta em Python para comprimir imagens usando um pipeline baseado em DCT e analisar os trade-offs entre nível de compressão, qualidade e performance.

Este projeto implementa e compara três métodos DCT baseados em artigos científicos, incluindo transformação de espaço de cores, quantização e análise completa de métricas de qualidade. **Todas as implementações usam Python puro com aritmética inteira** para fins educacionais e comparação justa.

## ✨ Principais Funcionalidades

-   **Pipeline similar ao JPEG**: Implementa os passos principais de compressão: conversão de RGB para YCbCr, processamento em blocos 8x8, DCT, Quantização (tabelas Q50 padrão) e as operações inversas.
-   **Três Implementações de DCT** (todas em Python puro):
    -   **DCT Rápida de Loeffler**: Algoritmo otimizado com apenas 11 multiplicações (Loeffler et al. 1989)
    -   **DCT Matricial**: Implementação matemática pura usando fórmula direta da DCT-II
    -   **DCT Aproximada**: Aproximação Cintra-Bayer 2011 de baixa complexidade com matriz inteira (valores {-1, 0, 1})
-   **Dois Modos de Comparação**:
    -   **Modo K-Factor**: Compara métodos em diferentes fatores de compressão [2.0, 5.0, 10.0, 15.0]
    -   **Modo Bitrate**: Compara métodos em taxas de bits alvo [0.1, 0.25, 0.5, 1.0 bpp]
-   **📊 Análise Completa**: Calcula e plota automaticamente métricas essenciais:
    -   PSNR (Peak Signal-to-Noise Ratio)
    -   SSIM (Structural Similarity Index)
    -   Tempo de Processamento (ms)
    -   Estimativa de taxa de bits (bpp - bits por pixel)
    -   Eficiência Rate-Distortion (PSNR/bpp)
-   **Comparação de Métodos**: Ferramenta integrada (`compare_methods.py`) que avalia os três métodos DCT no mesmo dataset.
-   **Visualizações Ricas**: Gera múltiplos gráficos para cada imagem, incluindo qualidade vs. fator de compressão, trade-offs de performance e boxplots que resumem os resultados de todo o dataset.
-   **Saída Organizada**: Salva as imagens comprimidas, os gráficos e os resultados numéricos em diretórios de saída estruturados.

## 🔧 Como Funciona

O processo de compressão segue os seguintes passos para cada imagem:
1.  **Conversão de Espaço de Cores**: A imagem RGB de entrada é convertida para o espaço de cores YCbCr. Isso separa a luminância (Y) da crominância (Cb, Cr), o que é mais eficiente para a compressão.
2.  **Divisão em Blocos**: Cada canal (Y, Cb, Cr) é dividido em blocos de 8x8 pixels.
3.  **Transformada Discreta de Cosseno (DCT)**: Uma DCT 2D é aplicada a cada bloco, convertendo valores espaciais de pixels em coeficientes de frequência.
4.  **Quantização**: Os coeficientes de frequência são quantizados usando tabelas padrão do JPEG, escalonadas por um fator `k` configurável. Este é o principal passo com perdas, onde informação é descartada.
5.  **Reconstrução**: O processo inverso (dequantização, DCT inversa, união dos blocos e conversão de YCbCr para RGB) é aplicado para reconstruir a imagem.

## 📂 Estrutura de Arquivos

```
.
├── src/
│   ├── imgs/              # Coloque suas imagens aqui
│   ├── constantes.py      # Constantes e tabelas de quantização (Q50_LUMA, Q50_CHROMA)
│   ├── dct.py             # Três implementações DCT (Loeffler, Matricial, Aproximada)
│   ├── main.py            # Script principal - processa imagens com método DCT selecionado
│   ├── pipeline.py        # Pipeline de compressão/descompressão
│   └── plots.py           # Cálculo de métricas e visualização
├── compare_methods.py     # Compara os três métodos DCT lado a lado
├── results_{metodo}/      # Imagens comprimidas (criado em tempo de execução)
├── plots_{metodo}/        # Gráficos de análise e métricas (criado em tempo de execução)
└── requirements.txt       # Dependências do projeto
```

## 🚀 Como Começar

### Pré-requisitos

-   Python 3.8 ou superior
-   `pip` e `venv`

## 📋 Como Usar

### Análise de Método Único

1.  **Adicione Imagens**: Coloque as imagens que você deseja processar dentro do diretório `src/imgs/`.

2.  **Configure**: Abra `src/main.py` para ajustar os parâmetros:
    ```python
    DCT_METHOD = 'loeffler'  # Opções: 'loeffler', 'matrix', 'approximate'
    K_FACTORS = [2.0, 5.0, 10.0, 15.0]  # Fatores de compressão
    ```

3.  **Execute o script:**
    ```sh
    python src/main.py
    ```


## 📈 Saída

Após a execução, o script irá gerar diretórios baseados no método selecionado:

-   `results_{metodo}/`: Imagens comprimidas, organizadas por nome da imagem original
-   `plots_{metodo}/`: Gráficos de análise e arquivos de texto com resultados numéricos
    - Gráficos individuais por imagem (PSNR, SSIM, Bitrate vs fator k)
    - Boxplots resumindo todo o dataset
    - Métricas detalhadas em arquivos `results.txt`

## 🔬 Comparação dos Métodos DCT

### Características de Performance

| Método | Qualidade (PSNR) | Velocidade | Complexidade | Precisão |
|--------|-----------------|-----------|--------------|----------|
| **Aproximada** | ⭐⭐⭐⭐ (24.38 dB) | ⚡⚡⚡⚡ Mais Rápida (~2.0s) | 0 multiplicações | Aproximada |
| **Loeffler** | ⭐⭐⭐⭐⭐ (26.06 dB) | ⚡⚡⚡ Rápida (~2.8s) | 11 multiplicações | Exata |
| **Matricial** | ⭐⭐⭐⭐⭐ (25.90 dB) | ⚡ Mais Lenta (~15.4s) | 64 multiplicações | Exata |

*Resultados de benchmark usando implementação Python pura com k=10.0*

### Detalhes de Implementação

- **Aproximada (Cintra-Bayer 2011)**: 
  - Usa matriz T inteira com valores {-1, 0, 1}
  - Apenas normalização por 1/√8 necessária (sem matriz S)
  - Zero multiplicações nas operações matriciais
  - **26% mais rápida que Loeffler** na implementação Python
  - Redução de ~1.5-2 dB PSNR comparada aos métodos exatos
  
- **Loeffler (1989)**: 
  - 11 multiplicações por 1D-DCT
  - Estrutura butterfly com fluxo de dados otimizado
  - DCT-II numericamente exata
  - Bom equilíbrio entre velocidade e qualidade
  
- **Matricial**: 
  - Fórmula direta da DCT-II: `Y[k] = Σ x[n]·cos(π·k·(2n+1)/(2N))`
  - 64 multiplicações por bloco 8x8
  - Implementação de referência para validação
  - Mais lenta mas mais direta

Todos os métodos usam:
- **Aritmética inteira** (fator de escala 1000) para consistência
- **Tabelas de quantização Q50** padrão JPEG (Wallace 1992)
- Mesmo pipeline: conversão YCbCr → blocos 8x8 → DCT → Quantização → IDCT → RGB

- **Loeffler**: 11 multiplicações, operações butterfly, otimizado para velocidade
- **Matricial**: Implementação direta da fórmula DCT-II com N²=64 multiplicações
- **Aproximada**: Algoritmo BAS-2008, operações apenas com inteiros, sem funções trigonométricas

Todos os métodos usam:
- Tabelas de quantização Q50 padrão do JPEG (do paper de Wallace)
- Aritmética inteira (fator de escala 1000) para precisão
- Mesmo pipeline de quantização para comparação justa

## 📚 Referências Bibliográficas

Este projeto é baseado nos seguintes papers de pesquisa (disponíveis em `PDFs/`):

1. **Loeffler, C., Ligtenberg, A., & Moschytz, G. S. (1989)**  
   "Practical fast 1-D DCT algorithms with 11 multiplications"  
   *Proceedings of the International Conference on Acoustics, Speech, and Signal Processing*

2. **Wallace, G. K. (1992)**  
   "The JPEG still picture compression standard"  
   *IEEE Transactions on Consumer Electronics, 38(1)*

3. **Cintra, R. J., & Bayer, F. M. (2011)**  
   "A DCT approximation for image compression"  
   *IEEE Signal Processing Letters, 18(10)*

## 📄 Licença

MIT License - veja o arquivo [LICENSE](LICENSE) para detalhes.

---

**Desenvolvido para análise comparativa de algoritmos DCT em compressão de imagens.**