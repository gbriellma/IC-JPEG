/**
 * @file main.c  (ESP32-S3)
 * @brief Controlador principal - UI, link SPI com a CAM, métricas e persistência SD.
 *
 * O S3 coordena um ensaio multi-registro a partir de uma captura única na CAM.
 * Cada registro recebido carrega metadados completos do ramo RAW ou JPEG
 * (LOEFFLER, MATRIX, RDCT, SILVEIRA_J3, SILVEIRA_J7), permitindo reconstrução, validação por CRC,
 * medição isolada de tempos e salvamento individual/agrupado no SD.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "led_strip.h"

#include "jpeg_codec.h"
#include "experiment_protocol.h"
#include "tft.h"
#include "sd_log.h"
#include "metrics.h"

static const char *TAG = "s3";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Pinos e constantes
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SPI_CAM_MISO_PIN 12         /* S3 IO12 ← CAM IO12 */
#define SPI_CAM_MOSI_PIN 13         /* S3 IO13 → CAM IO13 */
#define SPI_CAM_SCLK_PIN 14         /* S3 IO14 → CAM IO14 */
#define SPI_CAM_CS_PIN    6         /* S3 IO6  → CAM IO15 */
#define SPI_CAM_READY_PIN EXP_SPI_S3_READY_PIN
#define FLASH_PIN        -1         /* sem flash diagnostico no S3 */

#define SPI_CAM_HOST    SPI3_HOST
#define SPI_CAM_MODE      EXP_SPI_MODE
#define SPI_CAM_FREQ_HZ  ((int)EXP_SPI_FREQ_HZ)
#define SPI_POLL_TIMEOUT_MS      7000
#define SPI_TRIGGER_TIMEOUT_MS   15000
#define SPI_STATUS_WAIT_MS       100
#define SPI_XFER_RETRY_TIMEOUT_MS 300
#define SPI_XFER_TIMEOUT_MS      500
#define SPI_XFER_TIMEOUT_TICKS   ((TickType_t)((pdMS_TO_TICKS(SPI_XFER_TIMEOUT_MS) > 0) ? pdMS_TO_TICKS(SPI_XFER_TIMEOUT_MS) : 1))
#define SPI_PAYLOAD_XFER_TIMEOUT_MS    500
#define SPI_PAYLOAD_XFER_TIMEOUT_TICKS ((TickType_t)((pdMS_TO_TICKS(SPI_PAYLOAD_XFER_TIMEOUT_MS) > 0) ? pdMS_TO_TICKS(SPI_PAYLOAD_XFER_TIMEOUT_MS) : 1))
#define SPI_RETRY_DELAY_MS       5
#define SPI_RETRY_DELAY_TICKS    ((TickType_t)((pdMS_TO_TICKS(SPI_RETRY_DELAY_MS) > 0) ? pdMS_TO_TICKS(SPI_RETRY_DELAY_MS) : 1))
#define SPI_ARM_SETTLE_MS        2
#define SPI_ARM_SETTLE_TICKS     ((TickType_t)((pdMS_TO_TICKS(SPI_ARM_SETTLE_MS) > 0) ? pdMS_TO_TICKS(SPI_ARM_SETTLE_MS) : 1))
#define SPI_HEADER_READ_ATTEMPTS 20u
#define SPI_PAYLOAD_FIRST_CHUNK  64u
#define SPI_PAYLOAD_STREAM_CHUNK EXP_SPI_PAYLOAD_CHUNK
#ifndef SPI_PAYLOAD_GAP_MS
#define SPI_PAYLOAD_GAP_MS       5u
#endif
#define SPI_PAYLOAD_GAP_TICKS    ((TickType_t)((pdMS_TO_TICKS(SPI_PAYLOAD_GAP_MS) > 0) ? pdMS_TO_TICKS(SPI_PAYLOAD_GAP_MS) : 1))
#define SPI_PAYLOAD_PROGRESS_STEP_BYTES (32u * 1024u)
#define SPI_INVALID_STATUS_WARN_ATTEMPTS 3
#define SPI_DMA_ALLOC_CAPS      (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define DECODE_WORKSPACE_ALIGN  16u
#define DECODE_WORKSPACE_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_32BIT)

#define BTN_A_PIN       3
#define BTN_B_PIN       46    /* strapping pin */
#define BTN_C_PIN       47
#define BTN_D_PIN       21
#define DEBOUNCE_MS     50

#define LED_PIN         8

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constantes do protocolo
 * ═══════════════════════════════════════════════════════════════════════════ */

#define NUM_METHODS      6      /* LOEFFLER, MATRIX, RDCT, SILVEIRA_J3, SILVEIRA_J7, ALL */

#define RECORD_HEADER_TIMEOUT_MS  60000
#define RECORD_HEADER_STATUS_LOG_POLLS 10u
#define RECORD_HEADER_MAX_POLL_FAILURES 5u

/* ═══════════════════════════════════════════════════════════════════════════
 *  Tabelas
 * ═══════════════════════════════════════════════════════════════════════════ */

static const uint8_t METHOD_ORDER[NUM_METHODS] = {
    EXP_METHOD_LOEFFLER,
    EXP_METHOD_MATRIX,
    EXP_METHOD_RDCT,
    EXP_METHOD_SILVEIRA_J3,
    EXP_METHOD_SILVEIRA_J7,
    EXP_METHOD_ALL
};
static const char *METHOD_NAMES[NUM_METHODS] = {
    "Loeffler",
    "Matrix",
    "RDCT",
    "Silv j3",
    "Silv j7",
    "All"
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Estado global
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    STATE_READY,
    STATE_CAPTURING,
    STATE_RECEIVING,
    STATE_DECOMPRESSING,
    STATE_DISPLAY,
    STATE_COMPARE,
} app_state_t;

static app_state_t  g_state       = STATE_READY;
static int          g_method_idx  = 0;
static int          g_k_idx       = 0;
static bool         g_sd_ok       = false;
static bool         g_has_result  = false;
static bool         g_tft_busy    = false;
static bool         g_cam_link_ok = false;   /* link SPI com a CAM pronto */
static uint32_t     g_fallback_run_id = 1;
static bool         g_run_id_persistent = false;
static uint8_t      g_last_display_method = EXP_METHOD_LOEFFLER;
static uint8_t      g_last_display_k_idx = 0u;
static bool         g_uart_csv_header_printed = false;

/* SD: cooldown após falha - evita re-tentativas imediatas que travam o loop */
#define SD_RETRY_COOLDOWN_US  (30LL * 1000 * 1000)   /* 30 s */
static int64_t s_sd_fail_us = 0;   /* timestamp da última falha (0 = nunca falhou) */

/* Guarda de strapping pin IO46 */
static bool         s_boot_guard_done = false;
static TickType_t   s_boot_tick       = 0;

static ExperimentRunResult g_run;
static uint8_t            *s_rx_payload_buf = NULL;
static size_t              s_rx_payload_capacity = 0u;
static bool                s_rx_payload_internal = false;
static jpeg_compressed_t  *s_decode_comp = NULL;
static size_t              s_decode_max_chroma_blocks = 0u;
static size_t              s_decode_max_luma_blocks = 0u;
static spi_device_handle_t s_spi_cam = NULL;
static uint8_t            *s_spi_ctrl_tx_buf = NULL;
static uint8_t            *s_spi_status_rx_buf = NULL;
static uint8_t            *s_spi_header_rx_buf = NULL;
static uint8_t            *s_spi_rx_chunk_buf = NULL;
static uint8_t            *s_spi_dummy_tx_buf = NULL;
static spi_transaction_t   s_spi_trans;
static bool                s_spi_trans_inflight = false;
static bool                s_spi_bitshift_hint_logged = false;

static const char *reset_reason_to_string(esp_reset_reason_t rr)
{
    switch (rr) {
        case ESP_RST_POWERON:   return "POWER-ON";
        case ESP_RST_EXT:       return "PINO EXTERNO";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC / EXCECAO";
        case ESP_RST_INT_WDT:   return "INT WDT";
        case ESP_RST_TASK_WDT:  return "TASK WDT";
        case ESP_RST_WDT:       return "WDT (generico)";
        case ESP_RST_DEEPSLEEP: return "DEEP SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "DESCONHECIDO";
    }
}

static void try_write_boot_debug_log(esp_reset_reason_t rr, const esp_app_desc_t *app)
{
    FILE *f;
    bool mounted_now = false;

    if (!g_sd_ok) {
        ESP_LOGI(TAG, "SD boot probe: tentando montar SD para gravar boot_log.txt");
        if (!sd_init(NULL)) {
            ESP_LOGW(TAG, "SD boot probe: montagem falhou; boot_log.txt nao foi atualizado");
            return;
        }
        g_sd_ok = true;
        mounted_now = true;
    }

    f = fopen(SD_MOUNT "/boot_log.txt", "a");
    if (!f) {
        ESP_LOGE(TAG, "SD boot probe: nao conseguiu abrir %s/boot_log.txt", SD_MOUNT);
        return;
    }

    fprintf(f, "=== boot ===\n");
    fprintf(f, "uptime_ms: %lld\n", esp_timer_get_time() / 1000LL);
    fprintf(f, "reset: %s\n", reset_reason_to_string(rr));
    fprintf(f, "project: %s\n", app ? app->project_name : "unknown");
    fprintf(f, "version: %s\n", app ? app->version : "unknown");
    fprintf(f, "build: %s %s\n", app ? app->date : "?", app ? app->time : "?");
    fprintf(f, "cam_link_ok: %d\n", (int)g_cam_link_ok);
    fprintf(f, "mounted_now: %d\n", (int)mounted_now);
    fprintf(f, "free_internal: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    fprintf(f, "free_psram: %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    fprintf(f, "stack_hwm_bytes: %u\n",
            (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    fprintf(f, "buttons: A=%d B=%d C=%d D=%d\n",
            gpio_get_level(BTN_A_PIN), gpio_get_level(BTN_B_PIN),
            gpio_get_level(BTN_C_PIN), gpio_get_level(BTN_D_PIN));
    fprintf(f, "\n");
    fflush(f);
    fclose(f);

    ESP_LOGI(TAG, "SD boot probe: boot_log.txt atualizado%s",
             mounted_now ? " (com montagem no boot)" : "");
}

static void hex_preview(char *dst, size_t dst_size, const uint8_t *buf, size_t len)
{
    size_t pos = 0;

    if (!dst || dst_size == 0u) return;
    dst[0] = '\0';
    if (!buf || len == 0u) return;

    for (size_t i = 0; i < len; i++) {
        int wrote;

        if (pos + 3u >= dst_size) break;
        wrote = snprintf(dst + pos, dst_size - pos, "%02X", buf[i]);
        if (wrote < 0) break;
        pos += (size_t)wrote;
        if ((i + 1u) < len && pos + 2u < dst_size) {
            dst[pos++] = ' ';
            dst[pos] = '\0';
        }
    }
}

typedef struct {
    int     pin;
    int     raw_level;
    int     stable_level;
    int64_t last_change_us;
    bool    press_latched;
    bool    initialized;
} button_state_t;

static button_state_t s_btn_states[] = {
    { .pin = BTN_A_PIN },
    { .pin = BTN_B_PIN },
    { .pin = BTN_C_PIN },
    { .pin = BTN_D_PIN },
};

static int s_ready_dbg_a = -1;
static int s_ready_dbg_b = -1;
static int s_ready_dbg_c = -1;
static int s_ready_dbg_d = -1;

static void btn_update_state(button_state_t *st);
static bool ensure_rx_payload_buffer_capacity(size_t required, bool prefer_internal);
static bool ensure_run_image_slot(ExperimentRunResult *run, int slot);
static bool ensure_run_image_buffers(ExperimentRunResult *run);
static bool ensure_decode_workspace(void);
static bool compute_decode_block_counts(uint16_t width, uint16_t height,
                                        uint8_t colorspace, uint8_t subsampling,
                                        int32_t *num_luma, int32_t *num_chroma);
static bool prepare_decode_workspace(const exp_record_header_t *hdr,
                                     jpeg_compressed_t **out_comp);
static bool psram_probe_stage(const char *stage);
static bool heap_check_stage(const char *stage);
static jpeg_dct_method_t method_id_to_dct(uint8_t method_id);
static bool ensure_spi_buffers(void);
static void spi_cam_setup(void);
static bool spi_cam_wait_ready(int timeout_ms);
static bool spi_buf_looks_like_status_shifted_left(const uint8_t *buf, size_t len,
                                                   exp_spi_status_t *st_out);
static bool spi_cam_send_config(uint8_t method_id, uint8_t k_idx, uint32_t run_id);
static bool spi_cam_trigger(uint8_t method_id, uint8_t k_idx, uint32_t run_id);
static bool spi_cam_abort(void);
static bool spi_cam_recover_if_error(void);
static bool spi_cam_read_header(exp_record_header_t *hdr);
static bool spi_cam_read_telemetry(exp_record_header_t *hdr);
static bool spi_cam_read_payload(uint32_t payload_len, uint8_t mode_type,
                                 uint32_t *t_spi_rx_us, const char *tag_prefix);
static uint32_t current_run_id(void);
static uint8_t current_method_id(void);
static const char *display_method_name(void);
static void run_reset(ExperimentRunResult *run);
static bool receive_record_header(exp_record_header_t *hdr, const char *tag_prefix,
                                  int expected_order, int expected_count);
static bool receive_record_payload(uint32_t payload_len, uint8_t mode_type,
                                   uint32_t *t_spi_rx_us,
                                   const char *tag_prefix);
static bool process_received_record(ExperimentRunResult *run, const exp_record_header_t *hdr,
                                    uint32_t t_spi_rx_us, uint32_t retry_count);
static void uart_emit_run_csv(const ExperimentRunResult *run);
static void finalize_run_results(ExperimentRunResult *run);
static bool sync_selection_to_cam(void);
static void ui_draw_result_timed(const uint8_t *rgb888, ExperimentRecordResult *r);
static void ui_draw_run_summary_timed(ExperimentRunResult *run);
static bool ensure_sd_mounted(void);
static void do_save_current_run(void);

static bool ensure_run_persistent_id(ExperimentRunResult *run);
static void spi_cam_reset_state(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  LED WS2812B
 * ═══════════════════════════════════════════════════════════════════════════ */

#define LED_OFF      0,  0,  0
#define LED_GREEN    0, 32,  0
#define LED_BLUE     0,  0, 32
#define LED_YELLOW  32, 20,  0
#define LED_RED     32,  0,  0

static led_strip_handle_t g_led = NULL;

static void led_hw_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num         = LED_PIN,
        .max_leds               = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model              = LED_MODEL_WS2812,
        .flags.invert_out       = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &g_led) != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init falhou");
        g_led = NULL;
        return;
    }
    led_strip_set_pixel(g_led, 0, 0, 0, 0);
    led_strip_refresh(g_led);
}

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!g_led || g_tft_busy) return;
    led_strip_set_pixel(g_led, 0, r, g, b);
    led_strip_refresh(g_led);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Wrappers TFT com flag g_tft_busy
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline void tft_begin(void) { g_tft_busy = true;  }
static inline void tft_end(void)   { g_tft_busy = false; }

