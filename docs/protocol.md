# Arquitetura Embarcada

Este documento descreve o arranjo experimental atual entre `ESP32-CAM` e
`ESP32-S3` no projeto `IC-JPEG`.

O fluxo embarcado esta em validacao de bancada, mas ja e o caminho ativo do
firmware. A especificacao aqui segue:

- `prototype/common/experiment_protocol.h`;
- `prototype/cam/src/main.c`;
- `prototype/s3/src/main.c`;
- `prototype/s3/src/metrics.*`;
- `prototype/s3/src/sd_log.c`.

## Visao Geral

```text
ESP32-CAM                                      ESP32-S3
┌──────────────────────────────┐               ┌──────────────────────────────┐
│ OV2640 RGB565 QVGA           │               │ botoes A/B/C/D               │
│ fb_count=1                   │               │ TFT SPI                      │
│ grab when empty              │               │ SDMMC 1-bit                  │
│ AWB/AEC/AGC auto             │               │ SPI master                   │
│ warm-up antes da captura     │               │ recebe headers/payloads      │
│ RGB565 -> RGB888             │   SPI sync    │ valida CRC32                 │
│ RAW RGB888                   │<------------->│ usa RAW como referencia       │
│ JPEG-like por metodo/k       │               │ decodifica JPEG-like         │
│ SPI slave                    │               │ calcula PSNR/SSIM/bpp/tempo  │
└──────────────────────────────┘               │ salva BMP/TXT/CSV            │
                                               └──────────────────────────────┘
```

O ponto central do desenho experimental e que, no modo `ALL`, a CAM captura
uma unica imagem e congela o buffer `RGB888`. Todos os metodos e fatores `k`
sao derivados desse mesmo buffer.

## Topologia SPI

Pinagem do link:

```text
S3 IO14 -> CAM IO14  SCLK
S3 IO13 -> CAM IO13  MOSI
CAM IO12 -> S3 IO12  MISO
S3 IO6  -> CAM IO15  CS
CAM IO2 -> S3 IO9    READY
GND comum obrigatorio
```

Parametros atuais:

```text
S3 master
CAM slave
SPI mode = 1
EXP_SPI_FREQ_HZ = 1000000
EXP_SPI_PAYLOAD_CHUNK = 512
EXP_SPI_GUARD_BYTES = 1
```

Observacao historica: versoes anteriores dos documentos citavam `mode 0`,
`10 MHz` e chunks de `1024 B`. O firmware atual foi reduzido para `1 MHz`,
`mode 1` e chunks de `512 B` para estabilizar o link com ESP32-CAM em modo
slave e DMA.

## Frames Curtos

Controle S3 -> CAM:

```text
18 B logicos
19 B no fio
```

Status CAM -> S3:

```text
18 B logicos
19 B no fio
```

O byte extra e `EXP_SPI_GUARD_BYTES`. Ele cria margem para transacoes curtas e
para o comportamento do slave em leituras pequenas.

## Maquina de Estados

Estados SPI:

```text
BOOTING       0
IDLE          1
CAPTURING     2
RECORD_READY  3
DONE          4
ERROR         5
```

Erros:

```text
NONE
BUSY
BAD_CONFIG
CAPTURE_FAIL
ENCODE_FAIL
ABORTED
PROTO
CAM_OVF
```

Comandos:

```text
NOP
SET_CONFIG
TRIGGER
ABORT
READ_HEADER
READ_PAYLOAD
READ_TELEMETRY
```

`READ_PAYLOAD` continua no contrato, mas o firmware atual consegue auto-armar
o payload apos a leitura completa do header. `READ_HEADER` e `READ_TELEMETRY`
continuam no contrato para compatibilidade/diagnostico, mas o fluxo nominal
atual le header e telemetria diretamente porque a CAM auto-arma esses blocos.

## Sequencia de um Run

Fluxo nominal:

```text
POLL
SET_CONFIG
TRIGGER
RECORD_READY
header
payload
telemetria
RECORD_READY ou DONE
```

Detalhe importante do S3:

```text
le o header quando:
  READY == HIGH
  ou state == RECORD_READY com payload_len > 0
  current_order == order esperado
```

Isso impede que o S3 leia header quando a CAM ainda esta em `CAPTURING` ou
quando o proximo registro ainda nao avancou de ordem. No caminho nominal, a
CAM ja deixa o header pronto assim que sinaliza `READY=HIGH`.

## Captura na CAM

Configuracao atual da OV2640:

