/**
 * @file tft.c
 * @brief Driver ILI9341/ST7789V - SPI via ESP-IDF, 320×240 landscape.
 *
 * Pinos (PCB usinada - IMUTÁVEL):
 *   MOSI=IO36  SCK=IO37  MISO=IO35  CS=IO45  DC=IO48  RST=3.3V
 *
 * v2 - mudanças:
 *   • DMA habilitado (SPI_DMA_CH_AUTO, 4092 bytes/chunk); fallback sem DMA
 *   • Clock scaling: tenta 4 MHz → 2 MHz → 500 kHz
 *   • Auto-detecção ILI9341 vs ST7789V via leitura 0xD3
 *   • Se MISO flutuante (tudo 0xFF): tenta ILI9341 (RED 2s) e depois
 *     ST7789V (BLUE 2s) - usuário observa qual funcionou
 *   • do_ili9341_regs() e do_st7789_regs() isoladas para re-init limpo
 *   • SW reset aumentado para 500 ms
 *   • Log corrigido: mostra clock real (não "10 MHz" fixo)
 *   • tft_fill_rect e tft_draw_image_rgb888 usam s_dma_buf diretamente
 */

#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tft.h"

static const char *TAG = "tft";

/* ── Estado interno ─────────────────────────────────────────────────────── */
static spi_device_handle_t s_spi        = NULL;
static bool                s_use_dma    = false;
static int                 s_chunk_max  = 64;      /* 64 sem DMA, 4092 com DMA */
static tft_controller_t    s_controller = TFT_CTRL_UNKNOWN;
static int                 s_spi_freq   = 0;       /* Hz real usado */

/* Buffer DMA - alinhado a 4 bytes (obrigatório para DMA no ESP32-S3).
 * 4092 bytes = 2046 pixels RGB565 por transação com DMA.                   */
#define TFT_DMA_BUF_SZ  4092
#define TFT_NODMA_BUF_SZ  64
#define TFT_CMD_BUF_SZ  4
#define TFT_DATA_BUF_SZ 32
static uint8_t *s_dma_buf = NULL;
static uint8_t *s_cmd_buf = NULL;
static uint8_t *s_dat_buf = NULL;

#define TFT_PIN_IN_OCTAL_SPIRAM_RANGE(pin) ((pin) >= 33 && (pin) <= 37)

#if CONFIG_SPIRAM && \
    (TFT_PIN_IN_OCTAL_SPIRAM_RANGE(TFT_MOSI_PIN) || \
     TFT_PIN_IN_OCTAL_SPIRAM_RANGE(TFT_SCK_PIN) || \
     TFT_PIN_IN_OCTAL_SPIRAM_RANGE(TFT_MISO_PIN))
#define TFT_PINS_CONFLICT_WITH_SPIRAM 1
#else
#define TFT_PINS_CONFLICT_WITH_SPIRAM 0
#endif