/* ═══════════════════════════════════════════════════════════════════════════
 *  Periféricos
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gpio_setup(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << BTN_A_PIN) | (1ULL << BTN_B_PIN) |
                        (1ULL << BTN_C_PIN) | (1ULL << BTN_D_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << SPI_CAM_SCLK_PIN) |
                        (1ULL << SPI_CAM_CS_PIN) |
                        (1ULL << SPI_CAM_MOSI_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(SPI_CAM_SCLK_PIN, 0);
    gpio_set_level(SPI_CAM_CS_PIN, 1);
    gpio_set_level(SPI_CAM_MOSI_PIN, 0);

    gpio_config_t ready = {
        .pin_bit_mask = (1ULL << SPI_CAM_READY_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&ready));
}

static bool ensure_spi_buffers(void)
{
    const size_t spi_chunk_dma_bytes =
        EXP_SPI_DMA_ALIGNED_SIZE(EXP_SPI_PAYLOAD_CHUNK + EXP_SPI_GUARD_BYTES);

    // Aumentamos o tamanho de todos os buffers DMA para múltiplos de 4 bytes.
    if (!s_spi_ctrl_tx_buf) {
        s_spi_ctrl_tx_buf = (uint8_t *)heap_caps_malloc(64, SPI_DMA_ALLOC_CAPS);
        if (!s_spi_ctrl_tx_buf) return false;
    }
    if (!s_spi_status_rx_buf) {
        s_spi_status_rx_buf = (uint8_t *)heap_caps_malloc(64, SPI_DMA_ALLOC_CAPS);
        if (!s_spi_status_rx_buf) return false;
    }
    if (!s_spi_header_rx_buf) {
        s_spi_header_rx_buf = (uint8_t *)heap_caps_malloc(128, SPI_DMA_ALLOC_CAPS);
        if (!s_spi_header_rx_buf) return false;
    }
    if (!s_spi_rx_chunk_buf) {
        s_spi_rx_chunk_buf = (uint8_t *)heap_caps_malloc(spi_chunk_dma_bytes,
                                                         SPI_DMA_ALLOC_CAPS);
        if (!s_spi_rx_chunk_buf) return false;
    }
    if (!s_spi_dummy_tx_buf) {
        s_spi_dummy_tx_buf = (uint8_t *)heap_caps_malloc(spi_chunk_dma_bytes,
                                                         SPI_DMA_ALLOC_CAPS);
        if (!s_spi_dummy_tx_buf) return false;
        memset(s_spi_dummy_tx_buf, 0, spi_chunk_dma_bytes);
    }
    return true;
}

static esp_err_t spi_cam_drain_inflight(TickType_t wait_ticks)
{
    spi_transaction_t *done = NULL;
    esp_err_t ret;

    if (!s_spi_trans_inflight) return ESP_OK;
    ret = spi_device_get_trans_result(s_spi_cam, &done, wait_ticks);
    if (ret == ESP_OK) {
        s_spi_trans_inflight = false;
    }
    return ret;
}

static void copy_bytes_no_libc(uint8_t *dst, const uint8_t *src, size_t len)
{
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const volatile uint8_t *s = (const volatile uint8_t *)src;

    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
}

static esp_err_t spi_cam_transfer_timed(const uint8_t *tx, uint8_t *rx, size_t len,
                                        TickType_t timeout_ticks, uint32_t *elapsed_us)
{
    spi_transaction_t *done = NULL;
    esp_err_t ret;
    int64_t t0_us;

    if (elapsed_us) *elapsed_us = 0u;
    if (!s_spi_cam) return ESP_ERR_INVALID_STATE;
    if (len == 0u) return ESP_ERR_INVALID_ARG;
    if (s_spi_trans_inflight) {
        ret = spi_cam_drain_inflight(0);
        if (ret != ESP_OK) return ESP_ERR_INVALID_STATE;
    }

    memset(&s_spi_trans, 0, sizeof(s_spi_trans));
    s_spi_trans.length = len * 8u;
    s_spi_trans.rxlength = len * 8u;
    s_spi_trans.tx_buffer = tx;
    s_spi_trans.rx_buffer = rx;

    t0_us = esp_timer_get_time();
    ret = spi_device_queue_trans(s_spi_cam, &s_spi_trans, timeout_ticks);
    if (ret == ESP_OK) {
        s_spi_trans_inflight = true;
        ret = spi_device_get_trans_result(s_spi_cam, &done, timeout_ticks);
        if (ret == ESP_OK) {
            s_spi_trans_inflight = false;
            if (done != &s_spi_trans) {
                ret = ESP_FAIL;
            }
        }
    }
    if (elapsed_us) {
        int64_t dt = esp_timer_get_time() - t0_us;
        *elapsed_us = (dt > 0) ? (uint32_t)dt : 0u;
    }
    return ret;
}

static bool spi_cam_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    uint32_t elapsed_us = 0u;
    esp_err_t ret;

    ret = spi_cam_transfer_timed(tx, rx, len, SPI_XFER_TIMEOUT_TICKS, &elapsed_us);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_cam_transfer falhou (%u B, %lu us, READY=%d): %s",
                 (unsigned)len, (unsigned long)elapsed_us,
                 gpio_get_level(SPI_CAM_READY_PIN), esp_err_to_name(ret));
        return false;
    }
    return true;
}

static bool spi_cam_transfer_guarded(const uint8_t *tx, uint8_t *rx, size_t data_len)
{
    return spi_cam_transfer(tx, rx, data_len + EXP_SPI_GUARD_BYTES);
}

static esp_err_t spi_cam_transfer_guarded_timed(const uint8_t *tx, uint8_t *rx,
                                                size_t data_len,
                                                TickType_t timeout_ticks,
                                                uint32_t *elapsed_us)
{
    return spi_cam_transfer_timed(tx, rx, data_len + EXP_SPI_GUARD_BYTES,
                                  timeout_ticks, elapsed_us);
}

static bool spi_cam_poll_once(exp_spi_status_t *st, uint8_t cmd, uint8_t method_id,
                              uint8_t k_idx, uint8_t flags, uint32_t run_id, uint32_t arg0,
                              int attempt)
{
    exp_spi_ctrl_t ctrl = {
        .magic = EXP_SPI_MAGIC,
        .version = EXP_SPI_VERSION,
        .cmd = cmd,
        .flags = flags,
        .method_id = method_id,
        .k_idx = k_idx,
        .run_id = run_id,
        .arg0 = arg0,
    };

    if (!st || !ensure_spi_buffers()) return false;
    memset(s_spi_ctrl_tx_buf, 0, 64);
    exp_spi_ctrl_encode(&ctrl, s_spi_ctrl_tx_buf);
    memset(s_spi_status_rx_buf, 0, 64);

    ESP_LOGD(TAG, "=> Entrando em spi_cam_transfer cmd=%02X", cmd);
    if (!spi_cam_transfer(s_spi_ctrl_tx_buf, s_spi_status_rx_buf, EXP_SPI_STATUS_WIRE_SIZE)) {
        ESP_LOGD(TAG, "<= Retornou FALSE em spi_cam_transfer");
        return false;
    }
    ESP_LOGD(TAG, "<= Saiu de spi_cam_transfer");
    if (!exp_spi_status_decode(s_spi_status_rx_buf, st)) {
        char preview[64];
        exp_spi_status_t shifted;
        hex_preview(preview, sizeof(preview), s_spi_status_rx_buf, 8u);
        if (!s_spi_bitshift_hint_logged &&
                spi_buf_looks_like_status_shifted_left(s_spi_status_rx_buf,
                                                       EXP_SPI_STATUS_FRAME_SIZE,
                                                       &shifted)) {
            ESP_LOGW(TAG,
                     "spi_cam_poll_once: status parece deslocado 1 bit preview=%s; "
                     "modo SPI/CPHA incorreto? modo atual=%u corrigido seria state=%u payload=%lu",
                     preview, (unsigned)SPI_CAM_MODE, (unsigned)shifted.state,
                     (unsigned long)shifted.payload_len);
            s_spi_bitshift_hint_logged = true;
        }
        if (attempt >= SPI_INVALID_STATUS_WARN_ATTEMPTS &&
                (attempt == SPI_INVALID_STATUS_WARN_ATTEMPTS ||
                 attempt == 5 || attempt == 20)) {
            ESP_LOGW(TAG, "spi_cam_poll_once: status invalido preview=%s tentativa=%d",
                     preview, attempt);
        } else {
            ESP_LOGD(TAG, "spi_cam_poll_once: status invalido preview=%s tentativa=%d",
                     preview, attempt);
        }
        return false;
    }
    return true;
}

static bool spi_cam_poll_retry(exp_spi_status_t *st, int timeout_ms, uint8_t cmd,
                               uint8_t method_id, uint8_t k_idx, uint8_t flags,
                               uint32_t run_id, uint32_t arg0, const char *tag_prefix)
{
    int64_t deadline_us;
    int attempts = 0;

    if (!st) return false;
    if (timeout_ms < 1) timeout_ms = 1;
    deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    while (esp_timer_get_time() < deadline_us) {
        attempts++;
        ESP_LOGD(TAG, "spi_cam_poll_retry: tentando cmd %02X (tentativa %d)", cmd, attempts);

        if (spi_cam_poll_once(st, cmd, method_id, k_idx, flags, run_id, arg0, attempts)) {
            if (attempts > 1) {
                ESP_LOGI(TAG, "%s: status SPI valido apos %d tentativas",
                         tag_prefix ? tag_prefix : "spi", attempts);
            }
            return true;
        }

        ESP_LOGD(TAG, "spi_cam_poll_retry: falhou; delay tick=%d", (int)SPI_RETRY_DELAY_TICKS);

        vTaskDelay(SPI_RETRY_DELAY_TICKS);
    }

    if (tag_prefix) {
        ESP_LOGW(TAG, "%s: sem status SPI valido apos %d ms (%d tentativas)",
                 tag_prefix, timeout_ms, attempts);
    }
    return false;
}

static bool spi_buf_looks_like_status_shifted_left(const uint8_t *buf, size_t len,
                                                   exp_spi_status_t *st_out)
{
    uint8_t shifted[EXP_SPI_STATUS_FRAME_SIZE];
    exp_spi_status_t st;

    if (!buf || len < EXP_SPI_STATUS_FRAME_SIZE) return false;
    for (size_t i = 0; i < EXP_SPI_STATUS_FRAME_SIZE; i++) {
        shifted[i] = (uint8_t)(buf[i] >> 1);
    }
    if (!exp_spi_status_decode(shifted, &st)) return false;
    if (st_out) *st_out = st;
    return true;
}

static void spi_cam_setup(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = SPI_CAM_MOSI_PIN,
        .miso_io_num = SPI_CAM_MISO_PIN,
        .sclk_io_num = SPI_CAM_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXP_SPI_DMA_ALIGNED_SIZE(EXP_SPI_PAYLOAD_CHUNK + EXP_SPI_GUARD_BYTES),
    };
    spi_device_interface_config_t dev = {
        .mode = SPI_CAM_MODE,
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = SPI_CAM_FREQ_HZ,
        .spics_io_num = SPI_CAM_CS_PIN,
        .cs_ena_pretrans = 6,
        .cs_ena_posttrans = 3,
        .queue_size = 1,
        .flags = 0,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI_CAM_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI_CAM_HOST, &dev, &s_spi_cam));
    ESP_LOGI(TAG, "SPI-CAM setup (host=%d mode=%u SCLK=%d MOSI=%d MISO=%d CS=%d READY=%d freq=%d)",
             SPI_CAM_HOST, (unsigned)SPI_CAM_MODE, SPI_CAM_SCLK_PIN, SPI_CAM_MOSI_PIN,
             SPI_CAM_MISO_PIN, SPI_CAM_CS_PIN, SPI_CAM_READY_PIN, SPI_CAM_FREQ_HZ);
}

static bool spi_cam_wait_ready(int timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);

    while (esp_timer_get_time() < deadline_us) {
        exp_spi_status_t st = {0};
        if (spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                               EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                               "spi ready")) {
            if (st.state == EXP_SPI_STATE_IDLE || st.state == EXP_SPI_STATE_DONE) {
                ESP_LOGI(TAG, "SPI-CAM: status=%u run=%lu payload=%lu",
                         (unsigned)st.state,
                         (unsigned long)st.run_id,
                         (unsigned long)st.payload_len);
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
    }
    return false;
}

static bool spi_cam_send_config(uint8_t method_id, uint8_t k_idx, uint32_t run_id)
{
    exp_spi_status_t st;
    uint8_t flags = EXP_SPI_FLAG_RAW_FIRST;

    if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                            EXP_SPI_CMD_SET_CONFIG, method_id, k_idx, flags, run_id, 0u,
                            "spi config")) {
        return false;
    }

    for (int i = 0; i < 20; i++) {
        if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                                "spi config")) {
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
            continue;
        }
        if (st.error != EXP_SPI_ERROR_NONE) {
            ESP_LOGW(TAG, "spi config: CAM erro state=%u error=%u run=%lu",
                     (unsigned)st.state, (unsigned)st.error,
                     (unsigned long)st.run_id);
            return false;
        }
        if (st.state == EXP_SPI_STATE_IDLE && st.run_id == run_id) {
            ESP_LOGI(TAG, "spi config: confirmado run=%lu method=%s k=%s",
                     (unsigned long)run_id, exp_method_name(method_id),
                     exp_k_label_from_idx(k_idx));
            return true;
        }
        ESP_LOGW(TAG,
                 "spi config: aguardando confirmacao state=%u run_rx=%lu run_esperado=%lu",
                 (unsigned)st.state,
                 (unsigned long)st.run_id,
                 (unsigned long)run_id);
        vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
    }
    ESP_LOGE(TAG, "spi config: timeout aguardando IDLE com run_id=%lu",
             (unsigned long)run_id);
    return false;
}

static bool spi_cam_trigger(uint8_t method_id, uint8_t k_idx, uint32_t run_id)
{
    exp_spi_status_t st;

    if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                            EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                            "spi trigger pre")) {
        return false;
    }
    if (st.error != EXP_SPI_ERROR_NONE || st.state == EXP_SPI_STATE_ERROR) {
        ESP_LOGW(TAG, "spi trigger pre: CAM erro state=%u error=%u run=%lu",
                 (unsigned)st.state,
                 (unsigned)st.error,
                 (unsigned long)st.run_id);
        return false;
    }
    if (st.state != EXP_SPI_STATE_IDLE && st.state != EXP_SPI_STATE_DONE) {
        ESP_LOGW(TAG, "spi trigger pre: CAM ocupada state=%u run=%lu",
                 (unsigned)st.state,
                 (unsigned long)st.run_id);
        return false;
    }
    ESP_LOGI(TAG, "spi trigger pre: CAM pronta state=%u run_antigo=%lu novo_run=%lu",
             (unsigned)st.state,
             (unsigned long)st.run_id,
             (unsigned long)run_id);

    if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                            EXP_SPI_CMD_TRIGGER, method_id,
                            k_idx, EXP_SPI_FLAG_RAW_FIRST, run_id, 0u,
                            "spi trigger cmd")) {
        return false;
    }

    for (int i = 0; i < 50; i++) {
        if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                                "spi trigger wait")) {
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
            continue;
        }
        if (st.error != EXP_SPI_ERROR_NONE || st.state == EXP_SPI_STATE_ERROR) {
            ESP_LOGW(TAG, "spi trigger wait: CAM erro state=%u error=%u run=%lu",
                     (unsigned)st.state, (unsigned)st.error,
                     (unsigned long)st.run_id);
            return false;
        }
        if (st.run_id == run_id &&
                (st.state == EXP_SPI_STATE_CAPTURING ||
                 st.state == EXP_SPI_STATE_RECORD_READY ||
                 st.state == EXP_SPI_STATE_DONE)) {
            ESP_LOGI(TAG, "spi trigger: aceito state=%u run=%lu",
                     (unsigned)st.state, (unsigned long)st.run_id);
            return true;
        }
        if ((st.state == EXP_SPI_STATE_IDLE || st.state == EXP_SPI_STATE_DONE) &&
                st.run_id != run_id) {
            ESP_LOGW(TAG,
                     "spi trigger wait: CAM ainda pronta com run antigo state=%u run_rx=%lu; reenviando TRIGGER run=%lu",
                     (unsigned)st.state,
                     (unsigned long)st.run_id,
                     (unsigned long)run_id);
            (void)spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                     EXP_SPI_CMD_TRIGGER, method_id,
                                     k_idx, EXP_SPI_FLAG_RAW_FIRST, run_id, 0u,
                                     "spi trigger retry");
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
            continue;
        }
        ESP_LOGW(TAG,
                 "spi trigger wait: aguardando novo run state=%u run_rx=%lu run_esperado=%lu",
                 (unsigned)st.state,
                 (unsigned long)st.run_id,
                 (unsigned long)run_id);
        vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
    }

    ESP_LOGE(TAG, "spi trigger: timeout aguardando novo run=%lu",
             (unsigned long)run_id);
    return false;
}

static bool spi_cam_abort(void)
{
    exp_spi_status_t st;

    if (s_spi_trans_inflight) {
        esp_err_t drain_ret;

        ESP_LOGW(TAG, "spi abort: transacao SPI pendente; tentando drenar (500 ms) antes do ABORT");
        drain_ret = spi_cam_drain_inflight(pdMS_TO_TICKS(500));
        if (drain_ret == ESP_OK) {
            ESP_LOGI(TAG, "spi abort: [drain-ok] dreno concluido com sucesso");
        } else {
            ESP_LOGE(TAG, "spi abort: [drain-timeout] timeout; forçando reset de estado SPI: %s",
                     esp_err_to_name(drain_ret));
            s_spi_trans_inflight = false;
            memset(&s_spi_trans, 0, sizeof(s_spi_trans));
        }
    }

    for (int attempt = 1; attempt <= 8; attempt++) {
        if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                EXP_SPI_CMD_ABORT, 0u, 0u, 0u, 0u, 0u,
                                "spi abort")) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        for (int i = 0; i < 6; i++) {
            if (spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                   EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                                   "spi abort poll") &&
                    st.state == EXP_SPI_STATE_IDLE &&
                    (st.error == EXP_SPI_ERROR_NONE || st.error == EXP_SPI_ERROR_ABORTED)) {
                ESP_LOGI(TAG, "spi abort: CAM recuperada tentativa=%d state=%u error=%u run=%lu",
                         attempt,
                         (unsigned)st.state,
                         (unsigned)st.error,
                         (unsigned long)st.run_id);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
        }
    }
    ESP_LOGE(TAG, "spi abort: nao conseguiu recuperar CAM");
    return false;
}

static bool spi_cam_recover_if_error(void)
{
    exp_spi_status_t st;

    if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                            EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                            "spi recover")) {
        ESP_LOGW(TAG, "spi recover: sem status valido; tentando ABORT");
        return spi_cam_abort();
    }

    if (st.state == EXP_SPI_STATE_ERROR || st.error != EXP_SPI_ERROR_NONE ||
            st.state == EXP_SPI_STATE_CAPTURING ||
            st.state == EXP_SPI_STATE_RECORD_READY) {
        ESP_LOGW(TAG,
                 "spi recover: CAM ocupada/erro state=%u error=%u run=%lu; enviando ABORT",
                 (unsigned)st.state,
                 (unsigned)st.error,
                 (unsigned long)st.run_id);
        return spi_cam_abort();
    }

    return true;
}

static bool spi_cam_read_header(exp_record_header_t *hdr)
{
    if (!hdr || !ensure_spi_buffers()) return false;

    for (unsigned attempt = 1u; attempt <= SPI_HEADER_READ_ATTEMPTS; attempt++) {
        exp_spi_status_t st_probe = {0};
        TickType_t wait_ticks = pdMS_TO_TICKS(40u * attempt);

        if (wait_ticks < 1) wait_ticks = 1;
        if (wait_ticks > pdMS_TO_TICKS(500)) wait_ticks = pdMS_TO_TICKS(500);

        /* A CAM auto-arma o header quando entra em RECORD_READY. Assim o S3
         * nao depende de um comando READ_HEADER no meio do modo ALL. */
        vTaskDelay(SPI_ARM_SETTLE_TICKS);
        memset(s_spi_header_rx_buf, 0, EXP_RECORD_HEADER_SIZE + EXP_SPI_GUARD_BYTES);
        if (!spi_cam_transfer_guarded(s_spi_dummy_tx_buf,
                                      s_spi_header_rx_buf,
                                      EXP_RECORD_HEADER_SIZE)) {
            ESP_LOGW(TAG,
                     "spi_cam_read_header: falha na transferencia do header tentativa=%u",
                     attempt);
            vTaskDelay(wait_ticks);
            continue;
        }

        if (exp_record_header_decode(s_spi_header_rx_buf, hdr)) {
            ESP_LOGI(TAG, "spi_cam_read_header: OK tentativa=%u run=%lu method=%s payload=%lu",
                     attempt,
                     (unsigned long)hdr->run_id,
                     exp_method_name(hdr->method_id),
                     (unsigned long)hdr->payload_len);
            return true;
        }

        if (s_spi_header_rx_buf[0] == (uint8_t)(EXP_SPI_MAGIC & 0xFFu) &&
                s_spi_header_rx_buf[1] == (uint8_t)(EXP_SPI_MAGIC >> 8) &&
            exp_spi_status_decode(s_spi_header_rx_buf, &st_probe)) {
            ESP_LOGW(TAG,
                     "spi_cam_read_header: recebeu STATUS no lugar do HEADER tentativa=%u state=%u error=%u payload=%lu; aguardando auto-arm",
                     attempt,
                     (unsigned)st_probe.state,
                     (unsigned)st_probe.error,
                     (unsigned long)st_probe.payload_len);
            if (st_probe.error != EXP_SPI_ERROR_NONE) {
                return false;
            }
            vTaskDelay(wait_ticks);
            continue;
        }

        ESP_LOGW(TAG,
                 "spi_cam_read_header: bytes invalidos tentativa=%u hdr[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X",
                 attempt,
                 s_spi_header_rx_buf[0],
                 s_spi_header_rx_buf[1],
                 s_spi_header_rx_buf[2],
                 s_spi_header_rx_buf[3],
                 s_spi_header_rx_buf[4],
                 s_spi_header_rx_buf[5],
                 s_spi_header_rx_buf[6],
                 s_spi_header_rx_buf[7]);
        vTaskDelay(wait_ticks);
    }

    ESP_LOGE(TAG, "spi_cam_read_header: falhou apos %u tentativa(s)",
             (unsigned)SPI_HEADER_READ_ATTEMPTS);
    return false;
}

