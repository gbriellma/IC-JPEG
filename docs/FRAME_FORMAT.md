# Formato dos Frames

Especificacao dos formatos usados no transporte SPI entre ESP32-CAM e
ESP32-S3, incluindo o header minimo de imagem, payload RLE e telemetria
separada.

Os tamanhos aqui sao os tamanhos logicos do protocolo. No fio, algumas
transacoes carregam `EXP_SPI_GUARD_BYTES = 1`.

## Camadas

O experimento usa cinco camadas distintas:

1. transacao SPI fisica;
2. frame curto de controle/status;
3. `record_header` minimo da imagem;
4. payload `RAW` ou JPEG RLE puro;
5. telemetria experimental separada.

```text
SPI transaction
  ctrl/status ou header/payload
    record_header minimo
      RAW RGB888
      ou
      JPEG RLE payload
    record_telemetry separado
```

## Barramento Fisico

```text
S3 master:
  IO14 -> CAM IO14  SCLK
  IO13 -> CAM IO13  MOSI
  IO6  -> CAM IO15  CS
  IO9  <- CAM IO2   READY

CAM slave:
  IO12 -> S3 IO12   MISO
```

Parametros atuais:

```text
EXP_SPI_FREQ_HZ = 1000000
EXP_SPI_MODE = 1
EXP_SPI_PAYLOAD_CHUNK = 512
EXP_SPI_GUARD_BYTES = 1
```

O comentario no codigo explica o motivo do `mode 1`: no link de bancada, o
ESP32-CAM como SPI slave entrega MISO estavel com `CPHA=1`. Se o master ler
status deslocado um bit, o sintoma esperado e magic incorreto.

## Controle S3 -> CAM

Tamanho:

```text
EXP_SPI_CTRL_FRAME_SIZE = 18
EXP_SPI_CTRL_WIRE_SIZE  = 19
```

Layout logico:

```text
[magic_le16][version][cmd][flags][method_id][k_idx][reserved0]
[run_id_le32][arg0_le32][crc16_le]
```

Campos:

| Campo | Bytes | Tipo | Uso |
|---|---:|---|---|
| `magic` | 2 | `uint16_le` | `0x5350`, string `"PS"` little-endian |
| `version` | 1 | `uint8` | `EXP_SPI_VERSION = 3` |
| `cmd` | 1 | `uint8` | `NOP`, `SET_CONFIG`, `TRIGGER`, `ABORT`, `READ_HEADER`, `READ_PAYLOAD`, `READ_TELEMETRY` |
| `flags` | 1 | `uint8` | bit `RAW_FIRST` |
| `method_id` | 1 | `uint8` | `RAW`, metodos JPEG ou `ALL` |
| `k_idx` | 1 | `uint8` | indice de fator `k` |
| `reserved0` | 1 | `uint8` | reservado |
| `run_id` | 4 | `uint32_le` | id temporario/persistente do run |
| `arg0` | 4 | `uint32_le` | reservado |
| `crc16` | 2 | `uint16_le` | CRC16-CCITT dos 16 bytes anteriores |

Comandos:

```text
0 NOP
1 SET_CONFIG
2 TRIGGER
3 ABORT
4 READ_HEADER
5 READ_PAYLOAD
6 READ_TELEMETRY
```

## Status CAM -> S3

Tamanho:

```text
EXP_SPI_STATUS_FRAME_SIZE = 18
EXP_SPI_STATUS_WIRE_SIZE  = 19
```

Layout logico:

```text
[magic_le16][version][state][error][records_expected][current_order]
[reserved0][run_id_le32][payload_len_le32][crc16_le]
```

Campos:

| Campo | Bytes | Tipo | Uso |
|---|---:|---|---|
| `magic` | 2 | `uint16_le` | `0x5350`, string `"PS"` little-endian |
| `version` | 1 | `uint8` | `EXP_SPI_VERSION = 3` |
| `state` | 1 | `uint8` | estado da CAM |
| `error` | 1 | `uint8` | erro atual |
| `records_expected` | 1 | `uint8` | total esperado no run |
| `current_order` | 1 | `uint8` | ordem do registro pronto |
| `reserved0` | 1 | `uint8` | reservado |
| `run_id` | 4 | `uint32_le` | run atual |
| `payload_len` | 4 | `uint32_le` | payload do registro pronto |
| `crc16` | 2 | `uint16_le` | CRC16-CCITT dos 16 bytes anteriores |