/* ── Fonte 5×7 (ASCII 0x20–0x7E, estilo Adafruit, 5 bytes/glifo) ─────── */
static const uint8_t s_font[][5] = {
    { 0x00,0x00,0x00,0x00,0x00 }, // ' ' 0x20
    { 0x00,0x00,0x5F,0x00,0x00 }, // !
    { 0x00,0x07,0x00,0x07,0x00 }, // "
    { 0x14,0x7F,0x14,0x7F,0x14 }, // #
    { 0x24,0x2A,0x7F,0x2A,0x12 }, // $
    { 0x23,0x13,0x08,0x64,0x62 }, // %
    { 0x36,0x49,0x55,0x22,0x50 }, // &
    { 0x00,0x05,0x03,0x00,0x00 }, // '
    { 0x00,0x1C,0x22,0x41,0x00 }, // (
    { 0x00,0x41,0x22,0x1C,0x00 }, // )
    { 0x08,0x2A,0x1C,0x2A,0x08 }, // *
    { 0x08,0x08,0x3E,0x08,0x08 }, // +
    { 0x00,0x50,0x30,0x00,0x00 }, // ,
    { 0x08,0x08,0x08,0x08,0x08 }, // -
    { 0x00,0x60,0x60,0x00,0x00 }, // .
    { 0x20,0x10,0x08,0x04,0x02 }, // /
    { 0x3E,0x51,0x49,0x45,0x3E }, // 0
    { 0x00,0x42,0x7F,0x40,0x00 }, // 1
    { 0x42,0x61,0x51,0x49,0x46 }, // 2
    { 0x21,0x41,0x45,0x4B,0x31 }, // 3
    { 0x18,0x14,0x12,0x7F,0x10 }, // 4
    { 0x27,0x45,0x45,0x45,0x39 }, // 5
    { 0x3C,0x4A,0x49,0x49,0x30 }, // 6
    { 0x01,0x71,0x09,0x05,0x03 }, // 7
    { 0x36,0x49,0x49,0x49,0x36 }, // 8
    { 0x06,0x49,0x49,0x29,0x1E }, // 9
    { 0x00,0x36,0x36,0x00,0x00 }, // :
    { 0x00,0x56,0x36,0x00,0x00 }, // ;
    { 0x00,0x08,0x14,0x22,0x41 }, // <
    { 0x14,0x14,0x14,0x14,0x14 }, // =
    { 0x41,0x22,0x14,0x08,0x00 }, // >
    { 0x02,0x01,0x51,0x09,0x06 }, // ?
    { 0x32,0x49,0x79,0x41,0x3E }, // @
    { 0x7E,0x11,0x11,0x11,0x7E }, // A
    { 0x7F,0x49,0x49,0x49,0x36 }, // B
    { 0x3E,0x41,0x41,0x41,0x22 }, // C
    { 0x7F,0x41,0x41,0x22,0x1C }, // D
    { 0x7F,0x49,0x49,0x49,0x41 }, // E
    { 0x7F,0x09,0x09,0x01,0x01 }, // F
    { 0x3E,0x41,0x41,0x51,0x32 }, // G
    { 0x7F,0x08,0x08,0x08,0x7F }, // H
    { 0x00,0x41,0x7F,0x41,0x00 }, // I
    { 0x20,0x40,0x41,0x3F,0x01 }, // J
    { 0x7F,0x08,0x14,0x22,0x41 }, // K
    { 0x7F,0x40,0x40,0x40,0x40 }, // L
    { 0x7F,0x02,0x04,0x02,0x7F }, // M
    { 0x7F,0x04,0x08,0x10,0x7F }, // N
    { 0x3E,0x41,0x41,0x41,0x3E }, // O
    { 0x7F,0x09,0x09,0x09,0x06 }, // P
    { 0x3E,0x41,0x51,0x21,0x5E }, // Q
    { 0x7F,0x09,0x19,0x29,0x46 }, // R
    { 0x46,0x49,0x49,0x49,0x31 }, // S
    { 0x01,0x01,0x7F,0x01,0x01 }, // T
    { 0x3F,0x40,0x40,0x40,0x3F }, // U
    { 0x1F,0x20,0x40,0x20,0x1F }, // V
    { 0x7F,0x20,0x18,0x20,0x7F }, // W
    { 0x63,0x14,0x08,0x14,0x63 }, // X
    { 0x03,0x04,0x78,0x04,0x03 }, // Y
    { 0x61,0x51,0x49,0x45,0x43 }, // Z
    { 0x00,0x00,0x7F,0x41,0x41 }, // [
    { 0x02,0x04,0x08,0x10,0x20 }, // '\'
    { 0x41,0x41,0x7F,0x00,0x00 }, // ]
    { 0x04,0x02,0x01,0x02,0x04 }, // ^
    { 0x40,0x40,0x40,0x40,0x40 }, // _
    { 0x00,0x01,0x02,0x04,0x00 }, // `
    { 0x20,0x54,0x54,0x54,0x78 }, // a
    { 0x7F,0x48,0x44,0x44,0x38 }, // b
    { 0x38,0x44,0x44,0x44,0x20 }, // c
    { 0x38,0x44,0x44,0x48,0x7F }, // d
    { 0x38,0x54,0x54,0x54,0x18 }, // e
    { 0x08,0x7E,0x09,0x01,0x02 }, // f
    { 0x08,0x14,0x54,0x54,0x3C }, // g
    { 0x7F,0x08,0x04,0x04,0x78 }, // h
    { 0x00,0x44,0x7D,0x40,0x00 }, // i
    { 0x20,0x40,0x44,0x3D,0x00 }, // j
    { 0x00,0x7F,0x10,0x28,0x44 }, // k
    { 0x00,0x41,0x7F,0x40,0x00 }, // l
    { 0x7C,0x04,0x18,0x04,0x78 }, // m
    { 0x7C,0x08,0x04,0x04,0x78 }, // n
    { 0x38,0x44,0x44,0x44,0x38 }, // o
    { 0x7C,0x14,0x14,0x14,0x08 }, // p
    { 0x08,0x14,0x14,0x18,0x7C }, // q
    { 0x7C,0x08,0x04,0x04,0x08 }, // r
    { 0x48,0x54,0x54,0x54,0x20 }, // s
    { 0x04,0x3F,0x44,0x40,0x20 }, // t
    { 0x3C,0x40,0x40,0x40,0x7C }, // u
    { 0x1C,0x20,0x40,0x20,0x1C }, // v
    { 0x3C,0x40,0x30,0x40,0x3C }, // w
    { 0x44,0x28,0x10,0x28,0x44 }, // x
    { 0x0C,0x50,0x50,0x50,0x3C }, // y
    { 0x44,0x64,0x54,0x4C,0x44 }, // z
    { 0x00,0x08,0x36,0x41,0x00 }, // {
    { 0x00,0x00,0x7F,0x00,0x00 }, // |
    { 0x00,0x41,0x36,0x08,0x00 }, // }
    { 0x08,0x08,0x2A,0x1C,0x08 }, // ~
};