static bool spi_cam_read_telemetry(exp_record_header_t *hdr)
{
    if (!hdr || !ensure_spi_buffers()) return false;

    for (unsigned attempt = 1u; attempt <= SPI_HEADER_READ_ATTEMPTS; attempt++) {
        exp_spi_status_t st_probe = {0};
        TickType_t wait_ticks = pdMS_TO_TICKS(30u * attempt);

        if (wait_ticks < 1) wait_ticks = 1;
        if (wait_ticks > pdMS_TO_TICKS(300)) wait_ticks = pdMS_TO_TICKS(300);

        /* Depois do payload completo, a CAM auto-arma a telemetria do mesmo
         * registro para evitar outro comando de leitura sensivel a truncagem. */
        vTaskDelay(SPI_ARM_SETTLE_TICKS);
        memset(s_spi_header_rx_buf, 0, EXP_RECORD_TELEMETRY_SIZE + EXP_SPI_GUARD_BYTES);
        if (!spi_cam_transfer_guarded(s_spi_dummy_tx_buf,
                                      s_spi_header_rx_buf,
                                      EXP_RECORD_TELEMETRY_SIZE)) {
            ESP_LOGW(TAG,
                     "spi_cam_read_telemetry: falha na transferencia tentativa=%u",
                     attempt);
            vTaskDelay(wait_ticks);
            continue;
        }

        if (exp_record_telemetry_decode(s_spi_header_rx_buf, hdr)) {
            ESP_LOGI(TAG,
                     "spi_cam_read_telemetry: OK tentativa=%u capture_us=%lu compress_us=%lu dct_us=%lu dct_calls=%u",
                     attempt,
                     (unsigned long)hdr->t_capture_us,
                     (unsigned long)hdr->t_algorithm_us,
                     (unsigned long)hdr->t_dct_kernel_us,
                     (unsigned)hdr->dct_kernel_calls);
            return true;
        }

        if (s_spi_header_rx_buf[0] == (uint8_t)(EXP_SPI_MAGIC & 0xFFu) &&
                s_spi_header_rx_buf[1] == (uint8_t)(EXP_SPI_MAGIC >> 8) &&
            exp_spi_status_decode(s_spi_header_rx_buf, &st_probe)) {
            ESP_LOGW(TAG,
                     "spi_cam_read_telemetry: recebeu STATUS no lugar da telemetria tentativa=%u state=%u error=%u payload=%lu; aguardando auto-arm",
                     attempt,
                     (unsigned)st_probe.state,
                     (unsigned)st_probe.error,
                     (unsigned long)st_probe.payload_len);
            if (st_probe.error != EXP_SPI_ERROR_NONE) {
                return false;
            }
            vTaskDelay(wait_ticks);
            continue;
        }

        ESP_LOGW(TAG,
                 "spi_cam_read_telemetry: bytes invalidos tentativa=%u tel[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X",
                 attempt,
                 s_spi_header_rx_buf[0],
                 s_spi_header_rx_buf[1],
                 s_spi_header_rx_buf[2],
                 s_spi_header_rx_buf[3],
                 s_spi_header_rx_buf[4],
                 s_spi_header_rx_buf[5],
                 s_spi_header_rx_buf[6],
                 s_spi_header_rx_buf[7]);
        vTaskDelay(wait_ticks);
    }

    ESP_LOGE(TAG, "spi_cam_read_telemetry: falhou apos %u tentativa(s)",
             (unsigned)SPI_HEADER_READ_ATTEMPTS);
    return false;
}

static bool rx_payload_store_chunk(uint32_t offset, const uint8_t *src, size_t len,
                                   const char *tag_prefix)
{
    if (!s_rx_payload_buf || !src) {
        ESP_LOGE(TAG, "%s: payload store invalido dst=%p src=%p",
                 tag_prefix, (void *)s_rx_payload_buf, (const void *)src);
        return false;
    }
    if ((size_t)offset > s_rx_payload_capacity ||
            len > (s_rx_payload_capacity - (size_t)offset)) {
        ESP_LOGE(TAG, "%s: payload store overflow off=%lu len=%lu cap=%lu",
                 tag_prefix,
                 (unsigned long)offset,
                 (unsigned long)len,
                 (unsigned long)s_rx_payload_capacity);
        return false;
    }

    copy_bytes_no_libc(s_rx_payload_buf + offset, src, len);
    return true;
}