Estados:

```text
0 BOOTING
1 IDLE
2 CAPTURING
3 RECORD_READY
4 DONE
5 ERROR
```

Erros:

```text
0 NONE
1 BUSY
2 BAD_CONFIG
3 CAPTURE_FAIL
4 ENCODE_FAIL
5 ABORTED
6 PROTO
7 CAM_OVF
```

## Record Header Minimo

Tamanho fixo:

```text
EXP_RECORD_HEADER_SIZE = 24
EXP_RECORD_VERSION = 5
EXP_RECORD_MAGIC = 0x314A4349
```

`EXP_RECORD_MAGIC` corresponde a `"ICJ1"` em little-endian.

Layout:

| Campo | Bytes | Tipo | Uso |
|---|---:|---|---|
| `magic` | 4 | `uint32_le` | `"ICJ1"` |
| `version` | 1 | `uint8` | `5` |
| `header_size` | 1 | `uint8` | `24` |
| `record_type` | 1 | `uint8` | `RAW_FRAME` ou `JPEG_FRAME` |
| `flags` | 1 | `uint8` | inclui CRC de payload |
| `run_id` | 4 | `uint32_le` | run |
| `order_index` | 1 | `uint8` | ordem real no run |
| `method_id` | 1 | `uint8` | metodo |
| `mode_type` | 1 | `uint8` | `RAW` ou `JPEG` |
| `k_idx` | 1 | `uint8` | indice de fator |
| `payload_len` | 4 | `uint32_le` | bytes de payload |
| `crc32` | 4 | `uint32_le` | CRC32 do payload |

O S3 infere `seq_num`, `width`, `height`, `colorspace`, `subsampling` e
`records_expected` a partir da configuracao do experimento (`QVGA`, `RGB`,
`4:4:4` para JPEG atual e ordem esperada do run). Esses campos nao precisam ir
no header da imagem.

## Telemetria CAM

Depois do payload, a CAM auto-arma a telemetria e o S3 le uma estrutura
separada:

```text
EXP_RECORD_TELEMETRY_SIZE = 36
EXP_RECORD_TELEMETRY_VERSION = 1
EXP_RECORD_TELEMETRY_MAGIC = 0x544D
```

Layout:

| Campo | Bytes | Tipo | Uso |
|---|---:|---|---|
| `magic` | 2 | `uint16_le` | `"MT"` |
| `version` | 1 | `uint8` | `1` |
| `size` | 1 | `uint8` | `36` |
| `t_capture_us` | 4 | `uint32_le` | tempo de captura |
| `t_rgb565_to_rgb888_us` | 4 | `uint32_le` | tempo de conversao |
| `t_prepare_us` | 4 | `uint32_le` | pack RAW ou encode RLE |
| `t_algorithm_us` | 4 | `uint32_le` | compressao JPEG |
| `t_decompress_us` | 4 | `uint32_le` | decompressao CAM para PSNR local |
| `t_dct_kernel_us` | 4 | `uint32_le` | tempo acumulado dos kernels DCT na CAM |
| `psnr_x100` | 2 | `uint16_le` | PSNR da CAM multiplicado por 100 |
| `dct_kernel_calls` | 2 | `uint16_le` | chamadas DCT 8x8 medidas na CAM |
| `crc16` | 2 | `uint16_le` | CRC16-CCITT dos 32 bytes anteriores |
| `reserved0` | 2 | `uint16_le` | reservado |

Essa telemetria preserva as metricas, mas nao entra em `tx_us` nem em
`frame_bytes`, que representam o caminho da imagem.

Tipos:

```text
EXP_RECORD_RAW_FRAME  = 1
EXP_RECORD_JPEG_FRAME = 2

EXP_MODE_RAW  = 1
EXP_MODE_JPEG = 2
```

## Metodos e Slots

IDs:

```text
0 RAW
1 LOEFFLER
2 MATRIX
3 RDCT
4 SILVEIRA_J3
5 SILVEIRA_J7
6 ALL
```

Fatores:

```text
k_idx 0 -> 0.1
k_idx 1 -> 0.2
k_idx 2 -> 0.5
k_idx 3 -> 0.8
```

Slots no S3:

```text
slot 0 = RAW
slot = 1 + k_idx * 5 + method_index
```

Onde `method_index` e:

```text
0 loeffler
1 matrix
2 rdct
3 silveira_j3
4 silveira_j7
```

