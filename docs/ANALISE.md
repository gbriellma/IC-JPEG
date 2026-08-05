# Análise Experimental

Este documento descreve como interpretar os resultados atuais do `IC-JPEG`.
Existem duas frentes experimentais:

1. benchmark host, executado em PC com `libimage.so`;
2. ensaio embarcado, executado no par ESP32-CAM + ESP32-S3.

O benchmark host continua sendo a referencia canônica de qualidade,
rate-distortion e custo do codec em ambiente controlado. O ensaio embarcado
mede o comportamento do pipeline real com camera, SPI, decode, metricas, TFT e
SD.

## Pergunta Central

O projeto responde:

- qual é o compromisso entre qualidade, taxa, tempo e memória em um pipeline
  JPEG-like com interface `int32_t`, referências exatas em `float` e
  aproximações inteiras;
- como `Loeffler`, `Matrix`, `RDCT`, `Silveira j=3` e `Silveira j=7` se
  comportam sob a mesma quantizacao;
- quanto o pipeline embarcado acrescenta de custo de captura, empacotamento,
  transporte SPI, decode, metricas, display e SD;
- se todos os metodos e fatores `k` foram avaliados sobre exatamente a mesma
  imagem de entrada no modo `ALL`.

## Benchmark Host

Pipeline:

- biblioteca oficial: `libimage.so`;
- dataset: `experiments/imgs`;
- implementação: interface e pipeline em `int32_t`, com Loeffler e Matrix em
  `float` internamente e métodos aproximados em aritmética inteira;
- entrada: RGB;
- subsampling: `4:4:4`;
- flags: `0`;
- fatores: `k = {0.1, 0.2, 0.5, 0.8}`.

Metodos:

```text
loeffler
matrix
rdct
silveira_j3
silveira_j7
```

Metricas:

- `PSNR`;
- `SSIM`;
- `bpp`;
- tempo medio de compressao;
- tempo medio de descompressao;
- pico de memoria dinamica pertencente ao codec.

Execucao:

```bash
cd experiments
bash run_all.sh
```

Artefatos:

- `experiments/resultados/metrics.csv`;
- `experiments/resultados/summary_table.csv`;
- `experiments/resultados/matrix_loeffler_presentation.csv`;
- `experiments/resultados/matrix_loeffler_expectation.csv`;
- `experiments/resultados/matrix_loeffler_note.txt`;
- graficos em `experiments/resultados/`.

## Politica de Apresentacao

Para o grafico `Matrix vs Loeffler`:

- `PSNR`: 2 casas decimais, `HALF_UP`;
- `SSIM`: 2 casas decimais, `HALF_UP`;
- `bpp`: 2 casas decimais, `HALF_UP`.

Nos plots, rotulos visuais podem usar 1 casa decimal por legibilidade. Os CSVs
mantem precisao maior.

Valores de `Matrix` e `Loeffler` so devem ser apresentados como iguais quando
coincidem apos a politica declarada. Nao ha colapso artificial de series.

## Ensaio Embarcado

O ensaio embarcado usa:

- ESP32-CAM com OV2640;
- captura `RGB565`;
- conversao para `RGB888`;
- S3 como SPI master;
- CAM como SPI slave;
- SPI `mode 1`, `1 MHz`, chunk `512 B`;
- SD para persistencia;
- TFT para feedback visual.

No modo `ALL`, a CAM gera:

```text
1 RAW + 5 metodos JPEG * 4 fatores K = 21 registros
```

Esse modo e o mais importante para comparacao cientifica porque todos os
registros partem da mesma imagem congelada.

## Ordem Experimental Embarcada

Com `RAW_FIRST` ativo:

```text
0   raw
1   loeffler     k=0.1
2   matrix       k=0.1
3   rdct         k=0.1
4   silveira_j3  k=0.1
5   silveira_j7  k=0.1
6   loeffler     k=0.2
7   matrix       k=0.2
8   rdct         k=0.2
9   silveira_j3  k=0.2
10  silveira_j7  k=0.2
11  loeffler     k=0.5
12  matrix       k=0.5
13  rdct         k=0.5
14  silveira_j3  k=0.5
15  silveira_j7  k=0.5
16  loeffler     k=0.8
17  matrix       k=0.8
18  rdct         k=0.8
19  silveira_j3  k=0.8
20  silveira_j7  k=0.8
```

## Metricas Embarcadas

Por registro, o S3 salva:

- metodo;
- `k`;
- `PSNR`;
- `SSIM`;
- `bpp`;
- tempo de compressao na CAM;
- tempo de decompressao no S3;
- tempo dos kernels DCT na CAM;
- tempo dos kernels IDCT no S3;
- tempo de transmissao SPI no S3;
- bytes da imagem no fio (`frame_bytes = payload + header minimo de 24 B`);
- bytes RAW;
- razao de compressao contra RAW;
- `sha256_input_first8` da referencia;
- status;
- detalhe textual.

Colunas atuais do CSV:

```text
image,method,k,psnr,ssim,bpp,compress_us,decompress_us,
dct_kernel_us,idct_kernel_us,dct_kernel_calls,idct_kernel_calls,tx_us,
frame_bytes,tx_raw_us,raw_bytes,compression_ratio,sha256_input_first8,
status,status_detalhado
```