static bool spi_cam_read_payload(uint32_t payload_len, uint8_t mode_type,
                                 uint32_t *t_spi_rx_us, const char *tag_prefix)
{
    uint32_t total = 0u;
    int64_t t0 = esp_timer_get_time();

    (void)mode_type;

    if (!ensure_rx_payload_buffer_capacity((size_t)payload_len, true) ||
            !ensure_spi_buffers()) {
        return false;
    }
    if (payload_len == 0u) {
        if (t_spi_rx_us) *t_spi_rx_us = 0u;
        return true;
    }

    // Dá um tempo inicial generoso para a CAM armar o primeiro chunk na DMA
    vTaskDelay(pdMS_TO_TICKS(10));

    while (total < payload_len) {
        uint32_t remaining = payload_len - total;
        uint32_t chunk_limit = (total == 0u) ? SPI_PAYLOAD_FIRST_CHUNK : SPI_PAYLOAD_STREAM_CHUNK;
        size_t chunk = (remaining > chunk_limit) ? (size_t)chunk_limit : (size_t)remaining;
        uint32_t transfer_us = 0u;
        esp_err_t spi_ret;
        int ready_before;
        int ready_after;
        uint32_t prev_total;
        const bool trace_chunk = (total < (SPI_PAYLOAD_FIRST_CHUNK + (2u * SPI_PAYLOAD_STREAM_CHUNK)));

        ESP_LOGD(TAG, "payload: preparando chunk %lu", (unsigned long)chunk);

        // Limpa estritamente os bytes que usaremos, evitando qualquer overflow
        memset(s_spi_rx_chunk_buf, 0, chunk + EXP_SPI_GUARD_BYTES);

        ready_before = gpio_get_level(SPI_CAM_READY_PIN);
        ESP_LOGD(TAG,
                 "%s: payload antes SPI offset=%lu chunk=%lu remaining=%lu ready=%d",
                 tag_prefix,
                 (unsigned long)total,
                 (unsigned long)chunk,
                 (unsigned long)remaining,
                 ready_before);

        // Transação SPI
        spi_ret = spi_cam_transfer_guarded_timed(s_spi_dummy_tx_buf, s_spi_rx_chunk_buf,
                                                 chunk, SPI_PAYLOAD_XFER_TIMEOUT_TICKS,
                                                 &transfer_us);
        ready_after = gpio_get_level(SPI_CAM_READY_PIN);
        ESP_LOGD(TAG,
                 "%s: payload depois SPI offset=%lu chunk=%lu remaining=%lu ret=%s(%d) us=%lu ready=%d",
                 tag_prefix,
                 (unsigned long)total,
                 (unsigned long)chunk,
                 (unsigned long)remaining,
                 esp_err_to_name(spi_ret),
                 (int)spi_ret,
                 (unsigned long)transfer_us,
                 ready_after);
        if (spi_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "%s: falha lendo payload SPI state=%d offset=%lu chunk=%lu total=%lu ready=%d err=%s",
                     tag_prefix,
                     (int)g_state,
                     (unsigned long)total,
                     (unsigned long)chunk,
                     (unsigned long)payload_len,
                     ready_after,
                     esp_err_to_name(spi_ret));
            return false;
        }

        if (trace_chunk) {
            ESP_LOGD(TAG,
                     "%s: payload pos-transfer offset=%lu chunk=%lu first=%02X %02X %02X %02X hwm=%u",
                     tag_prefix,
                     (unsigned long)total,
                     (unsigned long)chunk,
                     s_spi_rx_chunk_buf[0],
                     s_spi_rx_chunk_buf[1],
                     s_spi_rx_chunk_buf[2],
                     s_spi_rx_chunk_buf[3],
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        // Salva no buffer de payload fora da região DMA temporária.
        if (trace_chunk) {
            ESP_LOGD(TAG, "%s: payload antes copy dst=%p off=%lu bytes=%lu cap=%lu",
                     tag_prefix,
                     (void *)(s_rx_payload_buf + total),
                     (unsigned long)total,
                     (unsigned long)chunk,
                     (unsigned long)s_rx_payload_capacity);
        }
        prev_total = total;
        if (!rx_payload_store_chunk(total, s_spi_rx_chunk_buf, chunk, tag_prefix)) {
            return false;
        }
        total += (uint32_t)chunk;
        if (trace_chunk) {
            ESP_LOGD(TAG, "%s: payload depois copy total=%lu/%lu",
                     tag_prefix, (unsigned long)total, (unsigned long)payload_len);
        }

        if (total == payload_len ||
                (total / SPI_PAYLOAD_PROGRESS_STEP_BYTES) !=
                (prev_total / SPI_PAYLOAD_PROGRESS_STEP_BYTES)) {
            ESP_LOGI(TAG, "%s: progresso SPI %lu/%lu bytes",
                     tag_prefix, (unsigned long)total, (unsigned long)payload_len);
        }

        // GAP VITAL: aguarda a CAM processar o término do chunk anterior
        // e armar a próxima DMA antes do próximo CS do master.
        if (total < payload_len) {
            if (trace_chunk) {
                ESP_LOGD(TAG, "%s: payload gap inicio total=%lu ticks=%u ready=%d",
                         tag_prefix, (unsigned long)total,
                         (unsigned)SPI_PAYLOAD_GAP_TICKS,
                         gpio_get_level(SPI_CAM_READY_PIN));
            }
            vTaskDelay(SPI_PAYLOAD_GAP_TICKS);
            if (trace_chunk) {
                ESP_LOGD(TAG, "%s: payload gap fim total=%lu ready=%d",
                         tag_prefix, (unsigned long)total,
                         gpio_get_level(SPI_CAM_READY_PIN));
            }
        }
    }

    if (t_spi_rx_us) {
        *t_spi_rx_us = (uint32_t)(esp_timer_get_time() - t0);
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Botões
 * ═══════════════════════════════════════════════════════════════════════════ */

static button_state_t *btn_state_for_pin(int pin)
{
    for (size_t i = 0; i < sizeof(s_btn_states) / sizeof(s_btn_states[0]); i++) {
        if (s_btn_states[i].pin == pin) return &s_btn_states[i];
    }
    return NULL;
}

static void btn_poll_all(void)
{
    for (size_t i = 0; i < sizeof(s_btn_states) / sizeof(s_btn_states[0]); i++) {
        btn_update_state(&s_btn_states[i]);
    }
}

static void btn_update_state(button_state_t *st)
{
    if (!st) return;

    int64_t now_us = esp_timer_get_time();
    int raw = gpio_get_level(st->pin);

    if (!st->initialized) {
        st->raw_level      = raw;
        st->stable_level   = raw;
        st->last_change_us = now_us;
        st->press_latched  = (raw == 0);
        st->initialized    = true;
        return;
    }

    if (raw != st->raw_level) {
        st->raw_level      = raw;
        st->last_change_us = now_us;
    }

    if (raw != st->stable_level &&
        (now_us - st->last_change_us) >= ((int64_t)DEBOUNCE_MS * 1000LL)) {
        st->stable_level = raw;
        if (raw == 1) st->press_latched = false;
    }
}

static bool btn_is_low(int pin)
{
    button_state_t *st = btn_state_for_pin(pin);
    btn_update_state(st);
    return st ? (st->stable_level == 0) : (gpio_get_level(pin) == 0);
}

static bool btn_pressed(int pin)
{
    button_state_t *st = btn_state_for_pin(pin);
    btn_update_state(st);
    if (!st) return false;

    if (st->stable_level == 0 && !st->press_latched) {
        st->press_latched = true;
        return true;
    }
    return false;
}

static bool btn_ac_held_1s(void)
{
    if (!btn_is_low(BTN_A_PIN) || !btn_is_low(BTN_C_PIN)) return false;
    int held_ms = 0;
    while (btn_is_low(BTN_A_PIN) && btn_is_low(BTN_C_PIN)) {
        vTaskDelay(pdMS_TO_TICKS(50));
        held_ms += 50;
        if (held_ms >= 1000) return true;
    }
    return false;
}

/* Retorna true se qualquer botão (exceto B) estiver pressionado.
 * Passada como cancel_fn para sd_init() - permite abortar o mount. */
static bool sd_cancel_check(void)
{
    return btn_is_low(BTN_A_PIN) || btn_is_low(BTN_C_PIN) || btn_is_low(BTN_D_PIN);
}

static bool btn_b_safe(void)
{
    if (!s_boot_guard_done) {
        if ((int32_t)(xTaskGetTickCount() - s_boot_tick) >
                (int32_t)pdMS_TO_TICKS(500)) {
            s_boot_guard_done = true;
        } else {
            return false;
        }
    }
    return btn_pressed(BTN_B_PIN);
}

static bool rx_payload_psram_smoke_test(size_t capacity)
{
    static const uint8_t pat[4] = {0xA5u, 0x5Au, 0xC3u, 0x3Cu};
    size_t tail;

    if (!s_rx_payload_buf || capacity < sizeof(pat)) return false;

    tail = capacity - sizeof(pat);
    ESP_LOGI(TAG, "rx_payload_buf: teste write/read ptr=%p cap=%u",
             (void *)s_rx_payload_buf, (unsigned)capacity);
    copy_bytes_no_libc(s_rx_payload_buf, pat, sizeof(pat));
    copy_bytes_no_libc(s_rx_payload_buf + tail, pat, sizeof(pat));

    for (size_t i = 0; i < sizeof(pat); i++) {
        if (s_rx_payload_buf[i] != pat[i] || s_rx_payload_buf[tail + i] != pat[i]) {
            ESP_LOGE(TAG,
                     "rx_payload_buf: teste write/read falhou i=%u head=%02X tail=%02X exp=%02X",
                     (unsigned)i,
                     s_rx_payload_buf[i],
                     s_rx_payload_buf[tail + i],
                     pat[i]);
            return false;
        }
    }
    return true;
}

static void release_rx_payload_buffer(void)
{
    if (s_rx_payload_buf) {
        heap_caps_free(s_rx_payload_buf);
        s_rx_payload_buf = NULL;
    }
    s_rx_payload_capacity = 0u;
    s_rx_payload_internal = false;
}

static bool ensure_rx_payload_buffer_capacity(size_t required, bool prefer_internal)
{
    size_t max_payload = IMG_RGB888_BYTES;
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const char *region = "PSRAM";

    if (EXP_JPEG_RLE_WORST_CASE_BYTES > max_payload) {
        max_payload = EXP_JPEG_RLE_WORST_CASE_BYTES;
    }
    if (required == 0u) required = 1u;
    if (required > max_payload) {
        ESP_LOGE(TAG, "rx_payload_buf: tamanho solicitado invalido req=%u max=%u",
                 (unsigned)required, (unsigned)max_payload);
        return false;
    }

    if (s_rx_payload_buf) {
        if (s_rx_payload_capacity >= required &&
                (!prefer_internal || s_rx_payload_internal)) {
            return true;
        }
        release_rx_payload_buffer();
    }

    if (prefer_internal) {
        caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        region = "DRAM interna";
    }

    s_rx_payload_buf = (uint8_t *)heap_caps_malloc(required, caps);
    if (!s_rx_payload_buf && prefer_internal) {
        ESP_LOGW(TAG,
                 "rx_payload_buf: DRAM interna insuficiente para %u bytes; tentando PSRAM",
                 (unsigned)required);
        caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        region = "PSRAM";
        s_rx_payload_buf = (uint8_t *)heap_caps_malloc(required, caps);
    }
    if (!s_rx_payload_buf) {
        ESP_LOGE(TAG, "Malloc rx_payload_buf falhou (%u bytes)", (unsigned)required);
        return false;
    }
    s_rx_payload_capacity = required;
    s_rx_payload_internal = (caps & MALLOC_CAP_INTERNAL) != 0u;
    ESP_LOGI(TAG, "rx_payload_buf alocado (%u bytes) em %s ptr=%p",
             (unsigned)required, region, (void *)s_rx_payload_buf);
    if (!rx_payload_psram_smoke_test(s_rx_payload_capacity)) {
        release_rx_payload_buffer();
        return false;
    }
    return true;
}

static uint32_t current_run_id(void)
{
    return g_fallback_run_id++;
}

static uint8_t current_method_id(void)
{
    return METHOD_ORDER[g_method_idx];
}

static const char *display_method_name(void)
{
    return METHOD_NAMES[g_method_idx];
}

static void run_reset(ExperimentRunResult *run)
{
    int i;
    uint8_t *saved_images[EXP_RECORD_SLOT_COUNT];

    if (!run) return;
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        saved_images[i] = run->images[i];
    }
    memset(run, 0, sizeof(*run));
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        run->images[i] = saved_images[i];
    }
}

static bool ensure_run_image_slot(ExperimentRunResult *run, int slot)
{
    if (!run || slot < 0 || slot >= EXP_RECORD_SLOT_COUNT) return false;
    if (run->images[slot]) return true;

    run->images[slot] = (uint8_t *)heap_caps_malloc(IMG_RGB888_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!run->images[slot]) return false;
    memset(run->images[slot], 0, IMG_RGB888_BYTES);
    ESP_LOGI(TAG, "run image slot=%d alocado (%u bytes) em PSRAM", slot, (unsigned)IMG_RGB888_BYTES);
    return true;
}

static bool ensure_run_image_buffers(ExperimentRunResult *run)
{
    int i;

    if (!run) return false;
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        if (!ensure_run_image_slot(run, i)) {
            ESP_LOGE(TAG, "falha ao alocar run image slot=%d", i);
            return false;
        }
    }
    return true;
}

static bool ensure_decode_workspace(void)
{
    size_t bx = ((size_t)IMG_WIDTH + 7u) / 8u;
    size_t by = ((size_t)IMG_HEIGHT + 7u) / 8u;
    size_t max_blocks = bx * by;
    size_t plane_bytes = max_blocks * 64u * sizeof(int32_t);

    if (s_decode_comp) return true;

    s_decode_comp = (jpeg_compressed_t *)heap_caps_calloc(1u, sizeof(*s_decode_comp), MALLOC_CAP_8BIT);
    if (!s_decode_comp) return false;

    ESP_LOGI(TAG,
             "decode workspace alloc caps=SPIRAM|32BIT align=%u free=%lu largest=%lu",
             (unsigned)DECODE_WORKSPACE_ALIGN,
             (unsigned long)heap_caps_get_free_size(DECODE_WORKSPACE_CAPS),
             (unsigned long)heap_caps_get_largest_free_block(DECODE_WORKSPACE_CAPS));

    s_decode_comp->y_quantized = (int32_t *)heap_caps_aligned_alloc(
        DECODE_WORKSPACE_ALIGN, plane_bytes, DECODE_WORKSPACE_CAPS);
    s_decode_comp->cb_quantized = (int32_t *)heap_caps_aligned_alloc(
        DECODE_WORKSPACE_ALIGN, plane_bytes, DECODE_WORKSPACE_CAPS);
    s_decode_comp->cr_quantized = (int32_t *)heap_caps_aligned_alloc(
        DECODE_WORKSPACE_ALIGN, plane_bytes, DECODE_WORKSPACE_CAPS);
    if (!s_decode_comp->y_quantized || !s_decode_comp->cb_quantized || !s_decode_comp->cr_quantized) {
        if (s_decode_comp->y_quantized) heap_caps_free(s_decode_comp->y_quantized);
        if (s_decode_comp->cb_quantized) heap_caps_free(s_decode_comp->cb_quantized);
        if (s_decode_comp->cr_quantized) heap_caps_free(s_decode_comp->cr_quantized);
        heap_caps_free(s_decode_comp);
        s_decode_comp = NULL;
        return false;
    }
    ESP_LOGI(TAG, "decode workspace ptrs: y=%p cb=%p cr=%p plane=%u",
             (void *)s_decode_comp->y_quantized,
             (void *)s_decode_comp->cb_quantized,
             (void *)s_decode_comp->cr_quantized,
             (unsigned)plane_bytes);

    memset(s_decode_comp->y_quantized, 0, plane_bytes);
    memset(s_decode_comp->cb_quantized, 0, plane_bytes);
    memset(s_decode_comp->cr_quantized, 0, plane_bytes);
    s_decode_max_luma_blocks = max_blocks;
    s_decode_max_chroma_blocks = max_blocks;
    ESP_LOGI(TAG, "decode workspace pronto y/cb/cr=%u bytes cada (blocks=%u)",
             (unsigned)plane_bytes, (unsigned)max_blocks);
    return true;
}

static bool compute_decode_block_counts(uint16_t width, uint16_t height,
                                        uint8_t colorspace, uint8_t subsampling,
                                        int32_t *num_luma, int32_t *num_chroma)
{
    int32_t bx;
    int32_t by;

    if (!num_luma || !num_chroma || width == 0u || height == 0u) return false;
    bx = (int32_t)((width + 7u) / 8u);
    by = (int32_t)((height + 7u) / 8u);
    *num_luma = bx * by;

    if (colorspace == JPEG_COLORSPACE_GRAYSCALE) {
        *num_chroma = 0;
        return true;
    }

    switch (subsampling) {
        case JPEG_SUBSAMP_444:
            *num_chroma = *num_luma;
            return true;
        case JPEG_SUBSAMP_422:
            *num_chroma = (int32_t)(((width + 15u) / 16u) * ((height + 7u) / 8u));
            return true;
        case JPEG_SUBSAMP_420:
            *num_chroma = (int32_t)(((width + 15u) / 16u) * ((height + 15u) / 16u));
            return true;
        default:
            return false;
    }
}

static bool prepare_decode_workspace(const exp_record_header_t *hdr,
                                     jpeg_compressed_t **out_comp)
{
    int32_t num_luma = 0;
    int32_t num_chroma = 0;

    if (!hdr || !out_comp) return false;
    if (!ensure_decode_workspace()) return false;
    if (!compute_decode_block_counts(hdr->width, hdr->height, hdr->colorspace,
                                     hdr->subsampling, &num_luma, &num_chroma)) {
        return false;
    }
    if ((size_t)num_luma > s_decode_max_luma_blocks || (size_t)num_chroma > s_decode_max_chroma_blocks) {
        ESP_LOGE(TAG, "decode workspace insuficiente luma=%ld chroma=%ld max=%u",
                 (long)num_luma, (long)num_chroma, (unsigned)s_decode_max_luma_blocks);
        return false;
    }

    s_decode_comp->width = hdr->width;
    s_decode_comp->height = hdr->height;
    s_decode_comp->quality_factor = exp_k_factor_from_idx(hdr->k_idx);
    s_decode_comp->dct_method = method_id_to_dct(hdr->method_id);
    s_decode_comp->colorspace = (jpeg_colorspace_t)hdr->colorspace;
    s_decode_comp->subsampling = (jpeg_subsampling_t)hdr->subsampling;
    s_decode_comp->flags = 0u;
    s_decode_comp->num_blocks_y = num_luma;
    s_decode_comp->num_blocks_chroma = num_chroma;
    s_decode_comp->y_coeffs = NULL;
    s_decode_comp->cb_coeffs = NULL;
    s_decode_comp->cr_coeffs = NULL;
    *out_comp = s_decode_comp;
    return true;
}

static bool psram_probe_stage(const char *stage)
{
    volatile uint32_t *p;
    uint32_t v;

    if (!stage) stage = "?";
    if (!s_decode_comp || !s_decode_comp->y_quantized) {
        ESP_LOGE(TAG, "psram probe[%s]: workspace nulo", stage);
        return false;
    }

    p = (volatile uint32_t *)s_decode_comp->y_quantized;
    ESP_LOGI(TAG,
             "psram probe[%s]: ptr=%p align4=%u psram_free=%lu largest=%lu",
             stage, (void *)p,
             (unsigned)((((uintptr_t)p) & 0x3u) == 0u),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "psram probe[%s]: before write", stage);
    p[0] = 0x12345678u;
    ESP_LOGI(TAG, "psram probe[%s]: after write", stage);

    ESP_LOGI(TAG, "psram probe[%s]: before read", stage);
    v = p[0];
    ESP_LOGI(TAG, "psram probe[%s]: after read value=0x%08lX",
             stage, (unsigned long)v);
    if (v != 0x12345678u) {
        ESP_LOGE(TAG, "psram probe[%s]: readback divergente=0x%08lX",
                 stage, (unsigned long)v);
        return false;
    }

    return true;
}

static bool heap_check_stage(const char *stage)
{
    bool ok;

    if (!stage) stage = "?";
    ESP_LOGI(TAG, "heap check[%s]: before", stage);
    ok = heap_caps_check_integrity_all(true);
    ESP_LOGI(TAG, "heap check[%s]: result=%d", stage, (int)ok);
    return ok;
}

static int run_present_record_count(const ExperimentRunResult *run)
{
    int count = 0, i;

    if (!run) return 0;
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        if (run->records[i].present) count++;
    }
    return count;
}

static int payload_timeout_ms(uint32_t payload_len)
{
    (void)payload_len;
    /* Para depuração, usar 15s fixos provisoriamente, ou o cálculo estendido  */
    return 15000;
}

static void spi_cam_reset_state(void)
{
    if (s_spi_trans_inflight) {
        spi_transaction_t *done = NULL;
        ESP_LOGW(TAG, "spi_cam_reset_state: estado preso detectado; forçando limpeza");
        spi_device_get_trans_result(s_spi_cam, &done, pdMS_TO_TICKS(100));
        s_spi_trans_inflight = false;
    }
    memset(&s_spi_trans, 0, sizeof(s_spi_trans));
}