/* ── SPI helpers ─────────────────────────────────────────────────────────── */

/* pre_transfer_cb: seta DC antes de cada transação via t->user.
 * t->user = 0 → comando (DC=LOW), t->user = 1 → dado (DC=HIGH). */
static void IRAM_ATTR pre_transfer_cb(spi_transaction_t *t)
{
    gpio_set_level(TFT_DC_PIN, (int)(intptr_t)t->user);
}

static inline void cs_low(void)  { gpio_set_level(TFT_CS_PIN, 0); }
static inline void cs_high(void) { gpio_set_level(TFT_CS_PIN, 1); }

static bool spi_tx(spi_transaction_t *t)
{
    if (!s_spi) return false;
    cs_low();
    esp_err_t ret = spi_device_transmit(s_spi, t);
    cs_high();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_tx ERR (%d bits, DC=%d): %s",
                 (int)t->length, (int)(intptr_t)t->user, esp_err_to_name(ret));
        return false;
    }
    return true;
}

static bool ensure_io_buffers(void)
{
    if (!s_dma_buf) {
        s_dma_buf = (uint8_t *)heap_caps_malloc(TFT_DMA_BUF_SZ, MALLOC_CAP_DMA);
        if (!s_dma_buf) {
            ESP_LOGE(TAG, "falha ao alocar s_dma_buf (%d bytes)", TFT_DMA_BUF_SZ);
            return false;
        }
    }
    if (!s_cmd_buf) {
        s_cmd_buf = (uint8_t *)heap_caps_malloc(TFT_CMD_BUF_SZ, MALLOC_CAP_DMA);
        if (!s_cmd_buf) {
            ESP_LOGE(TAG, "falha ao alocar s_cmd_buf (%d bytes)", TFT_CMD_BUF_SZ);
            return false;
        }
    }
    if (!s_dat_buf) {
        s_dat_buf = (uint8_t *)heap_caps_malloc(TFT_DATA_BUF_SZ, MALLOC_CAP_DMA);
        if (!s_dat_buf) {
            ESP_LOGE(TAG, "falha ao alocar s_dat_buf (%d bytes)", TFT_DATA_BUF_SZ);
            return false;
        }
    }
    return true;
}

static bool send_cmd(uint8_t cmd)
{
    if (!s_cmd_buf) return false;
    s_cmd_buf[0] = cmd;
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = s_cmd_buf,
        .user      = (void *)0,
    };
    return spi_tx(&t);
}

static void send_data(const uint8_t *data, int len)
{
    if (!s_dat_buf || len <= 0 || len > TFT_DATA_BUF_SZ) return;
    memcpy(s_dat_buf, data, (size_t)len);
    spi_transaction_t t = {
        .length    = (size_t)(8 * len),
        .tx_buffer = s_dat_buf,
        .user      = (void *)1,
    };
    spi_tx(&t);
}

/* Envia pixels já em RGB565 big-endian a partir de s_dma_buf.
 * Garante que len ≤ s_chunk_max por chamada. */
