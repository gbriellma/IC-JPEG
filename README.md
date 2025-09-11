# DCT Image Compression & Analysis Tool

[![Python 3.10+](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/downloads/)

A Python tool to compress images using a DCT-based pipeline (similar to JPEG) and analyze the trade-offs between compression level, quality, and performance.

This project applies core concepts of image processing, including color space transformation, Discrete Cosine Transform (DCT), and quantization to compress images and then generates detailed performance reports and visualizations.

## ✨ Key Features

-   **JPEG-like Pipeline**: Implements the core compression steps: RGB to YCbCr conversion, 8x8 block processing, DCT, Quantization, and the inverse operations.
-   **Configurable Compression**: Easily adjust the compression level (`k-factors`) to study its impact on the output.
-   **Dual DCT Methods**: Switch between a matrix-based DCT and an optimized Loeffler algorithm implementation.
-   **📊 In-depth Analysis**: Automatically calculates and plots key metrics:
    -   PSNR (Peak Signal-to-Noise Ratio)
    -   SSIM (Structural Similarity Index)
    -   Processing Time (ms)
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
│   └── imgs/
│       └── your_image.png  (Place your images here)
├── constantes.py         # Quantization tables
├── dct.py                # DCT algorithm implementations
├── main.py               # Main script to run the project
├── pipeline.py           # Core compression/decompression pipeline
├── utils.py              # Helper functions (metrics, plotting, logging)
└── requirements.txt      # Project dependencies
```

## 🚀 Getting Started

### Prerequisites

-   Python 3.10 or higher
-   `pip` and `venv`

## USAGE

1.  **Add Images**: Place the images you want to process inside the `src/imgs/` directory. If it doesn't exist, the script will create it for you.

2.  **Configure (Optional)**: Open the `main.py` file to adjust the main parameters:
    -   `DCT_METHOD`: Choose between `'loeffler'` (faster) or `'matrix'` (standard).
    -   `FATORES_K`: A list of compression factors to test (e.g., `[2.0, 5.0, 10.0]`).

3.  **Run the script:**
    ```sh
    python main.py
    ```

## 📈 Output

After running, the script will generate two main directories:

-   `resultados_{method}/`: Contains the compressed output images, sorted into subdirectories for each original image.
-   `plots_{method}/`: Contains all the generated analysis plots and text files with the numerical results.

---
---

### 🇧🇷 Versão em Português (`README.md`)

# Ferramenta de Compressão e Análise de Imagens com DCT

[![Python 3.10+](https://img.shields.io/badge/python-3.10+-blue.svg)](https://www.python.org/downloads/)

Uma ferramenta em Python para comprimir imagens usando um pipeline baseado em DCT e analisar os trade-offs entre nível de compressão, qualidade e performance.

Este projeto aplica conceitos centrais de processamento de imagens, incluindo transformação de espaço de cores, Transformada Discreta de Cosseno (DCT) e quantização para comprimir imagens, gerando em seguida relatórios de desempenho e visualizações detalhadas.

## ✨ Principais Funcionalidades

-   **Pipeline similar ao JPEG**: Implementa os passos principais de compressão: conversão de RGB para YCbCr, processamento em blocos 8x8, DCT, Quantização e as operações inversas.
-   **Compressão Configurável**: Ajuste facilmente o nível de compressão (`fatores k`) para estudar seu impacto no resultado.
-   **Dois Métodos de DCT**: Alterne entre uma implementação de DCT baseada em matriz e um algoritmo otimizado de Loeffler.
-   **📊 Análise Aprofundada**: Calcula e plota automaticamente métricas essenciais:
    -   PSNR (Peak Signal-to-Noise Ratio)
    -   SSIM (Structural Similarity Index)
    -   Tempo de Processamento (ms)
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
│   └── imgs/
│       └── sua_imagem.png  (Coloque suas imagens aqui)
├── constantes.py         # Tabelas de quantização
├── dct.py                # Implementações do algoritmo DCT
├── main.py               # Script principal para rodar o projeto
├── pipeline.py           # Pipeline principal de compressão/descompressão
├── utils.py              # Funções auxiliares (métricas, plots, logs)
└── requirements.txt      # Dependências do projeto
```

## 🚀 Como Começar

### Pré-requisitos

-   Python 3.10 ou superior
-   `pip` e `venv`

## 📋 Como Usar

1.  **Adicione Imagens**: Coloque as imagens que você deseja processar dentro do diretório `src/imgs/`. Se ele não existir, o script o criará para você.

2.  **Configure (Opcional)**: Abra o arquivo `main.py` para ajustar os parâmetros principais:
    -   `DCT_METHOD`: Escolha entre `'loeffler'` (mais rápido) ou `'matrix'` (padrão).
    -   `FATORES_K`: Uma lista de fatores de compressão para testar (ex: `[2.0, 5.0, 10.0]`).

3.  **Execute o script:**
    ```sh
    python main.py
    ```

## 📈 Saída

Após a execução, o script irá gerar dois diretórios principais:

-   `resultados_{metodo}/`: Contém as imagens de saída comprimidas, organizadas em subdiretórios para cada imagem original.
-   `plots_{metodo}/`: Contém todos os gráficos de análise gerados e os arquivos de texto com os resultados numéricos.