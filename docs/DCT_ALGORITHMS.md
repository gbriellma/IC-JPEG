# Algoritmos DCT e Regime Numérico

O codec usa dois regimes aritmeticos: Loeffler e Matrix rodam internamente em
`float` (precisao simples); os metodos aproximados (RDCT, Silveira) e o codec
em geral permanecem em `int32`. A interface publica (`int32_t *in/out`) e a
mesma para todos. Este documento cobre os metodos DCT e o regime numerico; o
transporte embarcado esta em [protocol.md](protocol.md) e
[FRAME_FORMAT.md](FRAME_FORMAT.md).

## Metodos Disponiveis

| Enum | Arquivo | Aritmetica interna | Papel |
|---|---|---|---|
| `JPEG_DCT_LOEFFLER` | `libimage/src/dct_loeffler.c` | float | DCT exata rapida (butterfly) |
| `JPEG_DCT_MATRIX` | `libimage/src/dct_matrix.c` | float | referencia matricial ortonormal |
| `JPEG_DCT_RDCT` | `libimage/src/dct_approx.c` | int32 | RDCT, Cintra-Bayer |
| `JPEG_DCT_SILVEIRA_J3` | `libimage/src/dct_class.c` | int32 | Silveira et al., instancia j=3 |
| `JPEG_DCT_SILVEIRA_J7` | `libimage/src/dct_class.c` | int32 | Silveira et al., instancia j=7 |
| `JPEG_DCT_IDENTITY` | `libimage/src/dct_identity.c` | int32 | validacao |

No firmware embarcado de comparacao, os cinco metodos efetivamente avaliados
sao:

```text
loeffler
matrix
rdct
silveira_j3
silveira_j7
```

`identity` permanece como ferramenta de validacao do codec.

## Escala e Aritmetica

Loeffler e Matrix usam constantes trigonometricas exatas em `float`:

```text
C1 = cos(pi/16),   S1 = sin(pi/16)
C3 = cos(3*pi/16), S3 = sin(3*pi/16)
C6 = cos(6*pi/16), S6 = sin(6*pi/16)
sqrt2 = sqrt(2)
```

O resultado de cada coeficiente e arredondado para `int32_t` com `roundf()`
(round half away from zero) apenas na saida final, eliminando erros de
arredondamento acumulados que existiam no regime Q13 inteiro.

Os metodos aproximados (RDCT, Silveira) permanecem `int32` puro. O nucleo
aritmetico da transformada aproximada e multiplier-free: escalas pequenas como
2, 3, 4, 6, 8, 9, 16 e 18 sao expressas por somas repetidas nos kernels, nao
por multiplicadores gerais. `SCALE_CONST = 8192` continua presente em
`internal.h` para quantizacao e correcao de norma fora do nucleo da DCT.

## Arredondamento por metodo

| Metodo | Arredondamento |
|---|---|
| Loeffler, Matrix | `roundf()` no output final (float) |
| RDCT, Silveira | divisao inteira simetrica (`codec_div_round_symm`) |
| Identity | sem arredondamento |

## Colorspace

Fluxo principal:

```text
RGB888 -> YCbCr centrado -> blocos 8x8 -> DCT -> quantizacao -> RLE
```

Entrada:

```text
R, G, B em [0, 255]
```

Saida centrada aproximada:

```text
Y  em [-128, 127]
Cb em [-128, 127]
Cr em [-128, 127]
```

No firmware embarcado, a camera entrega `RGB565`, que e convertido para
`RGB888` antes de chamar o codec.

## Matrix DCT

Caracteristicas:

- referencia ortonormal direta (produto interno com a matriz completa);
- maior custo aritmetico: 64 multiplicacoes float por 1D;
- util como baseline matematico exato;
- matriz float `CF[8][8]` com `alpha_0 = 1/sqrt(8)`, `alpha_k = 0.5` para `k > 0`.

A matriz e ortogonal, entao a IDCT e simplesmente a transposta (mesmo codigo,
indices invertidos).

No benchmark, `Matrix` e usada para comparar contra `Loeffler` sem mudar
quantizacao, RLE ou frame.

## Loeffler DCT

Caracteristicas:

- DCT exata rapida, estrutura butterfly;
- 11 multiplicacoes float por DCT 1D;
- constantes trigonometricas exatas (sem quantizacao Q13).