static __attribute__((unused)) void flush_buf(int len)
{
    if (len <= 0 || !s_spi || !s_dma_buf) return;
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > s_chunk_max) chunk = s_chunk_max;
        spi_transaction_t t = {
            .length    = (size_t)(8 * chunk),
            .tx_buffer = s_dma_buf + off,
            .user      = (void *)1,
        };
        spi_tx(&t);
        off += chunk;
    }
}

/* Copia data→s_dma_buf e envia em chunks de ≤s_chunk_max bytes. */
static void write_pixels(const uint8_t *data, int len)
{
    if (!s_dma_buf) return;
    while (len > 0) {
        int chunk = (len > s_chunk_max) ? s_chunk_max : len;
        memcpy(s_dma_buf, data, (size_t)chunk);
        spi_transaction_t t = {
            .length    = (size_t)(8 * chunk),
            .tx_buffer = s_dma_buf,
            .user      = (void *)1,
        };
        spi_tx(&t);
        data += chunk;
        len  -= chunk;
    }
}

/* ── Janela de endereço ──────────────────────────────────────────────────── */

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t caset[4] = { (uint8_t)(x0>>8), (uint8_t)(x0&0xFF),
                         (uint8_t)(x1>>8), (uint8_t)(x1&0xFF) };
    uint8_t paset[4] = { (uint8_t)(y0>>8), (uint8_t)(y0&0xFF),
                         (uint8_t)(y1>>8), (uint8_t)(y1&0xFF) };
    send_cmd(0x2A); send_data(caset, 4);
    send_cmd(0x2B); send_data(paset, 4);
    send_cmd(0x2C);
}

/* ── Leitura de ID via SPI ───────────────────────────────────────────────── */

/* Lê n bytes de resposta APÓS enviar o comando cmd. */
static __attribute__((unused)) void read_id(uint8_t cmd, uint8_t *out, int n)
{
    memset(out, 0, (size_t)n);
    { uint8_t c = cmd;
      spi_transaction_t tc = { .length=8, .tx_buffer=&c, .user=(void*)0 };
      spi_tx(&tc); }
    { spi_transaction_t tr = { .length=(size_t)(8*n), .rxlength=(size_t)(8*n),
                                .rx_buffer=out, .user=(void*)1 };
      spi_tx(&tr); }
}

/* ── Sequência ILI9341 ───────────────────────────────────────────────────── */

static void do_ili9341_regs(void)
{
    /* Unlock extended registers */
    send_cmd(0xEF); send_data((uint8_t[]){0x03,0x80,0x02}, 3);
    send_cmd(0xCF); send_data((uint8_t[]){0x00,0xC1,0x30}, 3);
    send_cmd(0xED); send_data((uint8_t[]){0x64,0x03,0x12,0x81}, 4);
    send_cmd(0xE8); send_data((uint8_t[]){0x85,0x00,0x78}, 3);
    send_cmd(0xCB); send_data((uint8_t[]){0x39,0x2C,0x00,0x34,0x02}, 5);
    send_cmd(0xF7); send_data((uint8_t[]){0x20}, 1);
    send_cmd(0xEA); send_data((uint8_t[]){0x00,0x00}, 2);
    /* Power control */
    send_cmd(0xC0); send_data((uint8_t[]){0x23}, 1);
    send_cmd(0xC1); send_data((uint8_t[]){0x10}, 1);
    send_cmd(0xC5); send_data((uint8_t[]){0x3E,0x28}, 2);
    send_cmd(0xC7); send_data((uint8_t[]){0x86}, 1);
    /* MADCTL: MY=1, MX=1, MV=1, BGR=1 → landscape 320×240 orientação correta */
    send_cmd(0x36); send_data((uint8_t[]){0xE8}, 1);
    send_cmd(0x3A); send_data((uint8_t[]){0x55}, 1);  /* RGB565 */
    send_cmd(0xB1); send_data((uint8_t[]){0x00,0x18}, 2);
    send_cmd(0xB6); send_data((uint8_t[]){0x08,0x82,0x27}, 3);
    send_cmd(0xF2); send_data((uint8_t[]){0x00}, 1);
    send_cmd(0x26); send_data((uint8_t[]){0x01}, 1);
    /* Gamma positiva */
    send_cmd(0xE0);
    send_data((uint8_t[]){0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                          0x37,0x07,0x10,0x03,0x0E,0x09,0x00}, 15);
    /* Gamma negativa */
    send_cmd(0xE1);
    send_data((uint8_t[]){0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                          0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F}, 15);
    /* Sleep out */
    send_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    /* Janela completa */
    send_cmd(0x2A); send_data((uint8_t[]){0x00,0x00,0x01,0x3F}, 4);
    send_cmd(0x2B); send_data((uint8_t[]){0x00,0x00,0x00,0xEF}, 4);
    /* Display on */
    send_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "ILI9341 regs aplicados (MADCTL=0xE8, BGR=1)");
}