static bool receive_record_header(exp_record_header_t *hdr, const char *tag_prefix,
                                  int expected_order, int expected_count)
{
    spi_cam_reset_state();
    int64_t t0 = esp_timer_get_time();
    int64_t deadline_us = t0 + ((int64_t)RECORD_HEADER_TIMEOUT_MS * 1000LL);
    int64_t next_wait_log_us = t0 + 1000000LL;
    uint32_t poll_count = 0u;
    uint32_t consecutive_poll_failures = 0u;

    ESP_LOGI(TAG, "%s: waiting header", tag_prefix);
    ESP_LOGI(TAG, "%s: transport=SPI freq=%u chunk=%u stack_hwm=%u B",
             tag_prefix,
             (unsigned)EXP_SPI_FREQ_HZ,
             (unsigned)EXP_SPI_PAYLOAD_CHUNK,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    while (1) {
        exp_spi_status_t st = {0};
        int64_t now_us = esp_timer_get_time();
        bool ready_low;
        bool poll_ok;

        if (now_us >= deadline_us) {
            ESP_LOGE(TAG, "%s: timeout aguardando RECORD_READY via SPI", tag_prefix);
            return false;
        }

        ready_low = (gpio_get_level(SPI_CAM_READY_PIN) == 0);
        if (!ready_low) {
            if (!spi_cam_read_header(hdr)) {
                ESP_LOGE(TAG, "%s: falha ao ler header via SPI", tag_prefix);
                return false;
            }
            hdr->seq_num = exp_method_to_seq(hdr->method_id);
            hdr->width = IMG_WIDTH;
            hdr->height = IMG_HEIGHT;
            hdr->colorspace = JPEG_COLORSPACE_RGB;
            hdr->subsampling = (hdr->record_type == EXP_RECORD_RAW_FRAME) ?
                               EXP_SUBSAMPLING_NONE : JPEG_SUBSAMP_444;
            hdr->records_expected = (expected_count > 0 &&
                                     expected_count <= (int)EXP_RECORD_SLOT_COUNT) ?
                                    (uint8_t)expected_count : 1u;
            if (expected_order >= 0 && hdr->order_index != (uint8_t)expected_order) {
                ESP_LOGW(TAG, "%s: header order inesperado %u/%d; tentando ressincronizar",
                         tag_prefix,
                         (unsigned)hdr->order_index,
                         expected_order);
                vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
                continue;
            }
            ESP_LOGI(TAG, "%s: run=%lu method=%s mode=%s order=%u payload=%lu",
                     tag_prefix,
                     (unsigned long)hdr->run_id,
                     exp_method_name(hdr->method_id),
                     metrics_mode_name(hdr->mode_type),
                     (unsigned)hdr->order_index,
                     (unsigned long)hdr->payload_len);
            return true;
        }

        poll_ok = spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                     EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                                     tag_prefix);
        if (!poll_ok) {
            consecutive_poll_failures++;
            if (consecutive_poll_failures >= RECORD_HEADER_MAX_POLL_FAILURES) {
                ESP_LOGE(TAG, "%s: CAM em estado invalido; %lu polls SPI consecutivos sem status valido",
                         tag_prefix, (unsigned long)consecutive_poll_failures);
                return false;
            }
            if (now_us >= next_wait_log_us) {
                ESP_LOGI(TAG, "%s: aguardando status SPI validado falhas=%lu rem_ms=%d",
                         tag_prefix,
                         (unsigned long)consecutive_poll_failures,
                         (int)((deadline_us - now_us) / 1000LL));
                next_wait_log_us = now_us + 1000000LL;
            }
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
            continue;
        }

        consecutive_poll_failures = 0u;
        poll_count++;
        if ((poll_count % RECORD_HEADER_STATUS_LOG_POLLS) == 0u) {
            ESP_LOGI(TAG, "%s: poll=%lu READY=%s state=%u error=%u order=%u payload=%lu rem_ms=%d",
                     tag_prefix,
                     (unsigned long)poll_count,
                     ready_low ? "LOW" : "HIGH",
                     (unsigned)st.state,
                     (unsigned)st.error,
                     (unsigned)st.current_order,
                     (unsigned long)st.payload_len,
                     (int)((deadline_us - now_us) / 1000LL));
        }

        if (st.state == EXP_SPI_STATE_RECORD_READY) {
            if (ready_low) {
                ESP_LOGW(TAG, "%s: status RECORD_READY mas READY=LOW; seguindo por SPI",
                         tag_prefix);
            }
            if (expected_order >= 0 && st.current_order != (uint8_t)expected_order) {
                if (now_us >= next_wait_log_us) {
                    ESP_LOGI(TAG,
                             "%s: aguardando order=%d state=%u order_atual=%u payload=%lu",
                             tag_prefix,
                             expected_order,
                             (unsigned)st.state,
                             (unsigned)st.current_order,
                             (unsigned long)st.payload_len);
                    next_wait_log_us = now_us + 1000000LL;
                }
                vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
                continue;
            }
            if (st.payload_len == 0u) {
                if (now_us >= next_wait_log_us) {
                    ESP_LOGI(TAG,
                             "%s: RECORD_READY sem payload ainda order=%u; aguardando",
                             tag_prefix,
                             (unsigned)st.current_order);
                    next_wait_log_us = now_us + 1000000LL;
                }
                vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
                continue;
            }
            if (!spi_cam_read_header(hdr)) {
                ESP_LOGE(TAG, "%s: falha ao ler header via SPI", tag_prefix);
                return false;
            }
            hdr->seq_num = exp_method_to_seq(hdr->method_id);
            hdr->width = IMG_WIDTH;
            hdr->height = IMG_HEIGHT;
            hdr->colorspace = JPEG_COLORSPACE_RGB;
            hdr->subsampling = (hdr->record_type == EXP_RECORD_RAW_FRAME) ?
                               EXP_SUBSAMPLING_NONE : JPEG_SUBSAMP_444;
            hdr->records_expected = (expected_count > 0 &&
                                     expected_count <= (int)EXP_RECORD_SLOT_COUNT) ?
                                    (uint8_t)expected_count : 1u;
            if (expected_order >= 0 && hdr->order_index != (uint8_t)expected_order) {
                ESP_LOGW(TAG, "%s: header order inesperado %u/%d; tentando ressincronizar",
                         tag_prefix,
                         (unsigned)hdr->order_index,
                         expected_order);
                vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
                continue;
            }
            if (st.payload_len != hdr->payload_len) {
                ESP_LOGW(TAG, "%s: payload_len status/header divergente %lu/%lu",
                         tag_prefix,
                         (unsigned long)st.payload_len,
                         (unsigned long)hdr->payload_len);
            }
            ESP_LOGI(TAG, "%s: run=%lu method=%s mode=%s order=%u payload=%lu",
                     tag_prefix,
                     (unsigned long)hdr->run_id,
                     exp_method_name(hdr->method_id),
                     metrics_mode_name(hdr->mode_type),
                     (unsigned)hdr->order_index,
                     (unsigned long)hdr->payload_len);
            return true;
        }

        if (st.state == EXP_SPI_STATE_ERROR || st.error != EXP_SPI_ERROR_NONE) {
            if (st.error == EXP_SPI_ERROR_CAM_OVF) {
                ESP_LOGE(TAG, "%s: CAM em erro state=%u error=%u status=\"CAM_OVF\" run=%lu",
                         tag_prefix,
                         (unsigned)st.state,
                         (unsigned)st.error,
                         (unsigned long)st.run_id);
            } else {
                ESP_LOGE(TAG, "%s: CAM em erro state=%u error=%u run=%lu",
                         tag_prefix,
                         (unsigned)st.state,
                         (unsigned)st.error,
                         (unsigned long)st.run_id);
            }
            return false;
        }

        if (st.state == EXP_SPI_STATE_DONE) {
            ESP_LOGE(TAG, "%s: CAM sinalizou DONE antes do header", tag_prefix);
            return false;
        }

        if (now_us >= next_wait_log_us) {
            ESP_LOGI(TAG, "%s: aguardando %s state=%u error=%u order=%u payload=%lu",
                     tag_prefix,
                     ready_low ? "READY=HIGH" : "RECORD_READY",
                     (unsigned)st.state,
                     (unsigned)st.error,
                     (unsigned)st.current_order,
                     (unsigned long)st.payload_len);
            next_wait_log_us = now_us + 1000000LL;
        }

        vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
    }
}

static bool receive_record_payload(uint32_t payload_len, uint8_t mode_type,
                                   uint32_t *t_spi_rx_us,
                                   const char *tag_prefix)
{
    int timeout_ms;

    if (payload_len == 0u) {
        if (t_spi_rx_us) *t_spi_rx_us = 0u;
        return true;
    }
    if (payload_len > (uint32_t)IMG_RGB888_BYTES &&
            payload_len > (uint32_t)EXP_JPEG_RLE_WORST_CASE_BYTES) {
        ESP_LOGE(TAG, "%s: payload_len invalido %lu", tag_prefix, (unsigned long)payload_len);
        return false;
    }

    timeout_ms = payload_timeout_ms(payload_len);
    ESP_LOGI(TAG, "%s: lendo payload SPI total=%lu timeout=%d ms",
             tag_prefix, (unsigned long)payload_len, timeout_ms);
    return spi_cam_read_payload(payload_len, mode_type, t_spi_rx_us, tag_prefix);
}

static jpeg_dct_method_t method_id_to_dct(uint8_t method_id)
{
    return exp_method_to_jpeg_dct(method_id);
}

static bool validate_record_against_run(ExperimentRunResult *run, const exp_record_header_t *hdr)
{
    int i;
    const ExperimentRecordResult *base = NULL;
    int slot;

    if (!run || !hdr) return false;

    slot = exp_method_k_to_slot(hdr->method_id, hdr->k_idx);
    if (slot < 0) {
        ESP_LOGW(TAG, "record metodo/k invalido: method=%u k_idx=%u",
                 (unsigned)hdr->method_id, (unsigned)hdr->k_idx);
        return false;
    }
    if (hdr->record_type == EXP_RECORD_RAW_FRAME) {
        if (hdr->mode_type != EXP_MODE_RAW) {
            ESP_LOGW(TAG, "record RAW com mode_type invalido: %u", (unsigned)hdr->mode_type);
            return false;
        }
        if (hdr->method_id != EXP_METHOD_RAW || hdr->subsampling != EXP_SUBSAMPLING_NONE) {
            ESP_LOGW(TAG, "record RAW com metodo/subsampling invalidos");
            return false;
        }
    } else if (hdr->record_type == EXP_RECORD_JPEG_FRAME) {
        if (hdr->mode_type != EXP_MODE_JPEG || !exp_method_is_jpeg(hdr->method_id)) {
            ESP_LOGW(TAG, "record JPEG com metodo/modo invalido");
            return false;
        }
        if (hdr->k_idx >= EXP_K_FACTOR_COUNT) {
            ESP_LOGW(TAG, "record JPEG com k_idx invalido: %u", (unsigned)hdr->k_idx);
            return false;
        }
        if (hdr->colorspace != JPEG_COLORSPACE_RGB || hdr->subsampling != JPEG_SUBSAMP_444) {
            ESP_LOGW(TAG, "record JPEG fora do contexto suportado pelo receiver (RGB/444)");
            return false;
        }
    } else {
        ESP_LOGW(TAG, "record_type invalido: %u", (unsigned)hdr->record_type);
        return false;
    }
    if (hdr->colorspace != JPEG_COLORSPACE_RGB) {
        ESP_LOGW(TAG, "record colorspace invalido: %u", (unsigned)hdr->colorspace);
        return false;
    }
    /* records_expected, width, and height are NOT in the 24-byte wire header
     * (exp_record_header_decode sets width/height from EXP_IMG_WIDTH/HEIGHT but
     * leaves records_expected = 0). Validate using run context instead. */
    if (run->records_expected == 0u || run->records_expected > EXP_RECORD_SLOT_COUNT) {
        ESP_LOGW(TAG, "run records_expected invalido: %u", (unsigned)run->records_expected);
        return false;
    }

    if (run->run_id == 0u) {
        run->run_id = hdr->run_id;
        run->k_idx = hdr->k_idx;
        /* records_expected already set by the caller from exp_records_expected_for_method */
        run->raw_first = (hdr->order_index == 0u && hdr->method_id == EXP_METHOD_RAW);
        return true;
    }

    if (hdr->run_id != run->run_id) {
        ESP_LOGW(TAG, "record run_id %lu difere do esperado %lu",
                 (unsigned long)hdr->run_id, (unsigned long)run->run_id);
        return false;
    }
    if (hdr->width != IMG_WIDTH || hdr->height != IMG_HEIGHT) {
        ESP_LOGW(TAG, "record dimensoes %ux%u diferem do esperado %ux%u",
                 (unsigned)hdr->width, (unsigned)hdr->height, IMG_WIDTH, IMG_HEIGHT);
        return false;
    }
    if (hdr->order_index >= run->records_expected) {
        ESP_LOGW(TAG, "record order_index invalido: %u", (unsigned)hdr->order_index);
        return false;
    }
    if (run->records[slot].present) {
        ESP_LOGW(TAG, "record duplicado para metodo %s k=%s",
                 exp_method_name(hdr->method_id),
                 exp_k_label_from_idx(hdr->k_idx));
        return false;
    }
    if (run->records_expected == 1u && hdr->k_idx != run->k_idx) {
        ESP_LOGW(TAG, "record k_idx %u difere do esperado %u",
                 (unsigned)hdr->k_idx, (unsigned)run->k_idx);
        return false;
    }

    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        if (run->records[i].present) {
            base = &run->records[i];
            break;
        }
    }
    if (!base) {
        run->raw_first = (hdr->order_index == 0u && hdr->method_id == EXP_METHOD_RAW);
    }
    if (base &&
            (hdr->t_capture_us != base->t_capture_us ||
             hdr->t_rgb565_to_rgb888_us != base->t_rgb565_to_rgb888_us)) {
        ESP_LOGW(TAG, "record com tempos de captura/conversao inconsistentes no mesmo run");
        return false;
    }

    return true;
}