Papel experimental:

- referencia pratica de DCT exata;
- tende a ter qualidade quase indistinguivel de `Matrix` (diferenca de
  no maximo 1 LSB no arredondamento final);
- menor custo aritmetico que `Matrix`.

## RDCT

Caracteristicas:

- transformada aproximada;
- sem multiplicadores no nucleo da transformada;
- baseada na familia de aproximacoes tipo Cintra-Bayer;
- usa adicoes/subtracoes no forward;
- a inversa usa os fatores pequenos da normalizacao via somas repetidas e uma
  divisao inteira simetrica final.

Papel experimental:

- baseline aproximado de baixa complexidade;
- comparado principalmente contra `Silveira j=3` e `Silveira j=7`;
- util para medir ganho de qualidade de aproximacoes mais recentes.

## Silveira j=3 e j=7

Caracteristicas:

- transformadas aproximadas sem multiplicadores no nucleo;
- implementadas em `libimage/src/dct_class.c`;
- usam correcao de norma compativel com o pipeline inteiro;
- `j=7` e implementada como `2*T(a)` na passada 1D para representar os
  coeficientes `1/2` da Tabela 1 do artigo sem truncamento por shift;
- o fator `2` de `j=7` e implementado por soma (`x+x`) para evitar
  multiplicadores e evitar shift-left em valores assinados negativos;
- a correcao de norma da quantizacao usa as normas efetivas desse kernel, o
  que equivale a aplicar um fator extra `4` na tabela 2D.

Interpretacao:

- `j=3` e mais simples e tende a ser mais rapida;
- `j=7` e a instancia recomendada para melhor compromisso qualidade/custo;
- ambas sao avaliadas sob a mesma quantizacao e o mesmo RLE dos demais
  metodos.

## Quantizacao

Fatores atuais:

```text
k = 0.1, 0.2, 0.5, 0.8
```

No protocolo embarcado:

```text
k_idx 0 -> 0.1
k_idx 1 -> 0.2
k_idx 2 -> 0.5
k_idx 3 -> 0.8
```

O fator `k` escala a tabela base. O mesmo conjunto e usado no benchmark host e
no modo `ALL` embarcado atual.

### Faixa do Quality Factor (k)

```text
Faixa experimental:  0.1 – 0.8
Faixa documentada:   (0.0, 32.0]
Limite de overflow:  ~7481 (apply_approx_norm_correction int32)
```

O teto `32.0` foi escolhido com margem de 4× sobre o maximo pratico (`8.0 = baixa
qualidade`). Valores acima de `~7481` causam overflow int32 no produto intermediario
`q * APPROX_NORM_1024[i]` dentro de `apply_approx_norm_correction` para metodos
RDCT e Silveira.

A conversao interna usa `k_fixed = round(k × 1024)` (10 bits de precisao, com
arredondamento). Para os k experimentais os valores de tabela resultantes sao
numericamente identicos ao modo anterior (truncamento), mas o arredondamento e
correto por definicao.

Importante para leitura de tempo: `scale_quant_table()`,
`apply_approx_norm_correction()`, `apply_class_norm_correction()`,
`quantize_fast()` e `dequantize()` pertencem ao pipeline JPEG-like, nao ao
nucleo da transformada. A afirmacao "sem multiplicadores" se refere ao kernel
RDCT/Silveira. A correcao de norma e absorvida nas tabelas de quantizacao, como
no tratamento diagonal discutido nos trabalhos originais, e deve ser tratada
como setup/quantizacao quando a medicao quiser isolar apenas a DCT.

No prototipo embarcado, essa separacao aparece em duas metricas dedicadas:
`t_dct_kernel_us` mede somente as chamadas DCT 8x8 na ESP32-CAM, e
`t_idct_kernel_us` mede somente as chamadas IDCT 8x8 no ESP32-S3. Conversao de
cor, preparo de tabela, correcao de norma, quantizacao, dequantizacao, RLE,
frame e SPI ficam fora desses campos.

## RLE e Zig-Zag

O fluxo serializa coeficientes quantizados em ordem zig-zag:

```text
DC: int16_le
AC: zero_run:uint8 + value:int16_le
```

Planos:

