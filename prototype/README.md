# Protótipo Embarcado

Firmware de bancada do experimento `IC-JPEG` com duas placas:

- `cam`: ESP32-CAM com OV2640;
- `s3`: ESP32-S3 com TFT, SD, botoes e decodificacao.

O prototipo atual captura imagem real na ESP32-CAM, comprime usando o codec
JPEG-like do projeto, envia registros por SPI para o ESP32-S3, decodifica,
calcula metricas e grava resultados no SD.

## Resumo do Funcionamento Atual

CAM:

- inicializa OV2640 em `RGB565`, `QVGA 320x240`;
- usa `fb_count = 1` e `CAMERA_GRAB_WHEN_EMPTY`;
- aplica AWB/AEC/AGC automaticos uma vez no init;
- usa `vflip = 0` e `hmirror = 0`;
- faz warm-up leve no boot;
- antes de cada captura real, descarta frames adicionais;
- converte a captura `RGB565 -> RGB888`;
- envia `RAW` e/ou frames JPEG-like por SPI.

S3:

- inicializa GPIO, SPI-CAM, buffers em PSRAM, workspace de decode, SD e TFT;
- envia `SET_CONFIG` e `TRIGGER` para a CAM;
- espera `RECORD_READY`;
- so pede header quando `payload_len > 0` e `current_order` e o esperado;
- recebe payload em chunks SPI;
- valida CRC32;
- usa o `RAW` como referencia no modo completo;
- decodifica JPEGs;
- calcula PSNR, SSIM, bpp, tempos e taxa de compressao;
- desenha resultado na TFT;
- salva BMP/TXT/CSV no SD.

## Hardware Esperado

### ESP32-CAM

Camera:

```text
OV2640
PIXFORMAT_RGB565
FRAMESIZE_QVGA
320x240
XCLK = 8 MHz
fb_location = PSRAM
```

Pinos da camera AI-Thinker:

```text
PWDN  IO32
RESET -1
XCLK  IO0
SIOD  IO26
SIOC  IO27
D7    IO35
D6    IO34
D5    IO39
D4    IO36
D3    IO21
D2    IO19
D1    IO18
D0    IO5
VSYNC IO25
HREF  IO23
PCLK  IO22
```

### Link SPI CAM <-> S3