/* ── Sequência ST7789V ───────────────────────────────────────────────────── */

static void do_st7789_regs(void)
{
    /* SW reset interno (necessário quando chamado como re-init) */
    send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(150));

    send_cmd(0x11);  /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(10));

    send_cmd(0x3A); send_data((uint8_t[]){0x55}, 1);  /* COLMOD: RGB565 */

    /* Porch control */
    send_cmd(0xB2); send_data((uint8_t[]){0x0C,0x0C,0x00,0x33,0x33}, 5);
    /* Gate control */
    send_cmd(0xB7); send_data((uint8_t[]){0x35}, 1);
    /* VCOM */
    send_cmd(0xBB); send_data((uint8_t[]){0x19}, 1);
    /* LCM control */
    send_cmd(0xC0); send_data((uint8_t[]){0x2C}, 1);
    /* VDV/VRH command enable */
    send_cmd(0xC2); send_data((uint8_t[]){0x01,0xFF}, 2);
    /* VRH set */
    send_cmd(0xC3); send_data((uint8_t[]){0x12}, 1);
    /* VDV set */
    send_cmd(0xC4); send_data((uint8_t[]){0x20}, 1);
    /* Frame rate 60 Hz */
    send_cmd(0xC6); send_data((uint8_t[]){0x0F}, 1);
    /* Power control */
    send_cmd(0xD0); send_data((uint8_t[]){0xA4,0xA1}, 2);
    /* Positive gamma */
    send_cmd(0xE0);
    send_data((uint8_t[]){0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,
                          0x4C,0x18,0x0D,0x0B,0x1F,0x23}, 14);
    /* Negative gamma */
    send_cmd(0xE1);
    send_data((uint8_t[]){0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,
                          0x51,0x2F,0x1F,0x1F,0x20,0x23}, 14);

    /* MADCTL: MX=1, MV=1 → landscape 320×240.
     * Se imagem aparecer espelhada, trocar 0x70 por 0x28 (igual ILI9341). */
    send_cmd(0x36); send_data((uint8_t[]){0x70}, 1);

    /* Endereçamento: 320 colunas × 240 linhas */
    send_cmd(0x2A); send_data((uint8_t[]){0x00,0x00,0x01,0x3F}, 4);
    send_cmd(0x2B); send_data((uint8_t[]){0x00,0x00,0x00,0xEF}, 4);

    /* INVON - inversão necessária na maioria dos módulos ST7789V */
    send_cmd(0x21);
    vTaskDelay(pdMS_TO_TICKS(10));

    send_cmd(0x13);  /* NORON - modo normal */
    vTaskDelay(pdMS_TO_TICKS(10));

    send_cmd(0x29);  /* DISPON */
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "ST7789V regs aplicados (MADCTL=0x70, INVON)");
}

/* ── tft_init - GPIO, SPI, detecção, init do controlador ────────────────── */