static bool process_received_record(ExperimentRunResult *run, const exp_record_header_t *hdr,
                                    uint32_t t_spi_rx_us, uint32_t retry_count)
{
    ExperimentRecordResult *r;
    uint32_t crc_calc;
    int slot;
    int raw_slot = exp_method_to_slot(EXP_METHOD_RAW);

    if (!run || !hdr) return false;
    if (!validate_record_against_run(run, hdr)) {
        return false;
    }
    slot = exp_method_k_to_slot(hdr->method_id, hdr->k_idx);
    if (slot < 0) {
        ESP_LOGE(TAG, "record metodo/k invalido: method=%u k_idx=%u",
                 (unsigned)hdr->method_id, (unsigned)hdr->k_idx);
        return false;
    }

    r = &run->records[slot];
    metrics_init_record(r, hdr, retry_count);
    r->t_spi_rx_us = t_spi_rx_us;

    ESP_LOGI(TAG, "record: processando metodo=%s mode=%s payload=%lu",
             exp_method_name(hdr->method_id),
             metrics_mode_name(hdr->mode_type),
             (unsigned long)hdr->payload_len);

    crc_calc = exp_crc32_payload(s_rx_payload_buf, hdr->payload_len);
    r->crc_ok = (crc_calc == hdr->crc32);
    ESP_LOGI(TAG, "record: crc32 calc=0x%08lX hdr=0x%08lX ok=%d",
             (unsigned long)crc_calc,
             (unsigned long)hdr->crc32,
             (int)r->crc_ok);
    (void)heap_check_stage("after_crc");
    (void)psram_probe_stage("after_crc");
    if (!r->crc_ok) {
        strncpy(r->status, "CRC_FAIL", sizeof(r->status) - 1);
        metrics_set_note(r, "payload crc32 mismatch");
        metrics_finalize_core_time(r);
        ESP_LOGE(TAG, "record: CRC_FAIL; enviando ABORT para limpar stream SPI da CAM");
        (void)spi_cam_abort();
        return true;
    }

    ESP_LOGI(TAG,
             "record: pos-crc mode_type=%u width=%u height=%u subs=%u colorspace=%u",
             (unsigned)hdr->mode_type,
             (unsigned)hdr->width,
             (unsigned)hdr->height,
             (unsigned)hdr->subsampling,
             (unsigned)hdr->colorspace);

    if (hdr->mode_type == EXP_MODE_RAW) {
        int64_t t0;
        int64_t t1;

        if (hdr->payload_len != IMG_RGB888_BYTES) {
            strncpy(r->status, "RAW_SIZE_FAIL", sizeof(r->status) - 1);
            metrics_set_note(r, "unexpected RAW payload size");
            metrics_finalize_core_time(r);
            return true;
        }

        if (!ensure_run_image_slot(run, raw_slot)) {
            strncpy(r->status, "OOM", sizeof(r->status) - 1);
            metrics_set_note(r, "raw buffer allocation failed");
            metrics_finalize_core_time(r);
            return true;
        }

        t0 = esp_timer_get_time();
        copy_bytes_no_libc(run->images[raw_slot], s_rx_payload_buf, IMG_RGB888_BYTES);
        t1 = esp_timer_get_time();
        r->t_raw_unpack_us = (uint32_t)(t1 - t0);
        r->decode_ok = true;
        run->has_reference = true;
        r->compression_ratio_vs_raw = 1.0f;
        metrics_finalize_reference_record(r);
        ESP_LOGI(TAG, "record: RAW processado unpack_us=%lu status=%s",
                 (unsigned long)r->t_raw_unpack_us, r->status);
        return true;
    }

    if (hdr->mode_type == EXP_MODE_JPEG) {
        jpeg_compressed_t *comp = NULL;
        jpeg_image_t recon_view;
        int64_t t_fd0;
        int64_t t_fd1;
        int64_t t_d0;
        int64_t t_d1;
        jpeg_error_t err;

        if (hdr->payload_len == 0u || hdr->payload_len > (uint32_t)EXP_JPEG_RLE_WORST_CASE_BYTES) {
            strncpy(r->status, "JPEG_SIZE_FAIL", sizeof(r->status) - 1);
            metrics_set_note(r, "unexpected JPEG payload size");
            metrics_finalize_core_time(r);
            return true;
        }

        ESP_LOGI(TAG, "record: entrando ramo JPEG payload=%lu",
                 (unsigned long)hdr->payload_len);
        ESP_LOGI(TAG,
                 "record: jpeg alloc params w=%u h=%u subs=%u colorspace=%u",
                 (unsigned)hdr->width,
                 (unsigned)hdr->height,
                 (unsigned)hdr->subsampling,
                 (unsigned)hdr->colorspace);
        if (!prepare_decode_workspace(hdr, &comp) || !comp) {
            strncpy(r->status, "OOM", sizeof(r->status) - 1);
            metrics_set_note(r, "decode workspace unavailable");
            metrics_finalize_core_time(r);
            return true;
        }
        ESP_LOGI(TAG, "record: jpeg workspace pronto comp=%p y_blocks=%ld c_blocks=%ld",
                 (void *)comp, (long)comp->num_blocks_y, (long)comp->num_blocks_chroma);
        if (!psram_probe_stage("before_jpeg_decode")) {
            strncpy(r->status, "PSRAM_FAIL", sizeof(r->status) - 1);
            metrics_set_note(r, "quantized PSRAM write/read test failed");
            metrics_finalize_core_time(r);
            return true;
        }

        ESP_LOGI(TAG, "record: jpeg_frame_payload_decode inicio stack_hwm=%u heap_free=%lu psram_free=%lu",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        t_fd0 = esp_timer_get_time();
        if (jpeg_frame_payload_decode(s_rx_payload_buf, (int32_t)hdr->payload_len, comp) < 0) {
            t_fd1 = esp_timer_get_time();
            r->t_frame_decode_us = (uint32_t)(t_fd1 - t_fd0);
            strncpy(r->status, "DECODE_FAIL", sizeof(r->status) - 1);
            metrics_set_note(r, "jpeg_frame_payload_decode failed");
            metrics_finalize_core_time(r);
            return true;
        }
        t_fd1 = esp_timer_get_time();
        r->t_frame_decode_us = (uint32_t)(t_fd1 - t_fd0);
        ESP_LOGI(TAG, "record: jpeg_frame_payload_decode fim us=%lu", (unsigned long)r->t_frame_decode_us);

        if (!ensure_run_image_slot(run, slot)) {
            strncpy(r->status, "OOM", sizeof(r->status) - 1);
            metrics_set_note(r, "reconstruction buffer allocation failed");
            metrics_finalize_core_time(r);
            return true;
        }
        recon_view.width = hdr->width;
        recon_view.height = hdr->height;
        recon_view.colorspace = (jpeg_colorspace_t)hdr->colorspace;
        recon_view.data = run->images[slot];

        ESP_LOGI(TAG, "record: jpeg_decompress inicio stack_hwm=%u",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        t_d0 = esp_timer_get_time();
        err = jpeg_decompress_into(comp, &recon_view);
        t_d1 = esp_timer_get_time();
        r->t_decompress_us = (uint32_t)(t_d1 - t_d0);
        r->t_idct_kernel_us = comp->idct_kernel_us;
        r->idct_kernel_calls = comp->idct_kernel_calls;
        ESP_LOGI(TAG, "record: jpeg_decompress fim us=%lu idct_us=%lu idct_calls=%lu err=%d recon=%p",
                 (unsigned long)r->t_decompress_us,
                 (unsigned long)r->t_idct_kernel_us,
                 (unsigned long)r->idct_kernel_calls,
                 (int)err,
                 (void *)recon_view.data);
        if (err != JPEG_SUCCESS) {
            strncpy(r->status, "DECOMP_FAIL", sizeof(r->status) - 1);
            metrics_set_note(r, "jpeg_decompress failed");
            metrics_finalize_core_time(r);
            return true;
        }
        r->decode_ok = true;
        strncpy(r->status, "DECODED", sizeof(r->status) - 1);
        metrics_finalize_core_time(r);
        ESP_LOGI(TAG, "record: JPEG processado status=%s frame_decode_us=%lu decompress_us=%lu",
                 r->status,
                 (unsigned long)r->t_frame_decode_us,
                 (unsigned long)r->t_decompress_us);
        return true;
    }

    ESP_LOGE(TAG, "record: mode_type inesperado=%u", (unsigned)hdr->mode_type);
    strncpy(r->status, "MODE_FAIL", sizeof(r->status) - 1);
    metrics_set_note(r, "unknown mode_type");
    metrics_finalize_core_time(r);
    return true;
}

static void uart_emit_run_csv(const ExperimentRunResult *run)
{
    if (!run) return;
    if (!g_uart_csv_header_printed) {
        fputs(metrics_summary_csv_header(), stdout);
        g_uart_csv_header_printed = true;
    }
    for (int i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        const ExperimentRecordResult *r = &run->records[i];
        char row[768];

        if (!r->present) continue;
        metrics_format_summary_csv_row(r, row, (int)sizeof(row));
        fputs(row, stdout);
    }
    fflush(stdout);
}

static void finalize_run_results(ExperimentRunResult *run)
{
    uint32_t raw_bytes = 0u;
    uint32_t tx_raw_us = 0u;
    int i;
    int raw_slot = exp_method_to_slot(EXP_METHOD_RAW);

    if (!run) return;
    if (run->records[raw_slot].present && run->records[raw_slot].crc_ok) {
        raw_bytes = run->records[raw_slot].stream_bytes;
        tx_raw_us = run->records[raw_slot].t_spi_rx_us;
    }

    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        ExperimentRecordResult *r = &run->records[i];

        if (!r->present) continue;
        r->frame_bytes = r->stream_bytes + EXP_RECORD_HEADER_SIZE;
        r->raw_bytes = raw_bytes;
        r->tx_raw_us = tx_raw_us;
        if (run->has_reference && run->images[raw_slot]) {
            if (!metrics_set_reference_sha256_first8(r, run->images[raw_slot], IMG_RGB888_BYTES)) {
                metrics_append_note(r, "reference sha256 first8 unavailable");
            }
        }
        if (raw_bytes > 0u) {
            r->compression_ratio_vs_raw =
                metrics_calc_compression_ratio(raw_bytes, r->stream_bytes);
        }

        if (r->mode_type == EXP_MODE_JPEG && r->decode_ok) {
            if (run->has_reference && run->images[raw_slot]) {
                metrics_compute_jpeg_distortion(r, run->images[raw_slot], run->images[i]);
            } else {
                metrics_set_note(r, "reference RAW missing for PSNR");
                strncpy(r->status, "REF_MISSING", sizeof(r->status) - 1);
            }
        }
    }
    ESP_LOGI(TAG, "run: finalize concluido has_reference=%d",
             (int)run->has_reference);
    uart_emit_run_csv(run);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Telas TFT
 * ═══════════════════════════════════════════════════════════════════════════ */

static void ui_draw_ready(void)
{
    const char *action_b = g_sd_ok ? "A: capturar   B: testar SD" : "A: capturar   B: tentar SD";

    tft_begin();
    tft_fill(TFT_BLACK);
    tft_draw_string(8,   8, "PRONTO", 2, TFT_WHITE,  TFT_BLACK);

    char line[48];
    snprintf(line, sizeof(line), "Metodo: %s", METHOD_NAMES[g_method_idx]);
    tft_draw_string(8,  40, line, 2, TFT_GREEN,  TFT_BLACK);

    snprintf(line, sizeof(line), "k = %s", exp_k_label_from_idx((uint8_t)g_k_idx));
    tft_draw_string(8,  68, line, 2, TFT_YELLOW, TFT_BLACK);

    tft_fill_rect(0, 96, TFT_WIDTH, 1, TFT_DARKGRAY);
    tft_draw_string(4, 104, action_b,                 1, TFT_GRAY, TFT_BLACK);
    tft_draw_string(4, 116, "C: metodo     D: fator k",  1, TFT_GRAY, TFT_BLACK);
    tft_draw_string(4, 128, "A+C 1s: ALL completo",    1, TFT_GRAY, TFT_BLACK);

    /* Indicador de status do link SPI com a CAM */
    if (g_cam_link_ok)
        tft_draw_string(4, 144, "CAM: SPI pronta", 1, TFT_GREEN, TFT_BLACK);
    else
        tft_draw_string(4, 144, "CAM: sem resposta SPI", 1, TFT_RED, TFT_BLACK);
    if (g_sd_ok)
        tft_draw_string(4, 156, "SD: montado / boot log OK", 1, TFT_CYAN, TFT_BLACK);
    else
        tft_draw_string(4, 156, "SD: nao montado", 1, TFT_YELLOW, TFT_BLACK);
    tft_end();

    s_ready_dbg_a = s_ready_dbg_b = s_ready_dbg_c = s_ready_dbg_d = -1;
}

static void ui_draw_ready_inputs(void)
{
    int a = gpio_get_level(BTN_A_PIN);
    int b = gpio_get_level(BTN_B_PIN);
    int c = gpio_get_level(BTN_C_PIN);
    int d = gpio_get_level(BTN_D_PIN);

    if (a == s_ready_dbg_a && b == s_ready_dbg_b &&
        c == s_ready_dbg_c && d == s_ready_dbg_d) {
        return;
    }

    s_ready_dbg_a = a;
    s_ready_dbg_b = b;
    s_ready_dbg_c = c;
    s_ready_dbg_d = d;

    char line[40];
    snprintf(line, sizeof(line), "Btns A=%d B=%d C=%d D=%d", a, b, c, d);

    tft_begin();
    tft_fill_rect(0, 160, TFT_WIDTH, 12, TFT_BLACK);
    tft_draw_string(4, 160, line, 1, TFT_DARKGRAY, TFT_BLACK);
    tft_end();
}

static void ui_draw_processing(const char *step)
{
    tft_begin();
    tft_fill(TFT_BLACK);
    tft_draw_string(8, 100, "PROCESSANDO...", 2, TFT_BLUE, TFT_BLACK);
    if (step)
        tft_draw_string(8, 128, step, 1, TFT_GRAY, TFT_BLACK);
    tft_end();
}

static void ui_draw_error(const char *msg)
{
    tft_begin();
    tft_fill(TFT_BLACK);
    tft_draw_string(8, 100, "ERRO", 2, TFT_RED, TFT_BLACK);
    if (msg)
        tft_draw_string(4, 132, msg, 1, TFT_WHITE, TFT_BLACK);
    tft_end();
    vTaskDelay(pdMS_TO_TICKS(2000));
}

static void ui_draw_info(const char *msg)
{
    tft_begin();
    tft_fill_rect(0, TFT_HEIGHT - 14, TFT_WIDTH, 14, TFT_BLACK);
    tft_draw_string(4, TFT_HEIGHT - 14, msg, 1, TFT_CYAN, TFT_BLACK);
    tft_end();
}

/* Tela de erro específica para falha do link SPI com a CAM. */
static __attribute__((unused)) void ui_draw_handshake_error(void)
{
    tft_begin();
    tft_fill(TFT_BLACK);
    tft_draw_string(4,   4, "LINK SPI FALHOU", 2, TFT_RED, TFT_BLACK);
    tft_fill_rect(0, 36, TFT_WIDTH, 1, TFT_DARKGRAY);
    tft_draw_string(4,  44, "Verifique:", 1, TFT_YELLOW, TFT_BLACK);
    tft_draw_string(4,  58, "[ ] CAM ligada na PCB?",      1, TFT_WHITE, TFT_BLACK);
    tft_draw_string(4,  70, "[ ] Firmware CAM gravado?",   1, TFT_WHITE, TFT_BLACK);
    tft_draw_string(4,  82, "[ ] IO14(SCLK)->CAM IO14?",  1, TFT_WHITE, TFT_BLACK);
    tft_draw_string(4,  94, "[ ] IO13(MOSI), IO12(MISO)?", 1, TFT_WHITE, TFT_BLACK);
    tft_draw_string(4, 106, "[ ] IO6(CS), IO9(READY)?",   1, TFT_WHITE, TFT_BLACK);
    tft_draw_string(4, 118, "[ ] CAM responde a POLL?",    1, TFT_WHITE, TFT_BLACK);
    tft_fill_rect(0, 134, TFT_WIDTH, 1, TFT_DARKGRAY);
    if (g_cam_link_ok)
        tft_draw_string(4, 140, "SPI OK (IDLE detectado)", 1, TFT_GREEN, TFT_BLACK);
    else
        tft_draw_string(4, 140, "SPI: IDLE nao detectado", 1, TFT_RED,   TFT_BLACK);
    tft_end();
    vTaskDelay(pdMS_TO_TICKS(4000));
}

static void ui_draw_result(const uint8_t *rgb888, const ExperimentRecordResult *r)
{
    char line[80];

    if (!rgb888 || !r) return;

    tft_begin();
    tft_draw_image_rgb888(0, 0, r->width, r->height, rgb888);
    tft_fill_rect(0, TFT_HEIGHT - 34, TFT_WIDTH, 34, TFT_BLACK);

    if (r->psnr_db < 0.0f) {
        snprintf(line, sizeof(line), "%s k=%s  PSNR:N/A",
                 r->method_name, exp_k_label_from_idx(r->k_idx));
    } else {
        snprintf(line, sizeof(line), "%s k=%s  PSNR:%.1f",
                 r->method_name, exp_k_label_from_idx(r->k_idx),
                 (double)r->psnr_db);
    }
    tft_draw_string(4, TFT_HEIGHT - 30, line, 1, TFT_WHITE, TFT_BLACK);

    snprintf(line, sizeof(line), "bpp:%.2f core:%.0fms st:%s  B:salvar",
             (double)r->bpp,
             (double)(r->t_total_core_us / 1000.0f),
             r->status);
    tft_draw_string(4, TFT_HEIGHT - 16, line, 1, TFT_CYAN, TFT_BLACK);
    tft_end();
}

static void ui_draw_run_summary(const ExperimentRunResult *run)
{
    char line[96];
    int i;
    int y = 28;
    int total = 0;
    int shown = 0;

    if (!run) return;

    tft_begin();
    tft_fill(TFT_BLACK);
    snprintf(line, sizeof(line), "RUN %04lu", (unsigned long)run->run_id);
    tft_draw_string(4, 4, line, 2, TFT_YELLOW, TFT_BLACK);
    tft_fill_rect(0, 22, TFT_WIDTH, 1, TFT_DARKGRAY);

    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        const ExperimentRecordResult *r = &run->records[i];
        if (!r->present) continue;
        total++;

        if (y + 20 > TFT_HEIGHT - 30) {
            continue;
        }

        snprintf(line, sizeof(line), "%s k=%s  %luB  %.2fbpp",
                 r->method_name,
                 exp_k_label_from_idx(r->k_idx),
                 (unsigned long)r->stream_bytes,
                 (double)r->bpp);
        tft_draw_string(4, y, line, 1, TFT_WHITE, TFT_BLACK);

        if (r->mode_type == EXP_MODE_RAW) {
            snprintf(line, sizeof(line), "REF status:%s core:%.0fms",
                     r->status, (double)(r->t_total_core_us / 1000.0f));
        } else if (r->psnr_db < 0.0f) {
            snprintf(line, sizeof(line), "PSNR:N/A core:%.0fms",
                     (double)(r->t_total_core_us / 1000.0f));
        } else {
            snprintf(line, sizeof(line), "PSNR:%.1f core:%.0fms",
                     (double)r->psnr_db,
                     (double)(r->t_total_core_us / 1000.0f));
        }
        tft_draw_string(4, y + 10, line, 1, TFT_CYAN, TFT_BLACK);
        y += 28;
        shown++;
    }

    if (total > shown) {
        snprintf(line, sizeof(line), "+%d registros no SD/CSV", total - shown);
        tft_draw_string(4, TFT_HEIGHT - 28, line, 1, TFT_YELLOW, TFT_BLACK);
    }

    tft_fill_rect(0, TFT_HEIGHT - 14, TFT_WIDTH, 14, TFT_BLACK);
    tft_draw_string(4, TFT_HEIGHT - 14, "B: salvar  A/C/D: sair", 1, TFT_GREEN, TFT_BLACK);
    tft_end();
}

static void ui_draw_result_timed(const uint8_t *rgb888, ExperimentRecordResult *r)
{
    int64_t t0;
    int64_t t1;

    if (!rgb888 || !r) return;
    t0 = esp_timer_get_time();
    ui_draw_result(rgb888, r);
    t1 = esp_timer_get_time();
    r->t_tft_draw_us = (uint32_t)(t1 - t0);
}

static void ui_draw_run_summary_timed(ExperimentRunResult *run)
{
    int64_t t0;
    int64_t t1;
    int i;
    uint32_t draw_us;

    if (!run) return;
    t0 = esp_timer_get_time();
    ui_draw_run_summary(run);
    t1 = esp_timer_get_time();
    draw_us = (uint32_t)(t1 - t0);
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        if (run->records[i].present) {
            run->records[i].t_tft_draw_us = draw_us;
        }
    }
}

