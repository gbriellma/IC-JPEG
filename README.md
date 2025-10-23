# DCT Image Compression & Analysis Tool

[![Python 3.10+](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/downloads/)

A Python tool to compress images using a DCT-based pipeline (similar to JPEG) and analyze the trade-offs between compression level, quality, and performance across three different DCT implementations.

This project implements and compares three DCT methods based on research papers, including color space transformation, quantization, and comprehensive quality metrics analysis.

## ✨ Key Features

-   **JPEG-like Pipeline**: Implements the core compression steps: RGB to YCbCr conversion, 8x8 block processing, DCT, Quantization (Q50 standard tables), and the inverse operations.
-   **Three DCT Implementations**:
    -   **Loeffler Fast DCT**: Optimized algorithm with only 11 multiplications (from Loeffler et al. paper)
    -   **Matrix DCT**: Pure mathematical implementation using direct DCT-II formula
    -   **Approximate DCT**: BAS-2008 low-complexity approximation (from Cintra-Bayer paper)
-   **Configurable Compression**: Easily adjust the compression level (`k-factors`) to study its impact on the output.
-   **📊 Comprehensive Analysis**: Automatically calculates and plots key metrics:
    -   PSNR (Peak Signal-to-Noise Ratio)
    -   SSIM (Structural Similarity Index)
    -   Processing Time (ms)
    -   Bitrate estimation (bpp - bits per pixel)
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

-   Python 3.10 or higher
-   `pip` and `venv`

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

### Compare All Methods

To compare all three DCT implementations on the same dataset:

```sh
python compare_methods.py
```

This will generate comparative plots and metrics for Loeffler, Matrix, and Approximate methods.

## 📈 Output

After running, the script will generate directories based on the selected method:

-   `results_{method}/`: Compressed output images, organized by original image name
-   `plots_{method}/`: Analysis plots and text files with numerical results
    - Individual image plots (PSNR, SSIM, Bitrate vs k-factor)
    - Dataset summary boxplots
    - Detailed metrics in `results.txt` files

For `compare_methods.py`:
-   `comparison_results/`: Comparative plots showing all three methods side-by-side

## 🔬 DCT Methods Comparison

### Performance Characteristics

| Method | Quality (PSNR) | Speed | Accuracy | Use Case |
|--------|---------------|-------|----------|----------|
| **Loeffler** | ⭐⭐⭐⭐⭐ (26.06 dB) | ⚡⚡⚡ Fast (2.5s) | Exact |
| **Matrix** | ⭐⭐⭐⭐⭐ (25.90 dB) | ⚡ Slow (13.7s) | Exact |
| **Approximate** | ⭐⭐⭐⭐ (25.34 dB) | ⚡⚡⚡ Fast (2.2s) | ~2-5% error |

*Benchmark results averaged across test dataset with k-factors [2.0, 5.0, 10.0, 15.0]*

### Implementation Details

- **Loeffler**: 11 multiplications, butterfly operations, optimized for speed
- **Matrix**: Direct DCT-II formula implementation with N²=64 multiplications  
- **Approximate**: BAS-2008 algorithm, integer-only operations, no trigonometric functions

All methods use:
- Standard JPEG Q50 quantization tables (from Wallace paper)
- Integer arithmetic (scale factor 1000) for precision
- Same quantization pipeline for fair comparison

## 📚 References

This project is based on the following research papers (available in `PDFs/`):

1. **Loeffler, C., Ligtenberg, A., & Moschytz, G. S. (1989)**  
   "Practical fast 1-D DCT algorithms with 11 multiplications"  
   *Proceedings of the International Conference on Acoustics, Speech, and Signal Processing*

2. **Wallace, G. K. (1992)**  
   "The JPEG still picture compression standard"  
   *IEEE Transactions on Consumer Electronics, 38(1)*

3. **Cintra, R. J., & Bayer, F. M. (2011)**  
   "A DCT approximation for image compression"  
   *IEEE Signal Processing Letters, 18(10)*

## 📄 License

MIT License - see [LICENSE](LICENSE) file for details.

---

**Developed for comparative analysis of DCT algorithms in image compression.**

---
---

### 🇧🇷 Versão em Português (`README.md`)

# Ferramenta de Compressão e Análise de Imagens com DCT

[![Python 3.10+](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/downloads/)

Uma ferramenta em Python para comprimir imagens usando um pipeline baseado em DCT e analisar os trade-offs entre nível de compressão, qualidade e performance.

Este projeto aplica conceitos centrais de processamento de imagens, incluindo transformação de espaço de cores, Transformada Discreta de Cosseno (DCT) e quantização para comprimir imagens, gerando em seguida relatórios de desempenho e visualizações detalhadas.

## ✨ Principais Funcionalidades

-   **Pipeline similar ao JPEG**: Implementa os passos principais de compressão: conversão de RGB para YCbCr, processamento em blocos 8x8, DCT, Quantização (tabelas Q50 padrão) e as operações inversas.
-   **Três Implementações de DCT**:
    -   **DCT Rápida de Loeffler**: Algoritmo otimizado com apenas 11 multiplicações (baseado no paper de Loeffler et al.)
    -   **DCT Matricial**: Implementação matemática pura usando fórmula direta da DCT-II
    -   **DCT Aproximada**: Aproximação BAS-2008 de baixa complexidade (baseado no paper de Cintra-Bayer)
-   **Compressão Configurável**: Ajuste facilmente o nível de compressão (`fatores k`) para estudar seu impacto no resultado.
-   **📊 Análise Completa**: Calcula e plota automaticamente métricas essenciais:
    -   PSNR (Peak Signal-to-Noise Ratio)
    -   SSIM (Structural Similarity Index)
    -   Tempo de Processamento (ms)
    -   Estimativa de taxa de bits (bpp - bits por pixel)
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

-   Python 3.10 ou superior
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

### Comparar Todos os Métodos

Para comparar as três implementações DCT no mesmo dataset:

```sh
python compare_methods.py
```

Isso irá gerar gráficos comparativos e métricas para os métodos Loeffler, Matricial e Aproximado.

## 📈 Saída

Após a execução, o script irá gerar diretórios baseados no método selecionado:

-   `results_{metodo}/`: Imagens comprimidas, organizadas por nome da imagem original
-   `plots_{metodo}/`: Gráficos de análise e arquivos de texto com resultados numéricos
    - Gráficos individuais por imagem (PSNR, SSIM, Bitrate vs fator k)
    - Boxplots resumindo todo o dataset
    - Métricas detalhadas em arquivos `results.txt`

Para `compare_methods.py`:
-   `comparison_results/`: Gráficos comparativos mostrando os três métodos lado a lado

## 🔬 Comparação dos Métodos DCT

### Características de Performance

| Método | Qualidade (PSNR) | Velocidade | Precisão | Caso de Uso |
|--------|-----------------|-----------|----------|-------------|
| **Loeffler** | ⭐⭐⭐⭐⭐ (26.06 dB) | ⚡⚡⚡ Rápido (2.5s) | Exata | Produção - Melhor equilíbrio |
| **Matricial** | ⭐⭐⭐⭐⭐ (25.90 dB) | ⚡ Lento (13.7s) | Exata | Referência - Educacional |
| **Aproximada** | ⭐⭐⭐⭐ (25.34 dB) | ⚡⚡⚡ Rápido (2.2s) | ~2-5% erro | Dispositivos de baixa potência |

*Resultados de benchmark médios do dataset de teste com fatores k [2.0, 5.0, 10.0, 15.0]*

### Detalhes de Implementação

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