void tft_init(void)
{
    ESP_LOGI(TAG, "tft_init: MOSI=%d SCK=%d MISO=%d CS=%d DC=%d TOUCH_CS=%d",
             TFT_MOSI_PIN, TFT_SCK_PIN, TFT_MISO_PIN,
             TFT_CS_PIN, TFT_DC_PIN, TOUCH_CS_PIN);

#if TFT_PINS_CONFLICT_WITH_SPIRAM
    ESP_LOGE(TAG,
             "TFT desabilitado: GPIO35/36/37 entram em conflito com flash/PSRAM octal neste S3");
    ESP_LOGE(TAG,
             "TFT pins atuais MOSI=%d SCK=%d MISO=%d; remapeie para GPIO livres ou use alvo sem PSRAM octal",
             TFT_MOSI_PIN, TFT_SCK_PIN, TFT_MISO_PIN);
    s_spi = NULL;
    s_use_dma = false;
    s_chunk_max = TFT_NODMA_BUF_SZ;
    s_controller = TFT_CTRL_UNKNOWN;
    return;
#endif

    /* ── TOUCH_CS (IO42) HIGH - desabilita XPT2046 no barramento ── */
    gpio_config_t io_tcs = {
        .pin_bit_mask = (1ULL << TOUCH_CS_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_tcs));
    gpio_set_level(TOUCH_CS_PIN, 1);
    ESP_LOGI(TAG, "TOUCH_CS IO%d -> HIGH (XPT2046 desabilitado)", TOUCH_CS_PIN);

    /* ── CS (IO45) - GPIO_MODE_INPUT_OUTPUT mantém buffer de leitura ── */
    gpio_reset_pin(TFT_CS_PIN);
    gpio_config_t io_cs = {
        .pin_bit_mask = (1ULL << TFT_CS_PIN),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cs));
    gpio_set_level(TFT_CS_PIN, 1);
    gpio_set_level(TFT_CS_PIN, 0);
    int cs_lo = gpio_get_level(TFT_CS_PIN);
    gpio_set_level(TFT_CS_PIN, 1);
    int cs_hi = gpio_get_level(TFT_CS_PIN);
    ESP_LOGI(TAG, "CS IO%d toggle: LOW=%d HIGH=%d (esperado 0 e 1)",
             TFT_CS_PIN, cs_lo, cs_hi);
    if (cs_lo != 0 || cs_hi != 1)
        ESP_LOGE(TAG, "  *** CS IO%d nao oscila - strapping/curto ***", TFT_CS_PIN);

    /* ── DC (IO48) ── */
    gpio_reset_pin(TFT_DC_PIN);
    gpio_config_t io_dc = {
        .pin_bit_mask = (1ULL << TFT_DC_PIN),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io_dc) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config DC IO%d falhou", TFT_DC_PIN);
        return;
    }
    gpio_set_level(TFT_DC_PIN, 1);
    int dc_hi = gpio_get_level(TFT_DC_PIN);
    gpio_set_level(TFT_DC_PIN, 0);
    int dc_lo = gpio_get_level(TFT_DC_PIN);
    ESP_LOGI(TAG, "DC IO%d toggle: HIGH=%d LOW=%d (esperado 1 e 0)",
             TFT_DC_PIN, dc_hi, dc_lo);
    if (dc_hi != 1 || dc_lo != 0)
        ESP_LOGE(TAG, "  *** DC IO%d nao oscila - pino com defeito ***", TFT_DC_PIN);

    if (!ensure_io_buffers()) {
        return;
    }

    /* ── Barramento SPI - tenta DMA, fallback sem DMA ── */
    spi_bus_config_t bus = {
        .mosi_io_num   = TFT_MOSI_PIN,
        .sclk_io_num   = TFT_SCK_PIN,
        .miso_io_num   = TFT_MISO_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_DMA_BUF_SZ,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret == ESP_OK) {
        s_use_dma   = true;
        s_chunk_max = TFT_DMA_BUF_SZ;
        ESP_LOGI(TAG, "SPI2 com DMA (max %d bytes/chunk)", s_chunk_max);
    } else {
        ESP_LOGW(TAG, "DMA falhou (%s) - modo sem DMA (max 64 bytes)",
                 esp_err_to_name(ret));
        bus.max_transfer_sz = 0;
        ret = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize falhou: %s", esp_err_to_name(ret));
            return;
        }
        s_use_dma   = false;
        s_chunk_max = TFT_NODMA_BUF_SZ;
    }

    /* ── Device SPI - tenta clock decrescente: 4 MHz → 2 MHz → 500 kHz ── */
    static const int freq_table[] = { TFT_SPI_FREQ_HZ, 2000000, 500000 };
    spi_device_interface_config_t dev = {
        .mode         = 0,
        .spics_io_num = -1,   /* CS manual via GPIO */
        .queue_size   = 7,
        .pre_cb       = pre_transfer_cb,
    };
    s_spi = NULL;
    for (int i = 0; i < (int)(sizeof(freq_table)/sizeof(freq_table[0])); i++) {
        dev.clock_speed_hz = freq_table[i];
        ret = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
        if (ret == ESP_OK) {
            s_spi_freq = freq_table[i];
            ESP_LOGI(TAG, "SPI device OK - clock %d Hz", s_spi_freq);
            break;
        }
        ESP_LOGW(TAG, "spi_bus_add_device %d Hz falhou: %s",
                 freq_table[i], esp_err_to_name(ret));
        s_spi = NULL;
    }
    if (!s_spi) {
        ESP_LOGE(TAG, "Nao foi possivel registrar device SPI");
        return;
    }

    /* ── SW Reset global - 500 ms suficiente para todos os controladores ── */
    ESP_LOGI(TAG, "init: SW Reset (0x01) - aguardando 500 ms");
    send_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* ── Detecção de controlador via 0xD3 (RDID4) ── */
