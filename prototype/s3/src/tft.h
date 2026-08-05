/**
 * @file tft.h
 * @brief Driver TFT ILI9341/ST7789V - 320×240 landscape, SPI via ESP-IDF.
 *
 * Pin mapping (ESP32-S3, retrabalho físico):
 *   MOSI = IO10   SCK = IO11   MISO = não usado
 *   CS   = IO17   DC  = IO18   RESET = 3.3V (SW reset por software)
 *
 * v2 - novidades:
 *   • TFT_SPI_FREQ_HZ   - clock configurável (default 4 MHz) com fallback
 *   • TFT_FORCE_CONTROLLER - override de controlador em compile-time
 *   • tft_controller_t  - tipo do controlador detectado em runtime
 *   • tft_get_controller() - consulta controlador detectado
 *   • DMA habilitado por padrão (fallback para modo sem DMA)
 *   • Chunks de 4092 bytes com DMA (vs 64 sem DMA)
 */

#pragma once

#include <stdint.h>
#include "driver/spi_master.h"

/* ── Geometria ──────────────────────────────────────────────────────────── */
#define TFT_WIDTH  320
#define TFT_HEIGHT 240

/* ── Pinos de hardware TFT ────────────────────────────────────────────────
 * Nova pinagem após retrabalho físico:
 *   MOSI  = IO10
 *   SCK   = IO11
 *   MISO  = não usado
 *   CS    = IO17
 *   DC    = IO18
 *   RESET = 3.3V
 */
#define TFT_MOSI_PIN  10
#define TFT_SCK_PIN   11
#define TFT_MISO_PIN  -1
#define TFT_CS_PIN    17
#define TFT_DC_PIN    18

/* XPT2046 touch - CS mantido HIGH para não interferir no barramento SPI    */
#define TOUCH_CS_PIN  42

/* ── Configuração SPI ───────────────────────────────────────────────────── */

/**
 * Clock SPI padrão. O driver tenta 4 MHz; se falhar, faz fallback para
 * 2 MHz e depois 500 kHz. PCBs fresadas de alta capacitância podem precisar
 * de valores menores - redefina antes de incluir este header se necessário.
 */
#ifndef TFT_SPI_FREQ_HZ
#define TFT_SPI_FREQ_HZ  4000000   /* 4 MHz padrão */
#endif

/**
 * Controlador forçado em compile-time.
 *   0 → auto-detecção (padrão)
 *   1 → ILI9341 forçado
 *   2 → ST7789V forçado
 * Se MISO estiver flutuante (lê 0xFF), a auto-detecção tenta ILI9341 e
 * depois ST7789V, deixando cada cor por 2 s para observação visual.
 */
#ifndef TFT_FORCE_CONTROLLER
#define TFT_FORCE_CONTROLLER  1   /* 1 = ILI9341 confirmado em hardware */
#endif

/* ── Tipo do controlador detectado ─────────────────────────────────────── */
typedef enum {
    TFT_CTRL_UNKNOWN = 0,
    TFT_CTRL_ILI9341,
    TFT_CTRL_ST7789V,
} tft_controller_t;

/* ── Cores RGB565 comuns ────────────────────────────────────────────────── */
#define TFT_BLACK    0x0000
#define TFT_WHITE    0xFFFF
#define TFT_RED      0xF800
#define TFT_GREEN    0x07E0
#define TFT_BLUE     0x001F
#define TFT_YELLOW   0xFFE0
#define TFT_CYAN     0x07FF
#define TFT_ORANGE   0xFD20
#define TFT_GRAY     0x8410
#define TFT_DARKGRAY 0x4208

/* ── Fonte 5×7 + 1px gap = célula 6×8 ──────────────────────────────────── */
#define FONT_W  6
#define FONT_H  8

/* ── API pública ────────────────────────────────────────────────────────── */

/** Inicializa SPI + controlador. Deve ser chamado uma vez antes de qualquer draw. */
void tft_init(void);

/** Retorna o controlador detectado/usado (válido após tft_init). */
tft_controller_t tft_get_controller(void);

/** Preenche a tela inteira com uma cor sólida. */
void tft_fill(uint16_t color);

/** Preenche um retângulo com cor sólida. */
void tft_fill_rect(int x, int y, int w, int h, uint16_t color);

/**
 * Desenha um caractere em (x, y).
 * @param scale  Fator de escala (1 = 6×8 px, 2 = 12×16 px …)
 * @param fg     Cor do primeiro plano (RGB565)
 * @param bg     Cor do fundo (RGB565)
 */
void tft_draw_char(int x, int y, char c, int scale, uint16_t fg, uint16_t bg);

/**
 * Desenha uma string terminada em '\0'. Avanços de linha (\n) descem de linha.
 * Retorna a posição x após o último caractere.
 */
int tft_draw_string(int x, int y, const char *str, int scale,
                    uint16_t fg, uint16_t bg);

/**
 * Desenha uma imagem a partir de um buffer RGB888 (3 bytes/pixel: R,G,B).
 * O buffer deve ter w*h*3 bytes.
 */
void tft_draw_image_rgb888(int x, int y, int w, int h, const uint8_t *rgb888);