static bool send_config(uint8_t method_id, uint8_t k_idx, uint32_t run_id)
{
    return spi_cam_send_config(method_id, k_idx, run_id);
}

static bool sync_selection_to_cam(void)
{
    if (!g_cam_link_ok) return false;
    return send_config(current_method_id(), (uint8_t)g_k_idx, 0u);
}

static bool start_capture_transaction(uint8_t method_id, uint8_t k_idx,
                                      uint32_t run_id, uint32_t *retry_count_out)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)SPI_TRIGGER_TIMEOUT_MS * 1000LL);

    (void)psram_probe_stage("before_spi_trigger");
    if (!spi_cam_recover_if_error()) {
        ESP_LOGE(TAG, "capture: falha ao recuperar CAM antes do trigger");
        return false;
    }
    ESP_LOGI(TAG, "capture: disparando via SPI sem SET_CONFIG previo...");
    if (!spi_cam_trigger(method_id, k_idx, run_id)) {
        ESP_LOGE(TAG, "capture: trigger SPI falhou run=%lu method=%s",
                 (unsigned long)run_id, exp_method_name(method_id));
        ui_draw_info("SPI: trigger falhou");
        return false;
    }

    while (esp_timer_get_time() < deadline_us) {
        exp_spi_status_t st;

        if (!spi_cam_poll_retry(&st, SPI_XFER_RETRY_TIMEOUT_MS,
                                EXP_SPI_CMD_NOP, 0u, 0u, 0u, 0u, 0u,
                                "capture")) {
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
            continue;
        }
        if (st.run_id != run_id) {
            ESP_LOGW(TAG,
                     "capture: status antigo ignorado state=%u run_rx=%lu run_esperado=%lu",
                     (unsigned)st.state,
                     (unsigned long)st.run_id,
                     (unsigned long)run_id);
            vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
            continue;
        }
        if (st.state == EXP_SPI_STATE_CAPTURING || st.state == EXP_SPI_STATE_RECORD_READY) {
            if (retry_count_out) *retry_count_out = 0u;
            return true;
        }
        if (st.state == EXP_SPI_STATE_ERROR || st.error != EXP_SPI_ERROR_NONE) {
            ESP_LOGE(TAG, "capture: CAM entrou em erro state=%u error=%u",
                     (unsigned)st.state, (unsigned)st.error);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(SPI_STATUS_WAIT_MS));
    }

    ui_draw_info("SPI: timeout no trigger");
    ESP_LOGE(TAG, "capture: timeout no trigger run=%lu method=%s",
             (unsigned long)run_id, exp_method_name(method_id));
    return false;
}

static bool receive_records_for_run(ExperimentRunResult *run, int expected_count, uint32_t retry_count)
{
    int i;
    int target_count = expected_count;

    for (i = 0; i < target_count; i++) {
        exp_record_header_t hdr;
        uint32_t t_spi_rx_us = 0;
        bool processed_ok;

        ESP_LOGI(TAG, "rx_records[%d/%d]: aguardando header", i, target_count - 1);

        if (!receive_record_header(&hdr, "record", i, target_count)) {
            ESP_LOGE(TAG, "rx_records[%d]: FALHA NO HEADER", i);
            spi_cam_abort();
            return false;
        }

        ESP_LOGI(TAG,
                 "rx_records[%d]: HEADER OK method=%s k=%s payload=%lu mode=%s order=%u",
                 i,
                 exp_method_name(hdr.method_id),
                 exp_k_label_from_idx(hdr.k_idx),
                 (unsigned long)hdr.payload_len,
                 metrics_mode_name(hdr.mode_type),
                 (unsigned)hdr.order_index);

        if (!receive_record_payload(hdr.payload_len, hdr.mode_type, &t_spi_rx_us, "record")) {
            ESP_LOGE(TAG,
                     "rx_records[%d]: FALHA NO PAYLOAD method=%s payload=%lu",
                     i, exp_method_name(hdr.method_id), (unsigned long)hdr.payload_len);
            release_rx_payload_buffer();
            spi_cam_abort();
            return false;
        }

        ESP_LOGI(TAG, "rx_records[%d]: PAYLOAD OK tx_us=%lu", i, (unsigned long)t_spi_rx_us);

        if (!spi_cam_read_telemetry(&hdr)) {
            ESP_LOGE(TAG,
                     "rx_records[%d]: FALHA NA TELEMETRIA method=%s",
                     i, exp_method_name(hdr.method_id));
            release_rx_payload_buffer();
            spi_cam_abort();
            return false;
        }

        ESP_LOGI(TAG,
                 "rx_records[%d]: TELEMETRIA OK comp_us=%lu dct_us=%lu",
                 i,
                 (unsigned long)hdr.t_algorithm_us,
                 (unsigned long)hdr.t_dct_kernel_us);

        (void)heap_check_stage("after_payload_rx");
        (void)psram_probe_stage("after_payload_rx");
        processed_ok = process_received_record(run, &hdr, t_spi_rx_us, retry_count);
        release_rx_payload_buffer();
        if (!processed_ok) {
            ESP_LOGE(TAG,
                     "rx_records[%d]: FALHA AO PROCESSAR method=%s",
                     i, exp_method_name(hdr.method_id));
            spi_cam_abort();
            return false;
        }

        ESP_LOGI(TAG, "rx_records[%d]: COMPLETO method=%s", i, exp_method_name(hdr.method_id));
    }
    return true;
}

static void do_single_capture(void)
{
    uint32_t run_id;
    uint8_t method_id = current_method_id();
    uint32_t retry_count = 0;
    ExperimentRecordResult *r;

    run_reset(&g_run);
    g_run_id_persistent = false;
    g_has_result = false;
    g_state = STATE_CAPTURING;
    tft_end();
    led_set(LED_BLUE);
    /* Sem redraw de TFT antes/durante a transacao SPI critica.
     * A tela permanece no estado anterior ate o registro terminar. */

    run_id = current_run_id();
    ESP_LOGI(TAG, "capture: usando run_id temporario=%lu", (unsigned long)run_id);
    if (!start_capture_transaction(method_id, (uint8_t)g_k_idx, run_id, &retry_count)) {
        led_set(LED_RED);
        vTaskDelay(pdMS_TO_TICKS(500));
        led_set(LED_GREEN);
        g_state = STATE_READY;
        ui_draw_ready();
        return;
    }

    g_run.run_id = run_id;
    g_run.k_idx = (uint8_t)g_k_idx;
    g_run.records_expected = exp_records_expected_for_method(method_id);
    g_run.retry_count = retry_count;
    if (!receive_records_for_run(&g_run, 1, retry_count)) {
        ui_draw_error("falha ao receber registro");
        g_state = STATE_READY;
        ui_draw_ready();
        return;
    }
    ESP_LOGI(TAG, "capture: recepcao concluida; finalizando run");
    finalize_run_results(&g_run);
    g_has_result = true;
    g_last_display_method = method_id;
    g_last_display_k_idx = (uint8_t)g_k_idx;
    g_state = STATE_DISPLAY;
    {
        int method_slot = exp_method_k_to_slot(method_id, (uint8_t)g_k_idx);
        led_set(LED_GREEN);
        if (method_slot >= 0) {
            r = &g_run.records[method_slot];
        } else {
            r = NULL;
        }
        if (method_slot >= 0 && r && g_run.images[method_slot] && r->present) {
            ESP_LOGI(TAG, "capture: desenhando resultado slot=%d status=%s",
                     method_slot, r->status);
            ui_draw_result_timed(g_run.images[method_slot], r);
            ESP_LOGI(TAG, "capture: resultado desenhado");
            ESP_LOGI(TAG, "capture: autosave iniciando");
            do_save_current_run();
            ESP_LOGI(TAG, "capture: autosave concluido");
        } else {
            ui_draw_error("registro nao reconstruido");
            g_state = STATE_READY;
            ui_draw_ready();
        }
    }
}

static void do_full_experiment(void)
{
    uint32_t run_id;
    uint32_t retry_count = 0;

    run_reset(&g_run);
    g_run_id_persistent = false;
    g_has_result = false;
    g_state = STATE_COMPARE;
    tft_end();
    led_set(LED_YELLOW);
    /* Mantem o TFT quieto no caminho critico de enlace. */

    run_id = current_run_id();
    ESP_LOGI(TAG, "compare: usando run_id temporario=%lu", (unsigned long)run_id);
    if (!start_capture_transaction(EXP_METHOD_ALL, (uint8_t)g_k_idx, run_id, &retry_count)) {
        led_set(LED_RED);
        vTaskDelay(pdMS_TO_TICKS(500));
        led_set(LED_GREEN);
        g_state = STATE_READY;
        ui_draw_ready();
        return;
    }

    g_run.run_id = run_id;
    g_run.k_idx = (uint8_t)g_k_idx;
    g_run.records_expected = exp_records_expected_for_method(EXP_METHOD_ALL);
    g_run.retry_count = retry_count;
    if (!receive_records_for_run(&g_run, g_run.records_expected, retry_count)) {
        int present_count;

        ESP_LOGW(TAG, "ensaio %lu terminou de forma parcial", (unsigned long)run_id);
        finalize_run_results(&g_run);
        present_count = run_present_record_count(&g_run);
        ESP_LOGW(TAG, "compare: parcial tem %d registro(s) presentes", present_count);

        if (present_count <= 1) {
            /* Only RAW (or nothing) - not a useful experiment partial.
             * Log it for diagnosis but do not pollute SD with a lone RAW run. */
            ESP_LOGE(TAG,
                     "compare: parcial inutil (%d registro(s)); falhou logo apos RAW - verifique serial",
                     present_count);
            ui_draw_error("falhou apos RAW\nver serial p/ causa");
            g_has_result = false;
            g_state = STATE_READY;
            ui_draw_ready();
            return;
        }

        g_has_result = true;
        g_state = STATE_COMPARE;
        led_set(LED_YELLOW);
        ui_draw_run_summary_timed(&g_run);
        ui_draw_info("ensaio parcial");
        ESP_LOGW(TAG, "compare: autosave parcial iniciando");
        do_save_current_run();
        ESP_LOGW(TAG, "compare: autosave parcial concluido");
        return;
    }

    finalize_run_results(&g_run);
    g_has_result = true;
    g_state = STATE_COMPARE;
    led_set(LED_GREEN);
    ui_draw_run_summary_timed(&g_run);
    ESP_LOGI(TAG, "compare: autosave iniciando");
    do_save_current_run();
    ESP_LOGI(TAG, "compare: autosave concluido");
}