#if TFT_FORCE_CONTROLLER == 1
    ESP_LOGI(TAG, "Controlador FORÇADO: ILI9341 (TFT_FORCE_CONTROLLER=1)");
    s_controller = TFT_CTRL_ILI9341;
#elif TFT_FORCE_CONTROLLER == 2
    ESP_LOGI(TAG, "Controlador FORÇADO: ST7789V (TFT_FORCE_CONTROLLER=2)");
    s_controller = TFT_CTRL_ST7789V;
#else
    {
        uint8_t r04[4] = {0};
        uint8_t rd3[5] = {0};
        read_id(0x04, r04, 4);
        read_id(0xD3, rd3, 5);

        ESP_LOGI(TAG, "DIAG 0x04: %02X %02X %02X %02X", r04[0],r04[1],r04[2],r04[3]);
        ESP_LOGI(TAG, "DIAG 0xD3: %02X %02X %02X %02X %02X",
                 rd3[0],rd3[1],rd3[2],rd3[3],rd3[4]);

        bool all_ff = (rd3[0]==0xFF && rd3[1]==0xFF && rd3[2]==0xFF &&
                       rd3[3]==0xFF && rd3[4]==0xFF);

        if (rd3[3] == 0x93 && rd3[4] == 0x41) {
            ESP_LOGI(TAG, "  → ILI9341 identificado (0xD3[3:4]=93 41)");
            s_controller = TFT_CTRL_ILI9341;
        } else if (r04[1] == 0x85 || r04[2] == 0x85) {
            ESP_LOGI(TAG, "  → ST7789V identificado (0x04 byte=0x85)");
            s_controller = TFT_CTRL_ST7789V;
        } else if (all_ff) {
            ESP_LOGW(TAG, "  → Tudo 0xFF: MISO flutuante, controlador desconhecido");
            ESP_LOGW(TAG, "  → Tentando ILI9341 primeiro, depois ST7789V");
            s_controller = TFT_CTRL_UNKNOWN;
        } else {
            ESP_LOGW(TAG, "  → ID nao reconhecido - assumindo ILI9341");
            s_controller = TFT_CTRL_ILI9341;
        }
    }
#endif

    /* ── Init do controlador ── */
    if (s_controller == TFT_CTRL_ILI9341) {
        do_ili9341_regs();
        tft_fill(TFT_BLACK);
    } else if (s_controller == TFT_CTRL_ST7789V) {
        do_st7789_regs();
        tft_fill(TFT_BLACK);
    } else {
        /* Controlador desconhecido: tenta ILI9341 (confirmado em hardware). */
        ESP_LOGW(TAG, "UNKNOWN: assumindo ILI9341 (definir TFT_FORCE_CONTROLLER se errado)");
        do_ili9341_regs();
        s_controller = TFT_CTRL_ILI9341;
        tft_fill(TFT_BLACK);
    }

    const char *ctrl_str = (s_controller == TFT_CTRL_ILI9341) ? "ILI9341" :
                           (s_controller == TFT_CTRL_ST7789V)  ? "ST7789V" : "UNKNOWN";
    ESP_LOGI(TAG, "TFT iniciado: %s %dx%d  clock=%d Hz  DMA=%s",
             ctrl_str, TFT_WIDTH, TFT_HEIGHT, s_spi_freq,
             s_use_dma ? "sim" : "nao");
}

tft_controller_t tft_get_controller(void)
{
    return s_controller;
}

/* ── Funções de desenho públicas ─────────────────────────────────────────── */

void tft_fill(uint16_t color)
{
    tft_fill_rect(0, 0, TFT_WIDTH, TFT_HEIGHT, color);
}