```text
PIXFORMAT_RGB565
FRAMESIZE_QVGA
320 x 240
XCLK = 8 MHz
fb_count = 1
grab_mode = CAMERA_GRAB_WHEN_EMPTY
fb_location = PSRAM
AWB/AEC/AGC automaticos
vflip = 0
hmirror = 0
```

Warm-up:

```text
boot:
  delay de 300 ms
  descarte de 3 frames

antes de captura real:
  descarte de 8 frames
  gap de 120 ms entre descartes
```

O warm-up forte fica antes da captura, nao no boot, para evitar uma
inicializacao lenta no S3/TFT. A motivacao e a estabilizacao da OV2640:
primeiras capturas podem sair rosadas enquanto AWB/AEC/AGC ainda convergem.

## Modos de Captura

### Metodo unico

O S3 envia um metodo JPEG e um `k_idx`. A CAM:

1. descarta frames de pre-captura;
2. captura uma imagem;
3. converte `RGB565 -> RGB888`;
4. comprime pelo metodo selecionado;
5. envia um registro JPEG.

`records_expected = 1`.

### ALL

O S3 envia `method_id = ALL`. A CAM:

1. salva `k_idx` e metodo atuais;
2. descarta frames de pre-captura;
3. captura uma unica imagem;
4. converte para `RGB888`;
5. envia `RAW` se `RAW_FIRST` estiver ativo;
6. percorre todos os `k`;
7. para cada `k`, percorre os cinco metodos JPEG;
8. restaura `k_idx` e `quality_factor` anteriores.

Total atual:

```text
EXP_K_FACTOR_COUNT = 4
EXP_JPEG_METHOD_COUNT = 5
EXP_RECORD_SLOT_COUNT = 1 + 5 * 4 = 21
```

Fatores:

```text
k_idx 0 -> 0.1
k_idx 1 -> 0.2
k_idx 2 -> 0.5
k_idx 3 -> 0.8
```

Metodos:

```text
loeffler
matrix
rdct
silveira_j3
silveira_j7
```

Ordem com `RAW_FIRST`:

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

## RAW como Referencia

O `RAW` e um baseline de transporte e referencia:

- nao chama `jpeg_compress()`;
- nao chama `jpeg_decompress()`;
- nao quantiza;
- nao altera o buffer-base;
- e enviado como `RGB888` puro;
- possui `PSNR = N/A` ou `0` conforme contexto de relatorio;
- possui `SSIM = N/A`;
- vira a referencia contra a qual os JPEGs sao comparados no S3.

Para `320x240`:

```text
RAW bytes = 320 * 240 * 3 = 230400
```

## JPEG como Registro Experimental

Cada registro JPEG transporta somente o payload RLE do codec:

```text
[Y_RLE...][Cb_RLE...][Cr_RLE...]
```

O contexto externo vem do `record_header` minimo e da configuracao do
experimento:

- metodo;
- `k_idx`;
- largura/altura inferidas como `320x240`;
- colorspace inferido como `RGB`;
- subsampling JPEG inferido como `4:4:4`;
- CRC32 do payload.

Os tempos medidos na CAM nao ficam no header da imagem. Eles sao enviados na
telemetria separada de `36 B`, auto-armada pela CAM depois do payload.

Assim, o overhead da imagem comprimida no fio e:

```text
record_header minimo = 24 B
payload JPEG         = RLE puro
header JPEG interno  = 0 B
```

## Tempos Medidos

Na CAM:

- `t_capture_us`;
- `t_rgb565_to_rgb888_us`;
- `t_prepare_us`;
- `t_algorithm_us`;
- `t_dct_kernel_us`;
- `t_decompress_us` da CAM, quando aplicavel.

Interpretacao:

- em `RAW`, `t_prepare_us = raw_pack_us`;
- em `JPEG`, `t_algorithm_us = compress_us`;
- em `JPEG`, `t_prepare_us = rle_payload_encode_us` (`t_frame_encode_us` nos TXT).

No S3:

- `t_spi_rx_us`;
- `t_raw_unpack_us`;
- `t_frame_decode_us`;
- `t_decompress_us`;
- `t_idct_kernel_us`;
- `t_metrics_us`;
- `t_sd_save_us`;
- `t_tft_draw_us`;
- `t_total_core_us`.

`t_total_core_us` representa apenas o caminho experimental:

```text
RAW core  = raw_pack + spi_rx + raw_unpack
JPEG core = compress + rle_encode + spi_rx + rle_decode + decompress
```

A decompressao local na CAM usada para PSNR (`t_cam_decompress_us` nos TXT)
fica separada do core, assim como `metrics`, `SD` e `TFT`.