```text
Y_RLE
Cb_RLE
Cr_RLE
```

### Tabelas Zig-Zag

Existem dois arrays complementares (permutacoes inversas):

```text
ZIGZAG_SCAN[zigzag_pos]  = raster_pos  - usado no pipeline RLE (zigzag_rle.c)
ZIGZAG_ORDER[raster_pos] = zigzag_pos  - referencia/Python (quantization.c)
```

O pipeline C (encoder e decoder) usa exclusivamente `ZIGZAG_SCAN`.
`ZIGZAG_ORDER` e exportado para uso em Python/comparacoes externas.

### Seguranca int32 → int16 no RLE

Coeficientes quantizados sao `int32_t` internamente mas serializados como
`int16_t` no wire format. A analise de limites confirma que para imagens de
8-bit o range e seguro:

```text
J7 DC maximo = 4 × 64 × 127 = 32 512 < INT16_MAX (32 767)
J7 DC minimo = 4 × 64 × (-128) = -32 768 = INT16_MIN (exato, seguro)
```

Para coeficientes AC e demais metodos os valores sao ainda menores. O cast
se torna inseguro apenas para entradas com profundidade > 8 bits.

Payload SPI atual:

```text
[Y_RLE...][Cb_RLE...][Cr_RLE...]
```

O codec ainda aceita o frame interno v2 (`[0xA7][version=2][LEN_BE32]`) para
compatibilidade em outros usos, mas o prototipo CAM/S3 transmite o RLE puro.

## Payload e Contexto Externo

O payload RLE nao carrega tudo que o decoder precisa para o ensaio embarcado.
O contexto vem do `record_header` minimo e da configuracao do experimento:

- metodo DCT;
- `k_idx`;
- largura/altura fixas do prototipo;
- colorspace;
- subsampling;
- CRC.

Os metadados de medicao da CAM ficam na telemetria separada, lida apos o
payload para nao aumentar o header da imagem.

## Relacao com o Modo ALL

No modo `ALL`, a mesma imagem `RGB888` e processada por:

```text
5 metodos * 4 fatores k = 20 compressoes JPEG
```

mais:

```text
1 registro RAW de referencia
```

Total:

```text
21 registros
```

Esse desenho isola a comparacao dos metodos. Variacoes de iluminacao,
movimento da cena e ruido temporal da camera nao entram entre um metodo e
outro dentro do mesmo run.

## Faixa Dinamica

Loeffler e Matrix operam em `float` internamente. Nao ha risco de overflow para
blocos 8x8 com pixels em `[-128, 127]`. O resultado final e convertido para
`int32_t` com `roundf()`.

Os metodos aproximados permanecem `int32` puro. O caminho de quantizacao,
zigzag e RLE opera sempre em `int32`. Valores de tempo, contadores e tamanhos
usam tipos adequados fora do codec.

## Consequencia Experimental

`Matrix` e `Loeffler` compartilham:

- entrada;
- escala;
- quantizacao;
- ordem zig-zag;
- RLE;
- frame;
- decoder;
- calculo de metricas.

Mesmo assim, nao sao forçadas a produzir coeficientes identicos. A comparacao
correta e por reconstrucao final: `PSNR`, `SSIM`, `bpp` e tempo.

## Politica para Graficos

No grafico `Matrix vs Loeffler`:

- `PSNR`: 2 casas, `HALF_UP`;
- `SSIM`: 2 casas, `HALF_UP`;
- `bpp`: 2 casas, `HALF_UP`.

Valores so aparecem iguais quando coincidem depois dessa politica.

## Arquivos de Referencia

- [../libimage/include/internal.h](../libimage/include/internal.h)
- [../libimage/include/jpeg_codec.h](../libimage/include/jpeg_codec.h)
- [../libimage/src/dct_loeffler.c](../libimage/src/dct_loeffler.c)
- [../libimage/src/dct_matrix.c](../libimage/src/dct_matrix.c)
- [../libimage/src/dct_approx.c](../libimage/src/dct_approx.c)
- [../libimage/src/dct_class.c](../libimage/src/dct_class.c)
- [../libimage/src/quantization.c](../libimage/src/quantization.c)
- [../libimage/src/zigzag_rle.c](../libimage/src/zigzag_rle.c)
- [ANALISE.md](ANALISE.md)