```text
S3 IO14 -> CAM IO14  SCLK
S3 IO13 -> CAM IO13  MOSI
CAM IO12 -> S3 IO12  MISO
S3 IO6  -> CAM IO15  CS
CAM IO2 -> S3 IO9    READY
GND comum obrigatório
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

`ctrl` e `status` possuem `18 B` logicos, mas sao transferidos como `19 B`
no fio. Header e payload tambem usam uma margem de guarda nas leituras
`slave -> master`.

### TFT no S3

Pinagem atual:

```text
MOSI      IO10
SCK       IO11
MISO      -1
CS        IO17
DC        IO18
TOUCH_CS  IO42
```

O firmware força controlador `ILI9341` no estado atual. Se o backlight acende
mas a tela fica branca, os suspeitos principais sao alimentacao, controlador
TFT diferente, pinos CS/DC, backlight isolado do controlador ou falha de init.

### SD no S3

SDMMC 1-bit:

```text
CLK IO39
CMD IO38
D0  IO40
```

O SD e montado no boot para atualizar `boot_log.txt` e tambem pode ser
montado sob demanda. O BTN B em `READY` cria `s3_test.txt`.

## Protocolo de Alto Nivel

Fluxo nominal de controle e dados:

```text
POLL
SET_CONFIG
TRIGGER
RECORD_READY
header auto-armado
payload auto-armado
telemetria auto-armada
RECORD_READY ou DONE
```

O S3 rejeita a leitura prematura do header. Antes da leitura direta, ele
espera:

```text
state == RECORD_READY
payload_len > 0
current_order == order esperado
```

Isso é importante porque o barramento usa transações curtas de status e uma
transação maior para o header. Se o master consultar o status quando a CAM já
está preparada para o header, a CAM não aborta o ensaio: mantém o header
auto-armado para a próxima leitura. Os comandos `READ_HEADER`, `READ_PAYLOAD`
e `READ_TELEMETRY` permanecem no contrato para compatibilidade e diagnóstico.

## Registros

Header minimo:

```text
EXP_RECORD_HEADER_SIZE = 24
EXP_RECORD_VERSION = 5
EXP_RECORD_MAGIC = "ICJ1"
```

Campos principais:

- `run_id`;
- `order_index`;
- `method_id`;
- `mode_type`;
- `k_idx`;
- `payload_len`;
- `crc32` do payload.

O S3 infere `width`, `height`, `colorspace`, `subsampling` e
`records_expected` pela configuracao do experimento. Os tempos da CAM,
`psnr_x100` e `dct_kernel_us/calls` chegam em uma telemetria separada de `36 B`
lida apos o payload.

Tipos de payload:

```text
RAW  = RGB888 puro, 320 * 240 * 3 = 230400 bytes
JPEG = RLE puro do codec
```

Payload JPEG atual:

```text
[Y_RLE...][Cb_RLE...][Cr_RLE...]
```

## Modos de Ensaio

### Metodo Unico

Selecionado por BTN C e BTN D no S3.

Ao pressionar BTN A:

1. S3 envia `TRIGGER` com metodo e `k`;
2. CAM descarta `8` frames;
3. CAM captura uma imagem;
4. CAM converte para RGB888;
5. CAM comprime usando o metodo selecionado;
6. S3 recebe 1 registro JPEG;
7. S3 decodifica, exibe e salva.

### ALL

Selecionado quando o metodo atual e `All` ou por atalho de benchmark.

O run completo atual e:

```text
1 RAW + 5 metodos JPEG * 4 fatores K = 21 registros
```

Metodos:

```text
loeffler
matrix
rdct
silveira_j3
silveira_j7
```

Fatores:

```text
k = 0.1, 0.2, 0.5, 0.8
```

O modo `ALL` captura uma unica imagem base. Essa imagem vira:

- payload `RAW`;
- referencia oficial no S3;
- fonte congelada para todos os metodos e todos os fatores `k`.

Esse desenho permite afirmar no trabalho que todos os metodos e fatores foram
avaliados sobre exatamente a mesma imagem de entrada.

## Botoes

```text
A      captura com metodo/k atuais
B      em READY: testa SD; em resultado: salva novamente
C      alterna metodo
D      alterna k
A+C    executa ensaio completo ALL
```

Ordem de metodos no S3:

```text
Loeffler -> Matrix -> RDCT -> Silv j3 -> Silv j7 -> All
```

Ordem de `k`:

```text
0.1 -> 0.2 -> 0.5 -> 0.8 -> 0.1
```

## Warm-up e Imagem Rosada

O boot da CAM usa estabilizacao leve para nao atrasar a interface:

```text
CAMERA_BOOT_STABILIZE_MS = 300
CAMERA_WARMUP_FRAMES = 3
```

Antes de capturar de verdade, tanto em metodo unico quanto em `ALL`, a CAM
descarta:

```text
CAMERA_PRECAPTURE_DISCARD_FRAMES = 8
CAMERA_WARMUP_GAP_MS = 120
```

Motivo: a OV2640 pode entregar a primeira imagem util rosada enquanto
AWB/AEC/AGC ainda estao convergindo. No modo `ALL`, isso e especialmente
visivel porque todos os registros sao derivados da mesma imagem base.

## Robustez SPI Atual

Proteções implementadas:

- a leitura direta do header tem até `20` tentativas no S3;
- S3 espera `order_index` correto antes de aceitar um header;
- S3 trata `STATUS` recebido no lugar de `HEADER` como ressincronizacao;
- CAM nao aborta o registro quando recebe uma transacao de status no tamanho
  de `19 B` enquanto havia header pendente;
- CAM usa `ABORT` para recuperar estado quando o S3 detecta falha;
- payload e lido em chunks de `512 B` para reduzir risco de transacao curta.

## Saidas no SD

No modo `ALL`, cada run cria um diretorio proprio:

```text
/sdcard/runXXXX/
```

Dentro dele ficam os artefatos do experimento:

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

No modo de metodo unico, os arquivos continuam sendo salvos diretamente na
raiz do SD.

Arquivo global:

```text
/sdcard/summary.csv
```

Colunas:

```text
image,method,k,psnr,ssim,bpp,compress_us,decompress_us,
dct_kernel_us,idct_kernel_us,dct_kernel_calls,idct_kernel_calls,tx_us,
frame_bytes,tx_raw_us,raw_bytes,compression_ratio,sha256_input_first8,
status,status_detalhado
```

No CSV, `decompress_us` e a descompressao executada no S3. A descompressao
local na CAM para PSNR aparece somente nos TXT como `t_cam_decompress_us` e
nao entra no tempo core.

`dct_kernel_us` e `idct_kernel_us` medem apenas as chamadas reais aos kernels
DCT/IDCT 8x8. No RGB `4:4:4` QVGA atual, cada registro JPEG deve ter 3600
chamadas de DCT e 3600 chamadas de IDCT (`1200` blocos por canal).

`frame_bytes` representa o payload da imagem mais o `record_header` minimo de
`24 B`. A telemetria CAM de `36 B` e separada e nao entra em `frame_bytes` nem
em `tx_us`.

Registros com status claramente invalido, como `CRC_FAIL`, `DECODE_FAIL`,
`DECOMP_FAIL`, `RAW_SIZE_FAIL`, `OOM`, `SD_FAIL`, `REF_MISSING` e
`MODE_FAIL`, sao pulados no `summary.csv` global.

## Build

O `pio` global pode nao estar configurado nessa maquina. O ambiente local do
projeto usa:

```bash
cd prototype
../.venv/bin/pio run -e cam
../.venv/bin/pio run -e s3
```

Para compilar os dois:

```bash
cd prototype
../.venv/bin/pio run -e cam -e s3
```

## Alimentacao

Para bancada e bateria, alimente com `5 V` regulados e corrente suficiente.
Recomendacao pratica:

```text
minimo: 5 V / 2 A
ideal:  5 V / 3 A
```

Com bateria LiPo/Li-ion de `3.7 V`, use conversor boost:

```text
LiPo -> boost 5 V -> CAM/S3/TFT/SD
```

Nao ligue LiPo diretamente em `3V3` nem dependa de uma celula descarregando
direto no pino `5V`. Use GND comum entre CAM e S3.

Capacitores recomendados no barramento:

```text
470 uF a 1000 uF entre 5V e GND
100 nF proximo aos modulos
```

Tela branca com backlight ligado pode indicar que o backlight recebeu energia,
mas o controlador TFT nao inicializou. Antes de culpar o desenho na tela,
verifique alimentacao, GND comum, CS/DC, controlador correto e tensao durante
escrita no SD.

## Referencias

- [../docs/FRAME_FORMAT.md](../docs/FRAME_FORMAT.md)
- [../docs/protocol.md](../docs/protocol.md)
- [../docs/ANALISE.md](../docs/ANALISE.md)
- [common/experiment_protocol.h](common/experiment_protocol.h)