As metricas `t_dct_kernel_us` e `t_idct_kernel_us` isolam somente as chamadas
aos kernels DCT/IDCT 8x8. Elas existem para comparar a complexidade das
transformadas aproximadas sem misturar conversao de cor, quantizacao, RLE,
frame ou SPI.

No firmware ESP, esses campos sao medidos com `esp_timer_get_time()`
imediatamente antes e depois de cada chamada real a `dct_func`/`idct_func` no
codec. Os TXT por registro tambem gravam `dct_kernel_calls` e
`idct_kernel_calls`; em QVGA RGB 4:4:4, o valor esperado e 3600 chamadas de DCT
e 3600 chamadas de IDCT por registro JPEG (`40 * 30 * 3` blocos 8x8).

## Validacoes Atuais

O receiver no S3 valida:

- `run_id`;
- `records_expected`;
- `order_index`;
- metodo e `k_idx`;
- dimensoes;
- tipo `RAW` ou `JPEG`;
- `payload_len`;
- CRC32 do payload;
- contexto JPEG suportado;
- duplicidade de slot;
- presenca de referencia RAW para metricas;
- status de decode/decompress;
- status final por registro.

O S3 aceita run parcial quando algum registro falha depois de ja haver
registros presentes. Nesse caso ele finaliza o que existe, mostra resumo e
marca o ensaio como parcial.

## Resiliência de Header

O problema observado em bancada era a perda de sincronismo quando a CAM estava
pronta para transmitir um `record_header`, mas o S3 ainda fazia
uma transacao curta de status de `19 B`.

Comportamento atual:

- se a CAM detecta transacao de status enquanto havia header pendente, ela nao
  aborta o registro;
- a CAM volta para `RECORD_READY` mantendo o header auto-armado;
- o S3 trata `STATUS` recebido no lugar de `HEADER` como ressincronizacao;
- o S3 tenta header ate `20` vezes;
- o S3 verifica `order_index` antes de aceitar o header.

## SD e Saidas

Por run:

```text
runXXXX_ref.bmp
runXXXX_ref.txt
runXXXX_raw.bmp
runXXXX_raw.txt
runXXXX_<metodo>_k0p1.bmp
runXXXX_<metodo>_k0p1.txt
runXXXX_<metodo>_k0p2.bmp
runXXXX_<metodo>_k0p2.txt
runXXXX_<metodo>_k0p5.bmp
runXXXX_<metodo>_k0p5.txt
runXXXX_<metodo>_k0p8.bmp
runXXXX_<metodo>_k0p8.txt
runXXXX_summary.txt
runXXXX_summary.csv
```

Global:

```text
/sdcard/summary.csv
```

Colunas do CSV:

```text
image,method,k,psnr,ssim,bpp,compress_us,decompress_us,
dct_kernel_us,idct_kernel_us,dct_kernel_calls,idct_kernel_calls,tx_us,
frame_bytes,tx_raw_us,raw_bytes,compression_ratio,sha256_input_first8,
status,status_detalhado
```

`frame_bytes` representa `payload_len + 24 B` do header minimo da imagem. A
telemetria CAM de `36 B` preserva as metricas, mas nao entra em `frame_bytes`
nem em `tx_us`.

## Inicializacao do S3

Sequencia atual:

1. delay para USB CDC;
2. log do motivo de reset;
3. LED e GPIO;
4. SPI-CAM;
5. buffers de run em PSRAM;
6. workspace de decode;
7. workspace de SSIM;
8. poll inicial da CAM;
9. envio de configuracao inicial;
10. tentativa de boot log no SD;
11. init da TFT;
12. splash;
13. estado `READY`.

Se a TFT fica branca mas os logs mostram `TFT iniciado` e `Splash OK`, o
backlight provavelmente esta ligado, mas o painel nao esta recebendo comandos
corretamente ou a alimentacao esta caindo. Verifique primeiro tensao, GND
comum, CS/DC, controlador e corrente disponivel.

## Arquivos Principais

- [prototype/common/experiment_protocol.h](../prototype/common/experiment_protocol.h)
- [prototype/cam/src/main.c](../prototype/cam/src/main.c)
- [prototype/s3/src/main.c](../prototype/s3/src/main.c)
- [prototype/s3/src/metrics.h](../prototype/s3/src/metrics.h)
- [prototype/s3/src/metrics.c](../prototype/s3/src/metrics.c)
- [prototype/s3/src/sd_log.c](../prototype/s3/src/sd_log.c)