Assim:

```text
EXP_RECORD_SLOT_COUNT = 21
```

## Payload RAW

Formato:

```text
RGB888 puro
[R][G][B][R][G][B]...
```

Tamanho para QVGA:

```text
320 * 240 * 3 = 230400 bytes
```

O `RAW` e usado como referencia oficial do run quando presente.

## Payload JPEG

O payload JPEG enviado no prototipo atual e RLE puro:

```text
[Y_RLE...][Cb_RLE...][Cr_RLE...]
```

O contexto necessario para decodificar vem do header minimo e da configuracao
do experimento (`method`, `k`, `QVGA`, `RGB`, `4:4:4`). O codec ainda possui e
aceita o frame interno v2 para compatibilidade fora do transporte minimo:

```text
[0xA7][version=2][LEN_BE32][payload...]
```

O decoder ainda aceita frame v1 legado:

```text
[0xA5][LEN_BE16][payload...]
```

Mas o firmware atual da CAM/S3 usa o payload RLE puro no SPI.

## Chunking de Payload

Chunk logico atual:

```text
EXP_SPI_PAYLOAD_CHUNK = 512
```

Nas leituras `slave -> master`, o master clocka:

```text
chunk_logico + EXP_SPI_GUARD_BYTES
```

O primeiro chunk pode ser menor quando o fluxo esta ressincronizando ou
quando o firmware decide limitar a janela inicial.

## Regra de Nao Interferencia

Dentro de um run `ALL`:

- a captura fisica acontece uma vez;
- a conversao `RGB565 -> RGB888` acontece uma vez;
- o buffer `RGB888` vira fonte somente-leitura;
- `RAW` apenas empacota/transmite;
- cada metodo JPEG le o mesmo buffer congelado;
- cada fator `k` gera compressao independente;
- o S3 calcula metricas contra a referencia `RAW` do mesmo run.

## Robustez de Header

Comportamento atual contra perda de sincronismo:

- a CAM auto-arma o `record_header` quando entra em `RECORD_READY`;
- o S3 le o header diretamente quando `READY=HIGH`, sem depender de
  `READ_HEADER` no fluxo nominal;
- a CAM nao aborta se esperava header de `24 B` e recebe transacao de status
  de `19 B`;
- a CAM mantem o header armado para a proxima leitura direta;
- o S3 trata status recebido no lugar de header como tentativa incompleta;
- o S3 tenta ler header ate `20` vezes;
- o S3 valida `order_index` antes de aceitar o registro.

Em bancada, alguns comandos de leitura chegaram curtos na CAM (`actual_len`
menor que `18 B`), embora o S3 ainda recebesse um status valido. Para nao
perder o run, a CAM aceita comandos curtos somente para os reads idempotentes
`READ_HEADER`, `READ_PAYLOAD` e `READ_TELEMETRY`, usando apenas
`magic/version/cmd`. Esses comandos continuam no contrato para compatibilidade
e diagnostico, mas o caminho nominal usa auto-arm para `HEADER`, `PAYLOAD` e
`TELEMETRY`. Comandos que alteram configuracao ou iniciam captura continuam
exigindo o frame completo com CRC16.

## Resumo do CSV

O S3 gera `runXXXX_summary.csv` e tambem anexa ao `summary.csv` global.

Colunas:

```text
image,method,k,psnr,ssim,bpp,compress_us,decompress_us,
dct_kernel_us,idct_kernel_us,dct_kernel_calls,idct_kernel_calls,tx_us,
frame_bytes,tx_raw_us,raw_bytes,compression_ratio,sha256_input_first8,
status,status_detalhado
```

`bpp` e calculado a partir dos bytes reais de stream. `compression_ratio`
usa o tamanho RAW como referencia. `sha256_input_first8` identifica a imagem
de referencia usada no run.

`frame_bytes` representa `payload_len + 24 B` do header minimo da imagem. A
telemetria CAM de `36 B` nao entra em `frame_bytes` nem em `tx_us`.

`dct_kernel_us` e `idct_kernel_us` isolam as chamadas reais aos kernels DCT e
IDCT 8x8. Conversao de cor, quantizacao, dequantizacao, RLE, frame e SPI ficam
fora desses dois campos.

## Arquivo de Referencia do Codigo

- [prototype/common/experiment_protocol.h](../prototype/common/experiment_protocol.h)