static void do_benchmark_sweep(void)
{
    ESP_LOGI(TAG, "benchmark: iniciando ALL completo em uma captura");
    ui_draw_processing("BENCH ALL");
    do_full_experiment();
    ESP_LOGI(TAG, "benchmark: ALL completo finalizado");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Lazy-mount do SD
 * ═══════════════════════════════════════════════════════════════════════════ */

static bool ensure_sd_mounted(void)
{
    if (g_sd_ok) return true;

    /* Cooldown: não retenta dentro de 30 s após falha */
    if (s_sd_fail_us > 0) {
        int64_t elapsed = esp_timer_get_time() - s_sd_fail_us;
        if (elapsed < SD_RETRY_COOLDOWN_US) {
            int rem_s = (int)((SD_RETRY_COOLDOWN_US - elapsed) / 1000000LL);
            char msg[48];
            snprintf(msg, sizeof(msg), "SD falhou - aguarde %ds", rem_s);
            ESP_LOGW(TAG, "ensure_sd_mounted: %s", msg);
            ui_draw_info(msg);
            vTaskDelay(pdMS_TO_TICKS(1500));
            return false;
        }
    }

    ESP_LOGI(TAG, "ensure_sd_mounted: montando SD... (A/C/D = cancela)");
    ui_draw_info("SD: montando... (A/C/D=cancela)");

    g_sd_ok = sd_init(sd_cancel_check);

    if (!g_sd_ok) {
        s_sd_fail_us = esp_timer_get_time();
        ESP_LOGE(TAG, "ensure_sd_mounted: falha ao montar SD");
        ui_draw_info("SD: falha! (sem cartao?)");
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        s_sd_fail_us = 0;   /* reseta cooldown após sucesso */
        ESP_LOGI(TAG, "ensure_sd_mounted: SD montado com sucesso");
    }
    return g_sd_ok;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Salvar no SD
 * ═══════════════════════════════════════════════════════════════════════════ */

static void do_save_current_run(void)
{
    if (!g_has_result) return;
    if (!ensure_sd_mounted()) {
        int display_slot = exp_method_k_to_slot(g_last_display_method, g_last_display_k_idx);
        if (g_state == STATE_DISPLAY && display_slot >= 0 && g_run.images[display_slot]) {
            ui_draw_result_timed(g_run.images[display_slot],
                                 &g_run.records[display_slot]);
        } else {
            ui_draw_run_summary_timed(&g_run);
        }
        return;
    }

    if (!ensure_run_persistent_id(&g_run)) {
        ui_draw_error("erro ao reservar run_id");
        return;
    }

    ui_draw_processing("SALVANDO...");
    if (!sd_save_experiment_run(&g_run)) {
        ui_draw_error("erro ao salvar no SD");
        return;
    }

    {
        int display_slot = exp_method_k_to_slot(g_last_display_method, g_last_display_k_idx);
        if (g_state == STATE_DISPLAY && display_slot >= 0 && g_run.images[display_slot]) {
            ui_draw_result_timed(g_run.images[display_slot], &g_run.records[display_slot]);
        } else {
            ui_draw_run_summary_timed(&g_run);
        }
    }
}

static bool ensure_run_persistent_id(ExperimentRunResult *run)
{
    int i;
    uint32_t persistent_id;

    if (!run) return false;
    if (g_run_id_persistent) return true;

    persistent_id = sd_reserve_run_id();
    if (persistent_id == 0u) {
        ESP_LOGE(TAG, "autosave: sd_reserve_run_id retornou 0");
        return false;
    }

    ESP_LOGI(TAG, "autosave: promovendo run_id temporario=%lu para persistente=%lu",
             (unsigned long)run->run_id, (unsigned long)persistent_id);
    run->run_id = persistent_id;
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        if (run->records[i].present) {
            run->records[i].run_id = persistent_id;
        }
    }
    g_run_id_persistent = true;
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Teste de SD em STATE_READY (BTN B sem captura prévia)
 *  Monta o SD e cria /sdcard/s3_test.txt para confirmar que tudo funciona.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void do_test_sd(void)
{
    ESP_LOGI(TAG, "do_test_sd: teste de SD...");
    if (!ensure_sd_mounted()) {
        ESP_LOGE(TAG, "do_test_sd: SD nao montou");
        return;
    }

    FILE *f = fopen(SD_MOUNT "/s3_test.txt", "w");
    if (!f) {
        ESP_LOGE(TAG, "do_test_sd: nao conseguiu criar s3_test.txt");
        ui_draw_info("SD: erro ao criar arquivo!");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    int64_t up = esp_timer_get_time();
    fprintf(f, "IC-JPEG S3 - teste SD\n");
    fprintf(f, "uptime: %lld.%03lld s\n", up / 1000000LL, (up % 1000000LL) / 1000LL);
    fprintf(f, "metodo: %s  k: %s\n", display_method_name(),
            exp_k_label_from_idx((uint8_t)g_k_idx));
    fprintf(f, "cam_link_ok: %d\n", (int)g_cam_link_ok);
    fclose(f);

    ESP_LOGI(TAG, "do_test_sd: s3_test.txt salvo");
    ui_draw_info("SD OK: s3_test.txt salvo");
    vTaskDelay(pdMS_TO_TICKS(2000));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  controller_task - tarefa principal do sistema (16 KB de stack)
 *
 *  O app_main padrão do ESP-IDF tem apenas 3584 bytes de stack, insuficiente
 *  para as chamadas SPI + TFT + snprintf + codec. A solução padrão RTOS é
 *  lançar a lógica do aplicativo em uma tarefa dedicada com stack adequado.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CONTROLLER_STACK_SIZE  (16 * 1024)

static void controller_task(void *arg)
{
    (void)arg;
    const esp_app_desc_t *app = esp_app_get_description();

    /* Aguarda USB CDC conectar (~600 ms) */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* ── Motivo do reset ── */
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rr_str = reset_reason_to_string(rr);
    ESP_LOGW(TAG, "╔══════════════════════════════════╗");
    ESP_LOGW(TAG, "║  MOTIVO DO RESET: %-15s ║", rr_str);
    ESP_LOGW(TAG, "╚══════════════════════════════════╝");

    ESP_LOGI(TAG, "=== IC-JPEG S3 boot ===");
    led_hw_init();
    ESP_LOGI(TAG, "LED init OK (IO%d)", LED_PIN);
    ESP_LOGI(TAG, "versao libimage: %s", jpeg_version());
    ESP_LOGI(TAG, "build: project=%s version=%s time=%s %s proto_v=%u spi=%u",
             app ? app->project_name : "unknown",
             app ? app->version : "unknown",
             app ? app->date : "?",
             app ? app->time : "?",
             (unsigned)EXP_RECORD_VERSION,
             (unsigned)EXP_SPI_FREQ_HZ);
    ESP_LOGI(TAG, "transport: outer-link=SPI freq=%u chunk=%u stack_hwm=%u B",
             (unsigned)EXP_SPI_FREQ_HZ,
             (unsigned)EXP_SPI_PAYLOAD_CHUNK,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    ESP_LOGI(TAG, "GPIO setup...");
    gpio_setup();
    ESP_LOGI(TAG, "GPIO OK - A=%d B=%d C=%d D=%d",
             gpio_get_level(BTN_A_PIN), gpio_get_level(BTN_B_PIN),
             gpio_get_level(BTN_C_PIN), gpio_get_level(BTN_D_PIN));

    ESP_LOGI(TAG, "SPI-CAM pins: SCLK=IO%d MOSI=IO%d MISO=IO%d CS=IO%d READY=IO%d",
             SPI_CAM_SCLK_PIN, SPI_CAM_MOSI_PIN, SPI_CAM_MISO_PIN,
             SPI_CAM_CS_PIN, SPI_CAM_READY_PIN);

    if (!ensure_spi_buffers()) {
        ESP_LOGE(TAG, "SPI buffers nao puderam ser alocados - halting");
        vTaskDelete(NULL);
        return;
    }
    spi_cam_setup();
    ESP_LOGI(TAG, "RX payload buffer sob demanda (RAW=%u JPEG_RLE=%u bytes)",
             (unsigned)IMG_RGB888_BYTES, (unsigned)JPEG_FRAME_MAX_PAYLOAD_BYTES);
    run_reset(&g_run);
    if (!ensure_run_image_buffers(&g_run)) {
        ESP_LOGE(TAG, "run image buffers nao puderam ser alocados - halting");
        vTaskDelete(NULL);
        return;
    }
    if (!ensure_decode_workspace()) {
        ESP_LOGE(TAG, "decode workspace nao pode ser alocado - halting");
        vTaskDelete(NULL);
        return;
    }
    (void)psram_probe_stage("after_workspace_alloc");
    if (!metrics_init_ssim_workspace(IMG_WIDTH)) {
        ESP_LOGW(TAG, "SSIM workspace nao pode ser pre-alocado; usando caminho sem workspace dedicado");
    }

    ESP_LOGI(TAG, "SPI-CAM: aguardando status IDLE/DONE (timeout %d s)...",
             SPI_POLL_TIMEOUT_MS / 1000);
    g_cam_link_ok = spi_cam_wait_ready(SPI_POLL_TIMEOUT_MS);
    if (g_cam_link_ok) {
        ESP_LOGI(TAG, "SPI-CAM: CAM respondeu ao POLL - firmware CAM OK!");
        if (sync_selection_to_cam()) {
            ESP_LOGI(TAG, "Config inicial enviada para sincronizar CAM");
        }
    } else {
        ESP_LOGW(TAG, "SPI-CAM: sem resposta ao POLL (timeout %d s)",
                 SPI_POLL_TIMEOUT_MS / 1000);
        ESP_LOGW(TAG, "  Verifique: CAM ligada? Firmware gravado? Fios SPI?");
    }

    ESP_LOGI(TAG, "SPI-CAM permanece ativo continuamente");

    try_write_boot_debug_log(rr, app);
    (void)psram_probe_stage("after_sd_mount");

    /* ── TFT - init pode levar ~5-7 s (SW reset + fills de teste) ── */
    ESP_LOGI(TAG, "--- TFT init ---");
    tft_begin();
    tft_init();
    tft_end();
    ESP_LOGI(TAG, "--- TFT init concluido (controlador: %s) ---",
             tft_get_controller() == TFT_CTRL_ILI9341 ? "ILI9341" :
             tft_get_controller() == TFT_CTRL_ST7789V  ? "ST7789V" : "UNKNOWN");
    (void)psram_probe_stage("after_tft_init");

    if (g_sd_ok)
        ESP_LOGI(TAG, "SD: pronto desde o boot (boot_log.txt atualizado)");
    else
        ESP_LOGI(TAG, "SD: boot probe falhou; BTN B pode tentar novamente");

    /* ── Splash screen ── */
    tft_begin();
    tft_fill(TFT_BLACK);
    tft_draw_string(20, 50, "IC-JPEG Camera", 2, TFT_CYAN,   TFT_BLACK);
    tft_draw_string(32, 76, jpeg_version(),   1, TFT_GRAY,   TFT_BLACK);

    /* Status do link SPI-CAM */
    if (g_cam_link_ok)
        tft_draw_string(4, 96, "CAM: SPI conectada", 1, TFT_GREEN, TFT_BLACK);
    else
        tft_draw_string(4, 96, "CAM: sem resposta SPI", 1, TFT_RED, TFT_BLACK);

    if (g_sd_ok)
        tft_draw_string(4, 110, "SD: boot_log.txt atualizado", 1, TFT_GREEN, TFT_BLACK);
    else
        tft_draw_string(4, 110, "SD: boot probe falhou", 1, TFT_YELLOW, TFT_BLACK);

    char info[48];
    snprintf(info, sizeof(info), "Metodo: %s  k=%s",
             METHOD_NAMES[g_method_idx], exp_k_label_from_idx((uint8_t)g_k_idx));
    tft_draw_string(8, 128, info, 1, TFT_YELLOW, TFT_BLACK);
    tft_end();

    ESP_LOGI(TAG, "Splash OK - LED verde");
    led_set(LED_GREEN);
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Marca tick de boot para imunidade do IO46 */
    s_boot_tick       = xTaskGetTickCount();
    s_boot_guard_done = false;

    g_state = STATE_READY;
    ui_draw_ready();
    (void)psram_probe_stage("before_main_loop");
    ESP_LOGI(TAG, "=== PRONTO - entrando no loop principal ===");

    /* ── Loop principal ─────────────────────────────────────────────────── */
    while (1) {
        btn_poll_all();
        if (g_state == STATE_READY) {
            ui_draw_ready_inputs();
        }

        /* ── Heartbeat diagnóstico: TOPO do loop ─────────────────────────
         * Posição ANTES dos checks de botão e do vTaskDelay.
         * Se LOOP[N] aparece mas LOOP[N+1] não → travou dentro da iteração N.  */
        {
            static int s_loop_iter = 0;
            s_loop_iter++;
            if (s_loop_iter <= 5 || s_loop_iter % 250 == 0) {
                ESP_LOGI(TAG, "LOOP[%d] A=%d B=%d C=%d D=%d st=%d",
                         s_loop_iter,
                         gpio_get_level(BTN_A_PIN),
                         gpio_get_level(BTN_B_PIN),
                         gpio_get_level(BTN_C_PIN),
                         gpio_get_level(BTN_D_PIN),
                         (int)g_state);
            }
        }

        /* ── A+C por 1 s → ensaio completo RAW + JPEG ── */
        if (g_state == STATE_READY && btn_is_low(BTN_A_PIN)) {
            if (btn_ac_held_1s()) {
                do_benchmark_sweep();
                continue;
            }
        }

        /* ── Botão A: captura em READY; sai de telas de resultado ── */
        if (btn_pressed(BTN_A_PIN)) {
            if (g_state == STATE_READY) {
                ESP_LOGI(TAG, "BTN A - %s k=%s",
                         display_method_name(), exp_k_label_from_idx((uint8_t)g_k_idx));
                if (current_method_id() == EXP_METHOD_ALL) {
                    do_full_experiment();
                } else {
                    do_single_capture();
                }
            } else if (g_state == STATE_DISPLAY || g_state == STATE_COMPARE) {
                g_state = STATE_READY;
                led_set(LED_GREEN);
                ui_draw_ready();
            }
            continue;
        }

        /* ── Botão B: salvar (DISPLAY/COMPARE) ou testar SD (READY) ── */
        if (btn_b_safe()) {
            ESP_LOGI(TAG, "BTN B - estado=%d sd_ok=%d has_result=%d",
                     (int)g_state, (int)g_sd_ok, (int)g_has_result);
            if (g_state == STATE_DISPLAY || g_state == STATE_COMPARE) {
                do_save_current_run();
            } else if (g_state == STATE_READY) {
                do_test_sd();
            }
            continue;
        }

        /* ── Botão C: cicla método (Loeffler → Matrix → RDCT → Silveira → All) ── */
        if (btn_pressed(BTN_C_PIN)) {
            ESP_LOGI(TAG, "BTN C - metodo %s -> %s",
                     METHOD_NAMES[g_method_idx],
                     METHOD_NAMES[(g_method_idx + 1) % NUM_METHODS]);
            if (g_state == STATE_READY || g_state == STATE_DISPLAY || g_state == STATE_COMPARE) {
                g_method_idx = (g_method_idx + 1) % NUM_METHODS;
                g_state      = STATE_READY;
                led_set(LED_GREEN);
                ui_draw_ready();
                ui_draw_ready_inputs();
                ESP_LOGI(TAG, "Metodo atualizado localmente; CAM recebera no TRIGGER");
            }
            continue;
        }

        /* ── Botão D: cicla k ── */
        if (btn_pressed(BTN_D_PIN)) {
            ESP_LOGI(TAG, "BTN D - k %s -> %s",
                     exp_k_label_from_idx((uint8_t)g_k_idx),
                     exp_k_label_from_idx((uint8_t)((g_k_idx + 1) % EXP_K_FACTOR_COUNT)));
            if (g_state == STATE_READY || g_state == STATE_DISPLAY || g_state == STATE_COMPARE) {
                g_k_idx = (g_k_idx + 1) % EXP_K_FACTOR_COUNT;
                g_state = STATE_READY;
                led_set(LED_GREEN);
                ui_draw_ready();
                ui_draw_ready_inputs();
                ESP_LOGI(TAG, "k atualizado localmente; CAM recebera no TRIGGER");
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(20));

        /* Link SPI permanece ativo continuamente. */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  app_main - ponto de entrada do ESP-IDF (stack padrão: 3584 bytes)
 *
 *  Faz apenas o mínimo crítico (SYNC LOW para strapping do CAM) e lança
 *  a controller_task com 16 KB de stack no core 0 (APP_CPU fica livre).
 * ═══════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    /* Niveis seguros do link SPI antes do boot da CAM:
     * IO14/SCLK LOW, IO6/CS LOW e IO13/MOSI LOW. O READY da CAM fica em
     * IO9 como entrada com pull-down; a CAM só sobe IO2 depois de pronta. */
    gpio_reset_pin(SPI_CAM_SCLK_PIN);
    gpio_set_direction(SPI_CAM_SCLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CAM_SCLK_PIN, 0);

    gpio_reset_pin(SPI_CAM_CS_PIN);
    gpio_set_direction(SPI_CAM_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CAM_CS_PIN, 0);

    gpio_reset_pin(SPI_CAM_MOSI_PIN);
    gpio_set_direction(SPI_CAM_MOSI_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SPI_CAM_MOSI_PIN, 0);

    xTaskCreatePinnedToCore(controller_task, "ctrl",
                            CONTROLLER_STACK_SIZE, NULL,
                            5,      /* prioridade - acima de idle(0), abaixo de ISR */
                            NULL,   /* handle - não precisamos */
                            0);     /* core 0 - mesmo core do app_main original */
}