O `SSIM` embarcado usa janela uniforme `7x7`.

`frame_bytes` nao inclui a telemetria CAM de `36 B`; essa telemetria existe
para preservar metricas sem inflar o header da imagem comprimida.

Nos arquivos TXT por registro, `t_cam_decompress_us` tambem e salvo como
diagnostico da estimativa local de PSNR na CAM. Esse tempo nao entra no
`decompress_us` do CSV nem no tempo core do caminho experimental.

## Interpretacao dos Tempos

Tempos da CAM:

- `t_capture_us`: tempo de captura do frame OV2640;
- `t_rgb565_to_rgb888_us`: conversao para RGB888;
- `t_raw_pack_us`: preparo do RAW;
- `t_compress_us`: compressao JPEG-like;
- `t_cam_decompress_us`: decompressao local na CAM para estimativa de PSNR;
- `t_dct_kernel_us`: tempo acumulado apenas nas chamadas DCT 8x8;
- `t_frame_encode_us`: empacotamento RLE do payload JPEG.

Tempos do S3:

- `t_spi_rx_us`: recepcao do payload por SPI;
- `t_raw_unpack_us`: copia/conferencia do RAW;
- `t_frame_decode_us`: parse do payload RLE;
- `t_decompress_us`: reconstrucao JPEG-like;
- `t_idct_kernel_us`: tempo acumulado apenas nas chamadas IDCT 8x8;
- `t_metrics_us`: PSNR/SSIM/bpp;
- `t_sd_save_us`: gravacao dos artefatos;
- `t_tft_draw_us`: desenho no display.

Tempo core por tipo:

```text
RAW core  = raw_pack + spi_rx + raw_unpack
JPEG core = compress + rle_encode + spi_rx + rle_decode + decompress
```

`t_cam_decompress_us`, `SD`, `TFT` e `metrics` devem ser relatados
separadamente para nao misturar codec/transporte com medicao ou I/O de
apresentacao.

Para evidenciar a acao das aproximadas, use `t_dct_kernel_us` e
`t_idct_kernel_us`. Esses campos removem conversao de cor, preparo de tabela,
correcao de norma, quantizacao, dequantizacao, RLE, frame e transporte da
comparacao; portanto medem apenas o nucleo da transformada.

Como checagem de sanidade no prototipo atual, cada TXT de registro JPEG deve
trazer `dct_kernel_calls = 3600` e `idct_kernel_calls = 3600` em QVGA RGB
4:4:4. Esse numero vem de `40 * 30` blocos 8x8 por canal e tres canais. Se a
resolucao ou o subsampling mudar, essa contagem tambem deve mudar.

## Controle de Imagem Rosada

A OV2640 pode gerar primeira captura rosada por estabilizacao incompleta de
AWB/AEC/AGC. O firmware atual faz:

```text
boot:
  300 ms + 3 frames descartados

antes de captura real:
  8 frames descartados
  120 ms entre frames
```

No modo individual, uma primeira imagem rosada pode desaparecer em capturas
seguintes. No modo `ALL`, se a imagem base sair rosada, todos os metodos e
todos os fatores daquele run sairao rosados, porque todos partem da mesma
referencia.

Para diagnostico cientifico, o ideal e salvar uma sequencia de warm-up ou usar
colorbar da OV2640 em um build dedicado. O fluxo normal do firmware prioriza
boot rapido e estabilizacao antes da captura.

## Resultados Host Atuais

Os resultados host devem ser regenerados apos qualquer mudanca em:

- DCT;
- IDCT;
- quantizacao;
- RLE;
- frame;
- colorspace;
- lista de metodos;
- lista de fatores `k`.

Comando direto:

```bash
cd experiments
python compare_c.py --imgs imgs --out resultados
```

A tabela esperada contem exatamente:

```text
5 metodos * 4 fatores k = 20 linhas agregadas
```

Metodos:

```text
loeffler
matrix
rdct
silveira_j3
silveira_j7
```

## Relacao entre Host e Embarcado

Host:

- melhor para comparacao limpa de codec;
- dataset fixo;
- sem variacao de iluminacao;
- sem custo de SPI/TFT/SD;
- ideal para figuras do artigo.

Embarcado:

- melhor para demonstrar viabilidade em hardware;
- usa camera real;
- inclui variacao de sensor;
- mede transporte e armazenamento;
- produz imagens reais no SD;
- exige cuidado com alimentacao e estabilizacao da OV2640.

Os dois conjuntos nao devem ser misturados sem rotulo claro.

## Artefatos Correspondentes

- [summary_table.csv](../experiments/resultados/summary_table.csv)
- [matrix_loeffler_presentation.csv](../experiments/resultados/matrix_loeffler_presentation.csv)
- [matrix_loeffler_note.txt](../experiments/resultados/matrix_loeffler_note.txt)
- [matrix_vs_loeffler.png](../experiments/resultados/matrix_vs_loeffler.png)
- [protocol.md](protocol.md)
- [FRAME_FORMAT.md](FRAME_FORMAT.md)