void tft_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_spi || !s_dma_buf || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TFT_WIDTH)  w = TFT_WIDTH  - x;
    if (y + h > TFT_HEIGHT) h = TFT_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    set_window((uint16_t)x, (uint16_t)y,
               (uint16_t)(x+w-1), (uint16_t)(y+h-1));

    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);

    /* Preenche o buffer de DMA com pixels repetidos */
    int fill_px = s_chunk_max / 2;
    for (int i = 0; i < fill_px; i++) {
        s_dma_buf[i*2]   = hi;
        s_dma_buf[i*2+1] = lo;
    }

    int total = w * h;
    while (total > 0) {
        int px = (total > fill_px) ? fill_px : total;
        spi_transaction_t t = {
            .length    = (size_t)(8 * px * 2),
            .tx_buffer = s_dma_buf,
            .user      = (void *)1,
        };
        spi_tx(&t);
        total -= px;
    }
}

void tft_draw_char(int x, int y, char ch, int scale, uint16_t fg, uint16_t bg)
{
    if (!s_spi) return;
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;

    const uint8_t *glyph = s_font[(uint8_t)ch - 0x20];
    int cw   = FONT_W * scale;
    int ch_h = FONT_H * scale;

    set_window((uint16_t)x, (uint16_t)y,
               (uint16_t)(x+cw-1), (uint16_t)(y+ch_h-1));

    /* row_buf: até scale=4: 6*4*2 = 48 bytes - bem abaixo do s_chunk_max */
    uint8_t row_buf[FONT_W * 4 * 2];

    for (int row = 0; row < 7; row++) {
        uint16_t pixels[FONT_W];
        for (int col = 0; col < 5; col++)
            pixels[col] = ((glyph[col] >> row) & 1) ? fg : bg;
        pixels[5] = bg;

        for (int sr = 0; sr < scale; sr++) {
            int pos = 0;
            for (int col = 0; col < FONT_W; col++) {
                uint8_t phi = (uint8_t)(pixels[col] >> 8);
                uint8_t plo = (uint8_t)(pixels[col] & 0xFF);
                for (int sc = 0; sc < scale; sc++) {
                    row_buf[pos++] = phi;
                    row_buf[pos++] = plo;
                }
            }
            write_pixels(row_buf, cw * 2);
        }
    }
    /* Gap inferior */
    uint8_t hi = (uint8_t)(bg >> 8), lo = (uint8_t)(bg & 0xFF);
    int gap = (cw * 2 <= (int)sizeof(row_buf)) ? cw * 2 : (int)sizeof(row_buf);
    for (int i = 0; i < gap; i += 2) { row_buf[i] = hi; row_buf[i+1] = lo; }
    for (int sr = 0; sr < scale; sr++) write_pixels(row_buf, cw * 2);
}

int tft_draw_string(int x, int y, const char *str, int scale,
                    uint16_t fg, uint16_t bg)
{
    if (!str || !s_spi) return x;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    int cx = x;
    for (; *str; str++) {
        if (*str == '\n') { cx = x; y += FONT_H * scale; continue; }
        tft_draw_char(cx, y, *str, scale, fg, bg);
        cx += FONT_W * scale;
    }
    return cx;
}

void tft_draw_image_rgb888(int x, int y, int w, int h, const uint8_t *rgb888)
{
    if (!rgb888 || !s_spi || !s_dma_buf || w <= 0 || h <= 0) return;

    set_window((uint16_t)x, (uint16_t)y,
               (uint16_t)(x+w-1), (uint16_t)(y+h-1));

    int total = w * h;
    int pos   = 0;
    /* Deixa 2 bytes de margem para o último pixel não causar overflow */
    int cap   = s_chunk_max - 2;

    for (int i = 0; i < total; i++) {
        uint8_t  r  = rgb888[i*3+0];
        uint8_t  g  = rgb888[i*3+1];
        uint8_t  b  = rgb888[i*3+2];
        uint16_t px = ((uint16_t)(r & 0xF8) << 8)
                    | ((uint16_t)(g & 0xFC) << 3)
                    |  (uint16_t)(b >> 3);
        s_dma_buf[pos++] = (uint8_t)(px >> 8);
        s_dma_buf[pos++] = (uint8_t)(px & 0xFF);
        if (pos >= cap) {
            spi_transaction_t t = {
                .length    = (size_t)(8 * pos),
                .tx_buffer = s_dma_buf,
                .user      = (void *)1,
            };
            spi_tx(&t);
            pos = 0;
        }
    }
    if (pos > 0) {
        spi_transaction_t t = {
            .length    = (size_t)(8 * pos),
            .tx_buffer = s_dma_buf,
            .user      = (void *)1,
        };
        spi_tx(&t);
    }
}
