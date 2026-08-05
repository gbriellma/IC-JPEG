/**
 * @file main.c  (ESP32-CAM)
 * @brief Captura única + ramos RAW/JPEG + transporte SPI slave para o ensaio experimental.
 *
 * A CAM captura uma cena, converte RGB565 -> RGB888 uma única vez e congela
 * esse buffer base. A partir dele, emite registros independentes:
 *   - RAW (payload RGB888 puro)
 *   - JPEG LOEFFLER
 *   - JPEG MATRIX
 *   - JPEG RDCT
 *   - JPEG SILVEIRA_J3
 *   - JPEG SILVEIRA_J7
 *
 * O payload JPEG usa o frame interno versionado:
 *   [0xA7][version=2][LEN_BE32][payload...]
 *
 * O transporte experimental ponto a ponto adiciona um cabeçalho externo de
 * registro com metadados, tempos, run_id, CRC32 e identificação do ramo.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 *  Pinagem do link CAM <-> S3
 * ═══════════════════════════════════════════════════════════════════════════
 *  SPI   CAM IO12(MISO) → S3 IO12(MISO)
 *        CAM IO13(MOSI) ← S3 IO13(MOSI)
 *        CAM IO14(SCLK) ← S3 IO14(SCLK)
 *        CAM IO15(CS)   ← S3 IO6 (CS)
 *        CAM IO2(READY) → S3 IO9 (READY_IN)
 *
 * O caminho RAW é paralelo e não interfere no codec JPEG: ele apenas lê o
 * mesmo buffer RGB888 congelado e o empacota como registro próprio.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "jpeg_codec.h"
#include "experiment_protocol.h"

static const char *TAG = "cam";

#ifndef CAM_DISABLE_BROWNOUT
#define CAM_DISABLE_BROWNOUT  0
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 *  Pinos do link SPI
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SPI_MISO_PIN     12         /* CAM IO12 → S3 IO12 */
#define SPI_MOSI_PIN     13         /* CAM IO13 ← S3 IO13 */
#define SPI_SCLK_PIN     14         /* CAM IO14 ← S3 IO14 */
#define SPI_CS_PIN       15         /* CAM IO15 ← S3 IO6 */
#define SPI_READY_PIN    EXP_SPI_CAM_READY_PIN
#define FLASH_PIN         4         /* flash onboard */

#define SPI_LINK_HOST    SPI2_HOST
#define SPI_LINK_MODE    EXP_SPI_MODE
#define SPI_LINK_FREQ_HZ ((int)EXP_SPI_FREQ_HZ)
#define SPI_STATUS_WAIT_MS       100
#define SPI_RECORD_WAIT_MS       100
#define SPI_RECORD_TIMEOUT_MS  30000   /* timeout terminal para envio de registro */
#define SPI_STATUS_DMA_BYTES      64u
#define SPI_CTRL_DMA_BYTES        64u
#define SPI_DMA_ALLOC_CAPS        (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define SPI_RECORD_LOG_STEP_BYTES (32u * 1024u)
#define SPI_PAYLOAD_FIRST_CHUNK   64u

/* ═══════════════════════════════════════════════════════════════════════════
 *  Pinos da câmera - AI-Thinker ESP32-CAM (OV2640)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22
#define XCLK_FREQ_HZ    8000000

#define IMG_WIDTH   320
#define IMG_HEIGHT  240
#define RGB888_BUF_SIZE  ((size_t)IMG_WIDTH * (size_t)IMG_HEIGHT * 3u)
#define RGB565_BUF_SIZE  ((size_t)IMG_WIDTH * (size_t)IMG_HEIGHT * 2u)
#define CAMERA_BOOT_STABILIZE_MS 300u
#define CAMERA_WARMUP_FRAMES 3u
#define CAMERA_PRECAPTURE_DISCARD_FRAMES 8u
#define CAMERA_WARMUP_GAP_MS 120u
#define AEC_STABLE_FRAMES    3u
#define AEC_STABLE_DELTA     4u
#define AEC_WAIT_MAX_FRAMES 25u
#define EXPOSURE_MEAN_MIN   20u
#define EXPOSURE_MEAN_MAX  215u
#define EXPOSURE_MAX_RETRIES 3u
#define CAMERA_SENSOR_VFLIP 0
#define CAMERA_SENSOR_HMIRROR 0

/* ═══════════════════════════════════════════════════════════════════════════
 *  Constantes do protocolo
 * ═══════════════════════════════════════════════════════════════════════════ */

#define RAW_ORDER_FIRST_DEFAULT  1

/* ═══════════════════════════════════════════════════════════════════════════
 *  Estado
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t s_method   = EXP_METHOD_LOEFFLER;
static uint8_t s_k_idx    = 0;
static float   s_quality  = 0.1f;
static bool    s_all_mode = false;
static bool    s_raw_first = RAW_ORDER_FIRST_DEFAULT ? true : false;
static uint32_t s_run_id = 1;

static TaskHandle_t s_main_task   = NULL;
static TaskHandle_t s_spi_task    = NULL;
static uint8_t     *s_rgb888_buf  = NULL;
static uint8_t     *s_recon_buf   = NULL;
static uint8_t     *s_frame_buf   = NULL;
static uint8_t      s_record_hdr_buf[EXP_RECORD_HEADER_SIZE];
static uint8_t      s_record_telemetry_buf[EXP_RECORD_TELEMETRY_SIZE];
static uint8_t     *s_spi_status_tx_buf = NULL;
static uint8_t     *s_spi_ctrl_rx_buf = NULL;
static uint8_t     *s_spi_dma_chunk_buf = NULL;
static uint32_t     s_invalid_done_ctrl_count = 0u;
static uint32_t     s_spi_trans_count = 0u;

#define SPI_ARM_NONE    0u
#define SPI_ARM_HEADER  1u
#define SPI_ARM_PAYLOAD 2u
#define SPI_ARM_TELEMETRY 3u

typedef struct {
    uint8_t  state;
    uint8_t  error;
    uint8_t  records_expected;
    uint8_t  current_order;
    uint32_t run_id;
    uint32_t payload_len;
    uint32_t header_offset;
    uint32_t payload_offset;
    const uint8_t *payload_ptr;
    uint32_t telemetry_len;
    uint32_t telemetry_offset;
    const uint8_t *telemetry_ptr;
    bool     header_pending;
    bool     telemetry_pending;
    uint8_t  tx_armed_kind;
    bool     abort_requested;
    bool     worker_busy;
} cam_spi_runtime_t;

static cam_spi_runtime_t s_spi_rt = {
    .state = EXP_SPI_STATE_BOOTING,
    .error = EXP_SPI_ERROR_NONE,
};
static portMUX_TYPE s_spi_lock = portMUX_INITIALIZER_UNLOCKED;

static void spi_ready_set(bool ready)
{
    gpio_set_level(SPI_READY_PIN, ready ? 1 : 0);
}

static void spi_set_state(uint8_t state, uint8_t error)
{
    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.state = state;
    s_spi_rt.error = error;
    if (state != EXP_SPI_STATE_RECORD_READY) {
        s_spi_rt.payload_len = 0u;
        s_spi_rt.header_offset = 0u;
        s_spi_rt.payload_offset = 0u;
        s_spi_rt.payload_ptr = NULL;
        s_spi_rt.telemetry_len = 0u;
        s_spi_rt.telemetry_offset = 0u;
        s_spi_rt.telemetry_ptr = NULL;
        s_spi_rt.header_pending = false;
        s_spi_rt.telemetry_pending = false;
        s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
    }
    portEXIT_CRITICAL(&s_spi_lock);
    if (state != EXP_SPI_STATE_RECORD_READY) {
        spi_ready_set(false);
    }
}

static bool spi_is_abort_requested(void)
{
    bool abort_requested;

    portENTER_CRITICAL(&s_spi_lock);
    abort_requested = s_spi_rt.abort_requested;
    portEXIT_CRITICAL(&s_spi_lock);
    return abort_requested;
}

static void spi_clear_abort(void)
{
    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.abort_requested = false;
    portEXIT_CRITICAL(&s_spi_lock);
}

static void spi_mark_idle(void)
{
    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.state = EXP_SPI_STATE_IDLE;
    s_spi_rt.error = EXP_SPI_ERROR_NONE;
    s_spi_rt.records_expected = 0u;
    s_spi_rt.current_order = 0u;
    s_spi_rt.run_id = 0u;
    s_spi_rt.payload_len = 0u;
    s_spi_rt.header_offset = 0u;
    s_spi_rt.payload_offset = 0u;
    s_spi_rt.payload_ptr = NULL;
    s_spi_rt.telemetry_len = 0u;
    s_spi_rt.telemetry_offset = 0u;
    s_spi_rt.telemetry_ptr = NULL;
    s_spi_rt.header_pending = false;
    s_spi_rt.telemetry_pending = false;
    s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
    s_spi_rt.abort_requested = false;
    s_spi_rt.worker_busy = false;
    portEXIT_CRITICAL(&s_spi_lock);
    spi_ready_set(false);
}

static void spi_mark_done(uint32_t run_id)
{
    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.state = EXP_SPI_STATE_DONE;
    s_spi_rt.error = EXP_SPI_ERROR_NONE;
    s_spi_rt.run_id = run_id;
    s_spi_rt.payload_len = 0u;
    s_spi_rt.header_offset = 0u;
    s_spi_rt.payload_offset = 0u;
    s_spi_rt.payload_ptr = NULL;
    s_spi_rt.telemetry_len = 0u;
    s_spi_rt.telemetry_offset = 0u;
    s_spi_rt.telemetry_ptr = NULL;
    s_spi_rt.header_pending = false;
    s_spi_rt.telemetry_pending = false;
    s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
    s_spi_rt.worker_busy = false;
    portEXIT_CRITICAL(&s_spi_lock);
    spi_ready_set(false);
}

static void spi_mark_idle_after_done(uint32_t run_id, const char *prefix)
{
    vTaskDelay(pdMS_TO_TICKS(150));
    spi_mark_idle();
    ESP_LOGI(TAG, "%s: CAM voltou para IDLE apos run=%lu",
             prefix ? prefix : "capture",
             (unsigned long)run_id);
}

static void spi_mark_error(uint8_t error)
{
    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.state = EXP_SPI_STATE_ERROR;
    s_spi_rt.error = error;
    s_spi_rt.payload_len = 0u;
    s_spi_rt.header_offset = 0u;
    s_spi_rt.payload_offset = 0u;
    s_spi_rt.payload_ptr = NULL;
    s_spi_rt.telemetry_len = 0u;
    s_spi_rt.telemetry_offset = 0u;
    s_spi_rt.telemetry_ptr = NULL;
    s_spi_rt.header_pending = false;
    s_spi_rt.telemetry_pending = false;
    s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
    s_spi_rt.worker_busy = false;
    portEXIT_CRITICAL(&s_spi_lock);
    spi_ready_set(false);
}

static void spi_fill_status_frame(uint8_t *out, size_t out_len)
{
    exp_spi_status_t st = {
        .magic = EXP_SPI_MAGIC,
        .version = EXP_SPI_VERSION,
    };

    if (!out || out_len < EXP_SPI_STATUS_FRAME_SIZE) return;

    portENTER_CRITICAL(&s_spi_lock);
    st.state = s_spi_rt.state;
    st.error = s_spi_rt.error;
    st.records_expected = s_spi_rt.records_expected;
    st.current_order = s_spi_rt.current_order;
    st.run_id = s_spi_rt.run_id;
    st.payload_len = s_spi_rt.payload_len;
    portEXIT_CRITICAL(&s_spi_lock);

    memset(out, 0, out_len);
    exp_spi_status_encode(&st, out);
}

static bool ensure_spi_buffers(void)
{
    const size_t spi_chunk_dma_bytes =
        EXP_SPI_DMA_ALIGNED_SIZE(EXP_SPI_PAYLOAD_CHUNK + EXP_SPI_GUARD_BYTES);

    if (!s_spi_status_tx_buf) {
        s_spi_status_tx_buf = (uint8_t *)heap_caps_malloc(SPI_STATUS_DMA_BYTES, SPI_DMA_ALLOC_CAPS);
        if (!s_spi_status_tx_buf) {
            ESP_LOGE(TAG, "Malloc SPI status TX falhou");
            return false;
        }
        memset(s_spi_status_tx_buf, 0, SPI_STATUS_DMA_BYTES);
    }
    if (!s_spi_ctrl_rx_buf) {
        s_spi_ctrl_rx_buf = (uint8_t *)heap_caps_malloc(SPI_CTRL_DMA_BYTES, SPI_DMA_ALLOC_CAPS);
        if (!s_spi_ctrl_rx_buf) {
            ESP_LOGE(TAG, "Malloc SPI ctrl RX falhou");
            return false;
        }
        memset(s_spi_ctrl_rx_buf, 0, SPI_CTRL_DMA_BYTES);
    }
    if (!s_spi_dma_chunk_buf) {
        s_spi_dma_chunk_buf = (uint8_t *)heap_caps_malloc(spi_chunk_dma_bytes,
                                                          SPI_DMA_ALLOC_CAPS);
        if (!s_spi_dma_chunk_buf) {
            ESP_LOGE(TAG, "Malloc SPI chunk DMA falhou");
            return false;
        }
        memset(s_spi_dma_chunk_buf, 0, spi_chunk_dma_bytes);
    }
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Diagnóstico visual - pisca LED flash 3×.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void flash_blink(int n)
{
    if (FLASH_PIN < 0) {
        vTaskDelay(pdMS_TO_TICKS(120 * n));
        return;
    }

    gpio_set_direction(FLASH_PIN, GPIO_MODE_OUTPUT);
    for (int i = 0; i < n; i++) {
        gpio_set_level(FLASH_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(FLASH_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    /* Deixa como entrada para que o S3 possa controlar quando quiser */
    gpio_set_direction(FLASH_PIN, GPIO_MODE_INPUT);
}

static void fatal_halt(const char *reason)
{
    int log_count = 0;

    if (reason && reason[0] != '\0') {
        ESP_LOGE(TAG, "%s", reason);
    }

    while (1) {
        flash_blink(2);
        if (reason && reason[0] != '\0' && (log_count < 3 || (log_count % 10) == 0)) {
            ESP_LOGE(TAG, "falha fatal persistente: %s", reason);
        }
        log_count++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void log_psram_state(const char *stage)
{
    size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "%s: PSRAM livre=%u maior_bloco=%u",
             stage, (unsigned)free_psram, (unsigned)largest_block);
}

static jpeg_dct_method_t method_id_to_dct(uint8_t method_id)
{
    return exp_method_to_jpeg_dct(method_id);
}

static uint8_t method_id_to_seq(uint8_t method_id)
{
    switch (method_id) {
        case EXP_METHOD_RAW:      return EXP_SEQ_RAW;
        case EXP_METHOD_MATRIX:   return EXP_SEQ_MATRIX;
        case EXP_METHOD_RDCT:        return EXP_SEQ_RDCT;
        case EXP_METHOD_SILVEIRA_J3: return EXP_SEQ_SILVEIRA_J3;
        case EXP_METHOD_SILVEIRA_J7: return EXP_SEQ_SILVEIRA_J7;
        case EXP_METHOD_LOEFFLER:
        default:                  return EXP_SEQ_LOEFFLER;
    }
}

typedef enum {
    SPI_TX_STATUS = 0,
    SPI_TX_HEADER = 1,
    SPI_TX_PAYLOAD = 2,
    SPI_TX_TELEMETRY = 3,
} spi_tx_kind_t;

static const char *spi_tx_kind_name(spi_tx_kind_t kind)
{
    switch (kind) {
        case SPI_TX_HEADER:  return "header";
        case SPI_TX_PAYLOAD: return "payload";
        case SPI_TX_TELEMETRY: return "telemetry";
        case SPI_TX_STATUS:
        default:             return "status";
    }
}

static bool spi_decode_short_read_command(const uint8_t *buf, size_t len,
                                          exp_spi_ctrl_t *ctrl)
{
    uint8_t cmd;

    if (!buf || !ctrl || len < 4u) return false;
    if (exp_get_u16le(buf + 0) != EXP_SPI_MAGIC) return false;
    if (buf[2] != EXP_SPI_VERSION) return false;

    cmd = buf[3];
    if (cmd != EXP_SPI_CMD_READ_HEADER &&
            cmd != EXP_SPI_CMD_READ_PAYLOAD &&
            cmd != EXP_SPI_CMD_READ_TELEMETRY) {
        return false;
    }

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->magic = EXP_SPI_MAGIC;
    ctrl->version = EXP_SPI_VERSION;
    ctrl->cmd = cmd;
    return true;
}

static void spi_apply_control_config(const exp_spi_ctrl_t *ctrl)
{
    uint8_t method_id;
    uint8_t k_idx;
    bool all_mode;

    if (!ctrl) return;

    method_id = ctrl->method_id;
    k_idx = ctrl->k_idx;
    if (k_idx >= EXP_K_FACTOR_COUNT) k_idx = 0u;
    if (!exp_method_is_jpeg(method_id) && method_id != EXP_METHOD_ALL) {
        method_id = EXP_METHOD_LOEFFLER;
    }
    all_mode = (method_id == EXP_METHOD_ALL);

    s_method = method_id;
    s_all_mode = all_mode;
    s_k_idx = k_idx;
    s_quality = exp_k_factor_from_idx(s_k_idx);
    s_raw_first = (ctrl->flags & EXP_SPI_FLAG_RAW_FIRST) != 0u;
    s_run_id = ctrl->run_id;

    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.records_expected = exp_records_expected_for_method(s_all_mode ? EXP_METHOD_ALL : s_method);
    s_spi_rt.run_id = s_run_id;
    portEXIT_CRITICAL(&s_spi_lock);

    ESP_LOGI(TAG, "SPI config: run=%lu method=%s k=%s raw_first=%d",
             (unsigned long)s_run_id,
             exp_method_name(s_method),
             exp_k_label_from_idx(s_k_idx),
             (int)s_raw_first);
}

static bool spi_prepare_transaction(spi_slave_transaction_t *trans, spi_tx_kind_t *kind_out,
                                    size_t *expected_data_len_out)
{
    spi_tx_kind_t kind = SPI_TX_STATUS;
    size_t tx_len = EXP_SPI_STATUS_FRAME_SIZE;
    const uint8_t *payload_ptr = NULL;
    uint32_t header_offset = 0u;
    uint32_t payload_offset = 0u;
    uint32_t payload_len = 0u;
    const uint8_t *telemetry_ptr = NULL;
    uint32_t telemetry_offset = 0u;
    uint32_t telemetry_len = 0u;
    bool telemetry_pending = false;
    bool header_pending = false;
    uint8_t tx_armed_kind = SPI_ARM_NONE;

    if (!trans || !kind_out || !expected_data_len_out) return false;

    memset(trans, 0, sizeof(*trans));

    portENTER_CRITICAL(&s_spi_lock);
    header_pending = s_spi_rt.header_pending;
    payload_ptr = s_spi_rt.payload_ptr;
    header_offset = s_spi_rt.header_offset;
    payload_offset = s_spi_rt.payload_offset;
    payload_len = s_spi_rt.payload_len;
    telemetry_ptr = s_spi_rt.telemetry_ptr;
    telemetry_offset = s_spi_rt.telemetry_offset;
    telemetry_len = s_spi_rt.telemetry_len;
    telemetry_pending = s_spi_rt.telemetry_pending;
    tx_armed_kind = s_spi_rt.tx_armed_kind;
    if (s_spi_rt.state == EXP_SPI_STATE_RECORD_READY) {
        if (tx_armed_kind == SPI_ARM_HEADER &&
                header_pending &&
                header_offset < EXP_RECORD_HEADER_SIZE) {
            uint32_t remaining = (uint32_t)EXP_RECORD_HEADER_SIZE - header_offset;
            kind = SPI_TX_HEADER;
            tx_len = (size_t)remaining;
        } else if (tx_armed_kind == SPI_ARM_PAYLOAD &&
                   payload_ptr && payload_offset < payload_len) {
            uint32_t remaining = payload_len - payload_offset;
            uint32_t chunk_limit = (payload_offset == 0u) ? SPI_PAYLOAD_FIRST_CHUNK : EXP_SPI_PAYLOAD_CHUNK;
            kind = SPI_TX_PAYLOAD;
            tx_len = (remaining > chunk_limit) ? (size_t)chunk_limit : (size_t)remaining;
        } else if (tx_armed_kind == SPI_ARM_TELEMETRY &&
                   telemetry_pending &&
                   telemetry_ptr && telemetry_offset < telemetry_len) {
            uint32_t remaining = telemetry_len - telemetry_offset;
            kind = SPI_TX_TELEMETRY;
            tx_len = (size_t)remaining;
        }
    }
    portEXIT_CRITICAL(&s_spi_lock);

    switch (kind) {
        case SPI_TX_HEADER:
            memcpy(s_spi_dma_chunk_buf, s_record_hdr_buf + header_offset, tx_len);
            memset(s_spi_dma_chunk_buf + tx_len, 0, EXP_SPI_GUARD_BYTES);
            trans->tx_buffer = s_spi_dma_chunk_buf;
            trans->rx_buffer = NULL;
            trans->length = (tx_len + EXP_SPI_GUARD_BYTES) * 8u;
            break;
        case SPI_TX_PAYLOAD:
            memcpy(s_spi_dma_chunk_buf, payload_ptr + payload_offset, tx_len);
            memset(s_spi_dma_chunk_buf + tx_len, 0, EXP_SPI_GUARD_BYTES);
            trans->tx_buffer = s_spi_dma_chunk_buf;
            trans->rx_buffer = NULL;
            trans->length = (tx_len + EXP_SPI_GUARD_BYTES) * 8u;
            break;
        case SPI_TX_TELEMETRY:
            memcpy(s_spi_dma_chunk_buf, telemetry_ptr + telemetry_offset, tx_len);
            memset(s_spi_dma_chunk_buf + tx_len, 0, EXP_SPI_GUARD_BYTES);
            trans->tx_buffer = s_spi_dma_chunk_buf;
            trans->rx_buffer = NULL;
            trans->length = (tx_len + EXP_SPI_GUARD_BYTES) * 8u;
            break;
        case SPI_TX_STATUS:
        default:
            spi_fill_status_frame(s_spi_status_tx_buf, SPI_STATUS_DMA_BYTES);
            memset(s_spi_ctrl_rx_buf, 0, SPI_CTRL_DMA_BYTES);
            memset(s_spi_dma_chunk_buf, 0, EXP_SPI_PAYLOAD_CHUNK + EXP_SPI_GUARD_BYTES); // Dummy data handling explicitly zeroes chunk buf
            trans->tx_buffer = s_spi_status_tx_buf;
            trans->rx_buffer = s_spi_ctrl_rx_buf;
            trans->length = EXP_SPI_STATUS_WIRE_SIZE * 8u;
            tx_len = EXP_SPI_STATUS_WIRE_SIZE;
            break;
    }

    *kind_out = kind;
    *expected_data_len_out = tx_len;
    return true;
}

static void spi_handle_ctrl_command(const exp_spi_ctrl_t *ctrl)
{
    uint8_t state;
    bool worker_busy;

    if (!ctrl) return;

    portENTER_CRITICAL(&s_spi_lock);
    state = s_spi_rt.state;
    worker_busy = s_spi_rt.worker_busy;
    portEXIT_CRITICAL(&s_spi_lock);

    switch (ctrl->cmd) {
        case EXP_SPI_CMD_NOP:
            return;

        case EXP_SPI_CMD_SET_CONFIG:
            if (worker_busy || state == EXP_SPI_STATE_CAPTURING || state == EXP_SPI_STATE_RECORD_READY) {
                spi_set_state(state, EXP_SPI_ERROR_BUSY);
                return;
            }
            spi_apply_control_config(ctrl);
            spi_set_state(EXP_SPI_STATE_IDLE, EXP_SPI_ERROR_NONE);
            return;

        case EXP_SPI_CMD_TRIGGER:
            if (worker_busy || (state != EXP_SPI_STATE_IDLE && state != EXP_SPI_STATE_DONE)) {
                spi_set_state(state, EXP_SPI_ERROR_BUSY);
                return;
            }

            /* O TRIGGER e autossuficiente: a captura usa method/k/run vindos
             * do proprio comando, sem depender de SET_CONFIG previo. */
            spi_apply_control_config(ctrl);

            if (!exp_method_is_jpeg(s_method) && s_method != EXP_METHOD_ALL) {
                spi_mark_error(EXP_SPI_ERROR_BAD_CONFIG);
                return;
            }
            spi_clear_abort();
            portENTER_CRITICAL(&s_spi_lock);
            s_spi_rt.worker_busy = true;
            s_spi_rt.state = EXP_SPI_STATE_CAPTURING;
            s_spi_rt.error = EXP_SPI_ERROR_NONE;
            s_spi_rt.records_expected = exp_records_expected_for_method(s_all_mode ? EXP_METHOD_ALL : s_method);
            s_spi_rt.current_order = 0u;
            s_spi_rt.run_id = s_run_id;
            s_spi_rt.payload_len = 0u;
            s_spi_rt.header_offset = 0u;
            s_spi_rt.payload_offset = 0u;
            s_spi_rt.payload_ptr = NULL;
            s_spi_rt.telemetry_len = 0u;
            s_spi_rt.telemetry_offset = 0u;
            s_spi_rt.telemetry_ptr = NULL;
            s_spi_rt.header_pending = false;
            s_spi_rt.telemetry_pending = false;
            s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
            portEXIT_CRITICAL(&s_spi_lock);
            if (s_main_task) {
                xTaskNotifyGive(s_main_task);
            }
            ESP_LOGI(TAG, "SPI trigger: run=%lu method=%s k=%s",
                     (unsigned long)s_run_id,
                     exp_method_name(s_method),
                     exp_k_label_from_idx(s_k_idx));
            return;

        case EXP_SPI_CMD_READ_HEADER:
        {
            bool drop_ready = false;
            portENTER_CRITICAL(&s_spi_lock);
            if (s_spi_rt.state == EXP_SPI_STATE_RECORD_READY) {
                s_spi_rt.header_pending = true;
                s_spi_rt.header_offset = 0u;
                s_spi_rt.tx_armed_kind = SPI_ARM_HEADER;
                s_spi_rt.error = EXP_SPI_ERROR_NONE;
                drop_ready = true;
            } else {
                s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
            }
            portEXIT_CRITICAL(&s_spi_lock);
            if (drop_ready) spi_ready_set(false);
            return;
        }

        case EXP_SPI_CMD_READ_PAYLOAD:
            portENTER_CRITICAL(&s_spi_lock);
            if (s_spi_rt.state == EXP_SPI_STATE_RECORD_READY &&
                    !s_spi_rt.header_pending &&
                    s_spi_rt.payload_ptr &&
                    s_spi_rt.payload_offset < s_spi_rt.payload_len) {
                s_spi_rt.tx_armed_kind = SPI_ARM_PAYLOAD;
                s_spi_rt.error = EXP_SPI_ERROR_NONE;
            } else {
                s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
                if (s_spi_rt.state == EXP_SPI_STATE_RECORD_READY) {
                    /* Se o comando chegar cedo/torto, deixa o master tentar
                     * novamente sem contaminar o status com PROTO. */
                    s_spi_rt.error = EXP_SPI_ERROR_NONE;
                }
            }
            portEXIT_CRITICAL(&s_spi_lock);
            return;

        case EXP_SPI_CMD_READ_TELEMETRY:
            portENTER_CRITICAL(&s_spi_lock);
            if (s_spi_rt.state == EXP_SPI_STATE_RECORD_READY &&
                    !s_spi_rt.header_pending &&
                    s_spi_rt.payload_offset >= s_spi_rt.payload_len &&
                    s_spi_rt.telemetry_pending &&
                    s_spi_rt.telemetry_ptr &&
                    s_spi_rt.telemetry_offset < s_spi_rt.telemetry_len) {
                s_spi_rt.tx_armed_kind = SPI_ARM_TELEMETRY;
                s_spi_rt.error = EXP_SPI_ERROR_NONE;
            } else {
                s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
                if (s_spi_rt.state == EXP_SPI_STATE_RECORD_READY) {
                    s_spi_rt.error = EXP_SPI_ERROR_NONE;
                }
            }
            portEXIT_CRITICAL(&s_spi_lock);
            return;

        case EXP_SPI_CMD_ABORT:
            portENTER_CRITICAL(&s_spi_lock);
            s_spi_rt.abort_requested = true;
            s_spi_rt.state = EXP_SPI_STATE_IDLE;
            s_spi_rt.error = EXP_SPI_ERROR_NONE;
            s_spi_rt.payload_len = 0u;
            s_spi_rt.header_offset = 0u;
            s_spi_rt.payload_offset = 0u;
            s_spi_rt.payload_ptr = NULL;
            s_spi_rt.telemetry_len = 0u;
            s_spi_rt.telemetry_offset = 0u;
            s_spi_rt.telemetry_ptr = NULL;
            s_spi_rt.header_pending = false;
            s_spi_rt.telemetry_pending = false;
            s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
            s_spi_rt.worker_busy = false;
            portEXIT_CRITICAL(&s_spi_lock);
            spi_ready_set(false);
            if (s_main_task) {
                xTaskNotifyGive(s_main_task);
            }
            ESP_LOGW(TAG, "SPI abort solicitado pelo master");
            return;

        default:
            spi_mark_error(EXP_SPI_ERROR_PROTO);
            return;
    }
}

static void spi_handle_completed_transaction(spi_tx_kind_t kind, size_t actual_len,
                                             size_t expected_data_len)
{
    bool notify_worker = false;
    bool clear_ready = false;
    size_t payload_len = actual_len;
    size_t expected_wire_len = expected_data_len;
    bool log_payload_progress = false;
    uint32_t payload_progress = 0u;
    uint32_t payload_total = 0u;
    bool log_per_txn = false;
    uint32_t per_txn_offset_before = 0u;
    uint32_t per_txn_bytes = 0u;
    uint32_t per_txn_total = 0u;

    if (kind != SPI_TX_STATUS) {
        expected_wire_len = expected_data_len + EXP_SPI_GUARD_BYTES;
        if (actual_len < expected_wire_len) {
            uint8_t state;
            uint8_t arm;

            if (kind == SPI_TX_HEADER &&
                    (actual_len == EXP_SPI_STATUS_WIRE_SIZE ||
                     actual_len == EXP_SPI_STATUS_FRAME_SIZE)) {
                portENTER_CRITICAL(&s_spi_lock);
                state = s_spi_rt.state;
                s_spi_rt.error = EXP_SPI_ERROR_NONE;
                s_spi_rt.header_offset = 0u;
                s_spi_rt.header_pending = true;
                s_spi_rt.tx_armed_kind = SPI_ARM_HEADER;
                arm = s_spi_rt.tx_armed_kind;
                portEXIT_CRITICAL(&s_spi_lock);
                spi_ready_set(true);

                ESP_LOGW(TAG,
                         "SPI header recebeu transacao STATUS actual=%u expected=%u state=%u arm=%u; mantendo auto-arm",
                         (unsigned)actual_len,
                         (unsigned)expected_wire_len,
                         (unsigned)state,
                         (unsigned)arm);
                return;
            }

            portENTER_CRITICAL(&s_spi_lock);
            state = s_spi_rt.state;
            s_spi_rt.abort_requested = true;
            s_spi_rt.state = EXP_SPI_STATE_ERROR;
            s_spi_rt.error = EXP_SPI_ERROR_PROTO;
            s_spi_rt.payload_len = 0u;
            s_spi_rt.header_offset = 0u;
            s_spi_rt.payload_offset = 0u;
            s_spi_rt.payload_ptr = NULL;
            s_spi_rt.telemetry_len = 0u;
            s_spi_rt.telemetry_offset = 0u;
            s_spi_rt.telemetry_ptr = NULL;
            s_spi_rt.header_pending = false;
            s_spi_rt.telemetry_pending = false;
            s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
            s_spi_rt.worker_busy = false;
            arm = s_spi_rt.tx_armed_kind;
            portEXIT_CRITICAL(&s_spi_lock);
            spi_ready_set(false);
            if (s_main_task) {
                xTaskNotifyGive(s_main_task);
            }

            ESP_LOGW(TAG,
                     "SPI trans curta kind=%s actual=%u expected=%u state=%u arm=%u; abortando registro",
                     spi_tx_kind_name(kind),
                     (unsigned)actual_len,
                     (unsigned)expected_wire_len,
                     (unsigned)state,
                     (unsigned)arm);
            return;
        }
        if (payload_len > expected_wire_len) {
            payload_len = expected_wire_len;
        }
    }

    s_spi_trans_count++;
    if (s_spi_trans_count <= 8u || (s_spi_trans_count % 128u) == 0u) {
        uint8_t state;
        uint8_t arm;

        portENTER_CRITICAL(&s_spi_lock);
        state = s_spi_rt.state;
        arm = s_spi_rt.tx_armed_kind;
        portEXIT_CRITICAL(&s_spi_lock);
        ESP_LOGI(TAG,
                 "SPI trans ok count=%lu kind=%s actual=%u expected=%u state=%u arm=%u",
                 (unsigned long)s_spi_trans_count,
                 spi_tx_kind_name(kind),
                 (unsigned)actual_len,
                 (unsigned)((kind == SPI_TX_STATUS) ? expected_data_len : expected_wire_len),
                 (unsigned)state,
                 (unsigned)arm);
    }

    if (kind != SPI_TX_STATUS && payload_len >= EXP_SPI_GUARD_BYTES) {
        payload_len -= EXP_SPI_GUARD_BYTES;
    }

    switch (kind) {
        case SPI_TX_STATUS:
            if (actual_len >= EXP_SPI_CTRL_FRAME_SIZE) {
                exp_spi_ctrl_t ctrl;

                if (exp_spi_ctrl_decode(s_spi_ctrl_rx_buf, &ctrl)) {
                    s_invalid_done_ctrl_count = 0u;
                    if (ctrl.cmd != EXP_SPI_CMD_NOP) {
                        uint8_t state;
                        portENTER_CRITICAL(&s_spi_lock);
                        state = s_spi_rt.state;
                        portEXIT_CRITICAL(&s_spi_lock);
                        ESP_LOGI(TAG,
                                 "SPI ctrl RX: cmd=%u method=%s k=%s run=%lu flags=0x%02X state=%u",
                                 (unsigned)ctrl.cmd,
                                 exp_method_name(ctrl.method_id),
                                 exp_k_label_from_idx(ctrl.k_idx),
                                 (unsigned long)ctrl.run_id,
                                 (unsigned)ctrl.flags,
                                 (unsigned)state);
                    }
                    spi_handle_ctrl_command(&ctrl);
                } else {
                    exp_spi_ctrl_t short_ctrl;
                    uint8_t state;
                    portENTER_CRITICAL(&s_spi_lock);
                    state = s_spi_rt.state;
                    portEXIT_CRITICAL(&s_spi_lock);

                    if (spi_decode_short_read_command(s_spi_ctrl_rx_buf,
                                                      actual_len,
                                                      &short_ctrl)) {
                        ESP_LOGW(TAG,
                                 "SPI ctrl RX curto: cmd=%u actual=%u state=%u; aceitando read idempotente",
                                 (unsigned)short_ctrl.cmd,
                                 (unsigned)actual_len,
                                 (unsigned)state);
                        spi_handle_ctrl_command(&short_ctrl);
                    } else if (state == EXP_SPI_STATE_DONE) {
                        s_invalid_done_ctrl_count++;
                        if (s_invalid_done_ctrl_count <= 3u ||
                                (s_invalid_done_ctrl_count % 50u) == 0u) {
                            ESP_LOGW(TAG,
                                     "SPI ctrl invalido em DONE ignorado count=%lu rx=%02X %02X %02X %02X",
                                     (unsigned long)s_invalid_done_ctrl_count,
                                     s_spi_ctrl_rx_buf[0],
                                     s_spi_ctrl_rx_buf[1],
                                     s_spi_ctrl_rx_buf[2],
                                     s_spi_ctrl_rx_buf[3]);
                        }
                    }
                }
            } else {
                ESP_LOGW(TAG, "SPI status trans curta: actual_len=%u",
                         (unsigned)actual_len);
            }
            break;

        case SPI_TX_HEADER:
            portENTER_CRITICAL(&s_spi_lock);
            if (payload_len > 0u) {
                s_spi_rt.header_offset += (uint32_t)payload_len;
                if (s_spi_rt.header_offset > EXP_RECORD_HEADER_SIZE) {
                    s_spi_rt.header_offset = EXP_RECORD_HEADER_SIZE;
                }
            }
            if (s_spi_rt.header_offset >= EXP_RECORD_HEADER_SIZE) {
                s_spi_rt.header_pending = false;
                s_spi_rt.header_offset = 0u;
                clear_ready = true;
                if (s_spi_rt.payload_ptr && s_spi_rt.payload_offset < s_spi_rt.payload_len) {
                    /* Assim que o header terminar, o proximo frame no fio ja
                     * pode ser payload. Isso evita depender de um segundo
                     * comando de arm vindo do master. */
                    s_spi_rt.tx_armed_kind = SPI_ARM_PAYLOAD;
                } else {
                    s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
                }
            } else if (s_spi_rt.header_pending) {
                s_spi_rt.tx_armed_kind = SPI_ARM_HEADER;
            }
            if (!s_spi_rt.header_pending && s_spi_rt.payload_len == 0u) {
                s_spi_rt.state = EXP_SPI_STATE_CAPTURING;
                notify_worker = true;
            }
            portEXIT_CRITICAL(&s_spi_lock);
            break;

        case SPI_TX_PAYLOAD:
            portENTER_CRITICAL(&s_spi_lock);
            per_txn_offset_before = s_spi_rt.payload_offset;
            per_txn_total = s_spi_rt.payload_len;
            if (payload_len > 0u) {
                s_spi_rt.payload_offset += (uint32_t)payload_len;
                if (s_spi_rt.payload_offset > s_spi_rt.payload_len) {
                    s_spi_rt.payload_offset = s_spi_rt.payload_len;
                }
            }
            per_txn_bytes = (uint32_t)payload_len;
            log_per_txn = true;
            if (s_spi_rt.payload_len > 0u &&
                    (((s_spi_rt.payload_offset % 4096u) == 0u) ||
                     (s_spi_rt.payload_offset >= s_spi_rt.payload_len))) {
                log_payload_progress = true;
                payload_progress = s_spi_rt.payload_offset;
                payload_total = s_spi_rt.payload_len;
            }
            if (s_spi_rt.payload_offset >= s_spi_rt.payload_len) {
                s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
                s_spi_rt.payload_ptr = NULL;
                if (s_spi_rt.telemetry_pending &&
                        s_spi_rt.telemetry_ptr &&
                        s_spi_rt.telemetry_len > 0u) {
                    s_spi_rt.tx_armed_kind = SPI_ARM_TELEMETRY;
                    s_spi_rt.state = EXP_SPI_STATE_RECORD_READY;
                } else {
                    s_spi_rt.payload_len = 0u;
                    s_spi_rt.payload_offset = 0u;
                    s_spi_rt.state = EXP_SPI_STATE_CAPTURING;
                    notify_worker = true;
                }
            }
            portEXIT_CRITICAL(&s_spi_lock);
            break;

        case SPI_TX_TELEMETRY:
            portENTER_CRITICAL(&s_spi_lock);
            if (payload_len > 0u) {
                s_spi_rt.telemetry_offset += (uint32_t)payload_len;
                if (s_spi_rt.telemetry_offset > s_spi_rt.telemetry_len) {
                    s_spi_rt.telemetry_offset = s_spi_rt.telemetry_len;
                }
            }
            if (s_spi_rt.telemetry_offset >= s_spi_rt.telemetry_len) {
                s_spi_rt.tx_armed_kind = SPI_ARM_NONE;
                s_spi_rt.payload_len = 0u;
                s_spi_rt.payload_offset = 0u;
                s_spi_rt.payload_ptr = NULL;
                s_spi_rt.telemetry_len = 0u;
                s_spi_rt.telemetry_offset = 0u;
                s_spi_rt.telemetry_ptr = NULL;
                s_spi_rt.telemetry_pending = false;
                s_spi_rt.state = EXP_SPI_STATE_CAPTURING;
                notify_worker = true;
            } else if (s_spi_rt.telemetry_pending) {
                s_spi_rt.tx_armed_kind = SPI_ARM_TELEMETRY;
            }
            portEXIT_CRITICAL(&s_spi_lock);
            break;
    }

    if (log_per_txn) {
        uint32_t per_txn_next = per_txn_offset_before + per_txn_bytes;
        bool log_txn_info;

        if (per_txn_next < per_txn_offset_before || per_txn_next > per_txn_total) {
            per_txn_next = per_txn_total;
        }
        log_txn_info =
            per_txn_offset_before == 0u ||
            per_txn_next >= per_txn_total ||
            ((per_txn_offset_before / SPI_RECORD_LOG_STEP_BYTES) !=
             (per_txn_next / SPI_RECORD_LOG_STEP_BYTES));

        if (log_txn_info) {
            ESP_LOGI(TAG, "record tx: chunk offset=%lu bytes=%lu total=%lu",
                     (unsigned long)per_txn_offset_before,
                     (unsigned long)per_txn_bytes,
                     (unsigned long)per_txn_total);
        } else {
            ESP_LOGD(TAG, "record tx: chunk offset=%lu bytes=%lu total=%lu",
                     (unsigned long)per_txn_offset_before,
                     (unsigned long)per_txn_bytes,
                     (unsigned long)per_txn_total);
        }
    }
    if (log_payload_progress) {
        ESP_LOGI(TAG, "record tx: payload SPI %lu/%lu",
                 (unsigned long)payload_progress,
                 (unsigned long)payload_total);
    }
    if (clear_ready) {
        spi_ready_set(false);
    }
    if (notify_worker && s_main_task) {
        xTaskNotifyGive(s_main_task);
    }
}

static void spi_service_task(void *arg)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXP_SPI_DMA_ALIGNED_SIZE(EXP_SPI_PAYLOAD_CHUNK + EXP_SPI_GUARD_BYTES),
    };
    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = SPI_CS_PIN,
        .flags = 0,
        .queue_size = 1,
        .mode = SPI_LINK_MODE,
        .post_setup_cb = NULL,
        .post_trans_cb = NULL,
    };

    (void)arg;

    if (!ensure_spi_buffers()) {
        spi_mark_error(EXP_SPI_ERROR_PROTO);
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(spi_slave_initialize(SPI_LINK_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI slave pronto host=%d mode=%u freq=%d pins: MISO=%d MOSI=%d SCLK=%d CS=%d READY=%d",
             SPI_LINK_HOST, (unsigned)SPI_LINK_MODE, SPI_LINK_FREQ_HZ,
             SPI_MISO_PIN, SPI_MOSI_PIN, SPI_SCLK_PIN, SPI_CS_PIN, SPI_READY_PIN);
    spi_mark_idle();

    while (1) {
        spi_slave_transaction_t trans;
        spi_tx_kind_t kind = SPI_TX_STATUS;
        size_t expected_data_len = EXP_SPI_STATUS_WIRE_SIZE;
        esp_err_t err;
        size_t actual_len;

        if (!spi_prepare_transaction(&trans, &kind, &expected_data_len)) {
            spi_mark_error(EXP_SPI_ERROR_PROTO);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        err = spi_slave_transmit(SPI_LINK_HOST, &trans, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_slave_transmit falhou: %s", esp_err_to_name(err));
            spi_mark_error(EXP_SPI_ERROR_PROTO);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        actual_len = (trans.trans_len + 7u) / 8u;
        spi_handle_completed_transaction(kind, actual_len, expected_data_len);
    }
}

static bool send_record_payload(const exp_record_header_t *hdr, const uint8_t *payload)
{
    if (!hdr || !payload) return false;

    ESP_LOGI(TAG, "transport: outer-link=SPI freq=%u chunk=%u stack_hwm=%u B",
             (unsigned)EXP_SPI_FREQ_HZ,
             (unsigned)EXP_SPI_PAYLOAD_CHUNK,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    exp_record_header_encode(hdr, s_record_hdr_buf);
    exp_record_telemetry_encode(hdr, s_record_telemetry_buf);
    ESP_LOGI(TAG, "record tx: image_header=%u telemetry=%u hdr[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X payload=%lu",
             (unsigned)EXP_RECORD_HEADER_SIZE,
             (unsigned)EXP_RECORD_TELEMETRY_SIZE,
             s_record_hdr_buf[0], s_record_hdr_buf[1], s_record_hdr_buf[2], s_record_hdr_buf[3],
             s_record_hdr_buf[4], s_record_hdr_buf[5], s_record_hdr_buf[6], s_record_hdr_buf[7],
             (unsigned long)hdr->payload_len);
    portENTER_CRITICAL(&s_spi_lock);
    s_spi_rt.state = EXP_SPI_STATE_RECORD_READY;
    s_spi_rt.error = EXP_SPI_ERROR_NONE;
    s_spi_rt.records_expected = hdr->records_expected;
    s_spi_rt.current_order = hdr->order_index;
    s_spi_rt.run_id = hdr->run_id;
    s_spi_rt.payload_len = hdr->payload_len;
    s_spi_rt.header_offset = 0u;
    s_spi_rt.payload_offset = 0u;
    s_spi_rt.payload_ptr = payload;
    s_spi_rt.telemetry_len = EXP_RECORD_TELEMETRY_SIZE;
    s_spi_rt.telemetry_offset = 0u;
    s_spi_rt.telemetry_ptr = s_record_telemetry_buf;
    s_spi_rt.header_pending = true;
    s_spi_rt.telemetry_pending = true;
    s_spi_rt.tx_armed_kind = SPI_ARM_HEADER;
    portEXIT_CRITICAL(&s_spi_lock);
    spi_ready_set(true);

    {
        int64_t deadline_us = esp_timer_get_time() + ((int64_t)SPI_RECORD_TIMEOUT_MS * 1000LL);

        while (1) {
            if (spi_is_abort_requested()) {
                ESP_LOGW(TAG, "record tx: abortado pelo master");
                return false;
            }
            if (esp_timer_get_time() >= deadline_us) {
                ESP_LOGE(TAG, "record tx: timeout terminal (%d ms) - master sumiu?",
                         SPI_RECORD_TIMEOUT_MS);
                spi_mark_error(EXP_SPI_ERROR_PROTO);
                return false;
            }
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SPI_RECORD_WAIT_MS)) > 0) {
                break;
            }
        }
    }

    if (spi_is_abort_requested()) {
        ESP_LOGW(TAG, "record tx: abort detectado apos envio");
        return false;
    }
    spi_ready_set(false);
    return true;
}

static bool ensure_work_buffers(void)
{
    if (!s_rgb888_buf) {
        s_rgb888_buf = (uint8_t *)heap_caps_malloc(RGB888_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_rgb888_buf) {
            ESP_LOGE(TAG, "Malloc RGB888 persistente falhou (%u bytes)",
                     (unsigned)RGB888_BUF_SIZE);
            return false;
        }
    }

    if (!s_frame_buf) {
        /* Use worst-case RLE size (688 KB for QVGA 4:4:4), not the generic
         * JPEG_FRAME_MAX_PAYLOAD_BYTES (512 KB), to avoid encode failures on
         * pathological noisy images where many AC coefficients survive k=0.1. */
        s_frame_buf = (uint8_t *)heap_caps_malloc(EXP_JPEG_RLE_WORST_CASE_BYTES,
                                                  MALLOC_CAP_SPIRAM);
        if (!s_frame_buf) {
            ESP_LOGE(TAG, "Malloc frame_buf persistente falhou (%u bytes)",
                     (unsigned)EXP_JPEG_RLE_WORST_CASE_BYTES);
            return false;
        }
    }

    if (!s_recon_buf) {
        s_recon_buf = (uint8_t *)heap_caps_malloc(RGB888_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_recon_buf) {
            ESP_LOGE(TAG, "Malloc recon_buf persistente falhou (%u bytes)",
                     (unsigned)RGB888_BUF_SIZE);
            return false;
        }
    }

    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Inicialização da câmera
 * ═══════════════════════════════════════════════════════════════════════════ */

static void camera_apply_sensor_settings(void)
{
    sensor_t *sensor = esp_camera_sensor_get();

    if (!sensor) {
        ESP_LOGW(TAG, "Camera sensor indisponivel para ajustes AWB/AEC/AGC");
        return;
    }

    sensor->set_whitebal(sensor, 1);
    sensor->set_awb_gain(sensor, 1);
    sensor->set_wb_mode(sensor, 0);
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_aec2(sensor, 1);
    sensor->set_gain_ctrl(sensor, 1);
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 0);
    sensor->set_saturation(sensor, 0);
    sensor->set_special_effect(sensor, 0);
    sensor->set_colorbar(sensor, 0);
    sensor->set_vflip(sensor, CAMERA_SENSOR_VFLIP);
    sensor->set_hmirror(sensor, CAMERA_SENSOR_HMIRROR);

    ESP_LOGI(TAG,
             "Camera sensor: AWB/AEC/AGC auto, vflip=%d hmirror=%d",
             CAMERA_SENSOR_VFLIP,
             CAMERA_SENSOR_HMIRROR);
}

static esp_err_t camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7 = CAM_PIN_D7, .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5, .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3, .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1, .pin_d0 = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,
        .xclk_freq_hz = XCLK_FREQ_HZ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size   = FRAMESIZE_QVGA,
        .fb_count     = 1,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location  = CAMERA_FB_IN_PSRAM,
    };
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    camera_apply_sensor_settings();

    return ESP_OK;
}

static void camera_warmup_discard_frames(unsigned count)
{
    ESP_LOGI(TAG, "camera warmup: descartando %u frame(s)", (unsigned)count);
    for (unsigned i = 0; i < count; i++) {
        camera_fb_t *fb = esp_camera_fb_get();

        if (!fb) {
            ESP_LOGW(TAG, "camera warmup: falha ao capturar frame %u",
                     (unsigned)(i + 1u));
            vTaskDelay(pdMS_TO_TICKS(CAMERA_WARMUP_GAP_MS));
            continue;
        }

        ESP_LOGI(TAG,
                 "camera warmup: frame %u descartado len=%u %dx%d",
                 (unsigned)(i + 1u),
                 (unsigned)fb->len,
                 (int)fb->width,
                 (int)fb->height);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(CAMERA_WARMUP_GAP_MS));
    }
    ESP_LOGI(TAG, "camera warmup: concluido");
}

static uint8_t camera_mean_brightness_rgb565be(const camera_fb_t *fb)
{
    const uint8_t *p  = fb->buf;
    size_t         px = fb->width * fb->height;
    uint32_t sum = 0, cnt = 0;
    for (size_t i = 0; i < px; i += 32u) {
        uint8_t b0 = p[2u * i], b1 = p[2u * i + 1u];
        uint32_t r  = (uint32_t)(b0 >> 3);
        uint32_t g  = (uint32_t)(((b0 & 0x07u) << 3) | (b1 >> 5));
        uint32_t bv = (uint32_t)(b1 & 0x1Fu);
        sum += (r * 8u + g * 4u + bv * 8u) / 3u;
        cnt++;
    }
    return (uint8_t)(cnt ? sum / cnt : 0u);
}

static uint8_t camera_wait_aec_stable(void)
{
    uint8_t prev_mean = 0, stable_count = 0;
    for (unsigned i = 0; i < AEC_WAIT_MAX_FRAMES; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_WARMUP_GAP_MS));
            continue;
        }
        uint8_t mean = camera_mean_brightness_rgb565be(fb);
        esp_camera_fb_return(fb);
        uint8_t delta = (mean >= prev_mean) ? (mean - prev_mean) : (prev_mean - mean);
        if (i > 0 && delta <= AEC_STABLE_DELTA) {
            if (++stable_count >= AEC_STABLE_FRAMES) {
                ESP_LOGI(TAG, "AEC estabilizado: %u frames brilho=%u", i + 1u, mean);
                return mean;
            }
        } else {
            stable_count = 0;
        }
        prev_mean = mean;
        vTaskDelay(pdMS_TO_TICKS(CAMERA_WARMUP_GAP_MS));
    }
    ESP_LOGW(TAG, "AEC: timeout apos %u frames brilho=%u", AEC_WAIT_MAX_FRAMES, prev_mean);
    return prev_mean;
}

static uint8_t rgb888_mean_brightness(const uint8_t *buf, size_t pixels)
{
    uint32_t sum = 0, cnt = 0;
    for (size_t i = 0; i < pixels; i += 32u) {
        sum += (306u * buf[3u * i] + 601u * buf[3u * i + 1u] + 117u * buf[3u * i + 2u]) >> 10u;
        cnt++;
    }
    return (uint8_t)(cnt ? sum / cnt : 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Inicialização de GPIO
 *
 *  Configura os pinos SPI como entrada antes de inicializar o driver
 *  SPI slave. CS com pull-up; SCLK e MOSI com pull-down para evitar
 *  flutuação durante o boot.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void gpio_init_cam(void)
{
    gpio_config_t cs = {
        .pin_bit_mask = (1ULL << SPI_CS_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config_t sclk_mosi = {
        .pin_bit_mask = (1ULL << SPI_SCLK_PIN) | (1ULL << SPI_MOSI_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config_t miso = {
        .pin_bit_mask = (1ULL << SPI_MISO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config_t ready = {
        .pin_bit_mask = (1ULL << SPI_READY_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cs));
    ESP_ERROR_CHECK(gpio_config(&sclk_mosi));
    ESP_ERROR_CHECK(gpio_config(&miso));
    ESP_ERROR_CHECK(gpio_config(&ready));
    spi_ready_set(false);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Self-test - captura frame e comprime sem UART
 *
 *  Uso manual/debug: confirma que a câmera e a libimage funcionam
 *  independente da UART. Nao deve rodar automaticamente em producao.
 * ═══════════════════════════════════════════════════════════════════════════ */

static __attribute__((unused)) void do_self_test(void)
{
    ESP_LOGI(TAG, "=== SELF-TEST manual ===");

    /* Captura frame */
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "SELF-TEST: captura falhou");
        return;
    }
    ESP_LOGI(TAG, "SELF-TEST: frame capturado %dx%d (%u bytes RGB565)",
             (int)fb->width, (int)fb->height, (unsigned)fb->len);

    if (fb->width != (size_t)IMG_WIDTH || fb->height != (size_t)IMG_HEIGHT) {
        ESP_LOGE(TAG, "SELF-TEST: resolucao inesperada %dx%d", (int)fb->width, (int)fb->height);
        esp_camera_fb_return(fb);
        return;
    }

    /* Converte RGB565 → RGB888 */
    if (!ensure_work_buffers()) {
        ESP_LOGE(TAG, "SELF-TEST: buffers persistentes indisponiveis");
        esp_camera_fb_return(fb);
        return;
    }
    exp_convert_rgb565be_to_rgb888(fb->buf, s_rgb888_buf, IMG_WIDTH, IMG_HEIGHT);
    esp_camera_fb_return(fb);

    /* Comprime com método e qualidade atuais */
    jpeg_image_t img = {
        .width      = IMG_WIDTH,
        .height     = IMG_HEIGHT,
        .colorspace = JPEG_COLORSPACE_RGB,
        .data       = s_rgb888_buf,
    };
    jpeg_params_t params = {
        .quality_factor = s_quality,
        .dct_method = method_id_to_dct(s_method),
        .subsampling = JPEG_SUBSAMP_444,
        .flags = 0,
    };

    jpeg_compressed_t *comp = NULL;
    int64_t t0   = esp_timer_get_time();
    jpeg_error_t err = jpeg_compress(&img, &params, &comp);
    int64_t t1   = esp_timer_get_time();

    if (err != JPEG_SUCCESS || !comp) {
        ESP_LOGE(TAG, "SELF-TEST: jpeg_compress falhou: %s",
                 jpeg_error_string(err));
        return;
    }

    uint32_t t_us = (uint32_t)(t1 - t0);
    /* Calcula tamanho do payload RLE codificado para estimar bpp. */
    int32_t  frame_sz = jpeg_frame_payload_encode(comp, s_frame_buf,
                                                  JPEG_FRAME_MAX_PAYLOAD_BYTES);

    if (frame_sz > 0) {
        ESP_LOGI(TAG, "SELF-TEST OK: method=%d k=%s T_COMP=%lu us rle_payload=%ld B",
                 (int)s_method, exp_k_label_from_idx(s_k_idx),
                 (unsigned long)t_us, (long)frame_sz);
    } else {
        ESP_LOGW(TAG, "SELF-TEST OK: method=%d k=%s T_COMP=%lu us, mas o frame nao coube no limite de %u bytes",
                 (int)s_method, exp_k_label_from_idx(s_k_idx),
                 (unsigned long)t_us, (unsigned)JPEG_FRAME_MAX_PAYLOAD_BYTES);
    }
    ESP_LOGI(TAG, "SELF-TEST: camera + libimage funcionando corretamente");
    ESP_LOGI(TAG, "SELF-TEST: se handshake falha, problema eh na UART (cabos/firmware S3)");

    jpeg_free_compressed(comp);
}

static bool capture_rgb888_once(uint32_t *t_capture_us, uint32_t *t_rgb_us)
{
    camera_fb_t *fb;
    int64_t t0;
    int64_t t1;
    int64_t t2;
    int64_t t3;

    if (!ensure_work_buffers()) {
        ESP_LOGE(TAG, "Buffers persistentes indisponiveis");
        spi_mark_error(EXP_SPI_ERROR_CAPTURE_FAIL);
        return false;
    }

    t0 = esp_timer_get_time();
    fb = esp_camera_fb_get();
    t1 = esp_timer_get_time();
    if (!fb) {
        ESP_LOGE(TAG, "Captura falhou");
        spi_mark_error(EXP_SPI_ERROR_CAPTURE_FAIL);
        return false;
    }
    if (!fb->buf || fb->len != RGB565_BUF_SIZE) {
        ESP_LOGE(TAG,
                 "capture: status=\"CAM_OVF\" evidencia de overflow DMA len=%u esperado=%u buf=%p",
                 (unsigned)fb->len,
                 (unsigned)RGB565_BUF_SIZE,
                 (void *)fb->buf);
        esp_camera_fb_return(fb);
        spi_mark_error(EXP_SPI_ERROR_CAM_OVF);
        return false;
    }
    if (fb->width != (size_t)IMG_WIDTH || fb->height != (size_t)IMG_HEIGHT) {
        ESP_LOGE(TAG, "Frame inesperado: %dx%d (esperado %dx%d)",
                 (int)fb->width, (int)fb->height, IMG_WIDTH, IMG_HEIGHT);
        esp_camera_fb_return(fb);
        spi_mark_error(EXP_SPI_ERROR_CAPTURE_FAIL);
        return false;
    }

    t2 = esp_timer_get_time();
    exp_convert_rgb565be_to_rgb888(fb->buf, s_rgb888_buf, IMG_WIDTH, IMG_HEIGHT);
    t3 = esp_timer_get_time();
    esp_camera_fb_return(fb);

    if (t_capture_us) *t_capture_us = (uint32_t)(t1 - t0);
    if (t_rgb_us) *t_rgb_us = (uint32_t)(t3 - t2);
    return true;
}

static bool send_raw_record(uint8_t order_index, uint32_t t_capture_us, uint32_t t_rgb_us)
{
    exp_record_header_t hdr;
    int64_t t0;
    int64_t t1;

    memset(&hdr, 0, sizeof(hdr));

    t0 = esp_timer_get_time();
    hdr.crc32 = exp_crc32_payload(s_rgb888_buf, RGB888_BUF_SIZE);
    t1 = esp_timer_get_time();

    hdr.magic = EXP_RECORD_MAGIC;
    hdr.version = EXP_RECORD_VERSION;
    hdr.header_size = EXP_RECORD_HEADER_SIZE;
    hdr.record_type = EXP_RECORD_RAW_FRAME;
    hdr.flags = EXP_RECORD_FLAG_CRC_PAYLOAD;
    hdr.run_id = s_run_id;
    hdr.seq_num = EXP_SEQ_RAW;
    hdr.order_index = order_index;
    hdr.method_id = EXP_METHOD_RAW;
    hdr.mode_type = EXP_MODE_RAW;
    hdr.width = IMG_WIDTH;
    hdr.height = IMG_HEIGHT;
    hdr.colorspace = JPEG_COLORSPACE_RGB;
    hdr.subsampling = EXP_SUBSAMPLING_NONE;
    hdr.k_idx = 0u;
    hdr.records_expected = exp_records_expected_for_method(s_all_mode ? EXP_METHOD_ALL : s_method);
    hdr.payload_len = (uint32_t)RGB888_BUF_SIZE;
    hdr.t_capture_us = t_capture_us;
    hdr.t_rgb565_to_rgb888_us = t_rgb_us;
    hdr.t_prepare_us = (uint32_t)(t1 - t0);
    hdr.t_algorithm_us = 0u;

    ESP_LOGI(TAG, "RAW: run=%lu order=%u bytes=%u pack_us=%lu",
             (unsigned long)hdr.run_id,
             (unsigned)order_index,
             (unsigned)hdr.payload_len,
             (unsigned long)hdr.t_prepare_us);

    if (!send_record_payload(&hdr, s_rgb888_buf)) {
        if (!spi_is_abort_requested()) {
            spi_mark_error(EXP_SPI_ERROR_PROTO);
        }
        return false;
    }
    return true;
}

static uint16_t psnr_x100_rgb888(const uint8_t *ref, const uint8_t *img, size_t len)
{
    uint64_t sse = 0u;

    if (!ref || !img || len == 0u) return 0u;
    for (size_t i = 0; i < len; i++) {
        int d = (int)ref[i] - (int)img[i];
        sse += (uint64_t)(d * d);
    }
    if (sse == 0u) return 9999u;

    double mse = (double)sse / (double)len;
    double psnr = 10.0 * log10((255.0 * 255.0) / mse);
    int value = (int)(psnr * 100.0 + 0.5);
    if (value < 0) return 0u;
    if (value > 9999) return 9999u;
    return (uint16_t)value;
}

static bool send_jpeg_record(uint8_t method_id, uint8_t order_index,
                             uint32_t t_capture_us, uint32_t t_rgb_us)
{
    exp_record_header_t hdr;
    jpeg_image_t img;
    jpeg_params_t params;
    jpeg_compressed_t *comp = NULL;
    int32_t frame_len;
    int64_t t0;
    int64_t t1;
    int64_t t2;
    int64_t t3;
    int64_t t4;
    int64_t t5;
    jpeg_error_t err;
    uint32_t local_decompress_us = 0u;
    uint32_t dct_kernel_us = 0u;
    uint16_t dct_kernel_calls = 0u;
    uint16_t local_psnr_x100 = 0u;

    memset(&hdr, 0, sizeof(hdr));

    img.width = IMG_WIDTH;
    img.height = IMG_HEIGHT;
    img.colorspace = JPEG_COLORSPACE_RGB;
    img.data = s_rgb888_buf;

    params.quality_factor = s_quality;
    params.dct_method = method_id_to_dct(method_id);
    params.subsampling = JPEG_SUBSAMP_444;
    params.flags = 0;

    log_psram_state("jpeg: antes compress");
    t0 = esp_timer_get_time();
    err = jpeg_compress(&img, &params, &comp);
    t1 = esp_timer_get_time();
    if (err != JPEG_SUCCESS || !comp) {
        ESP_LOGE(TAG, "%s: jpeg_compress falhou: %s",
                 exp_method_name(method_id), jpeg_error_string(err));
        spi_mark_error(EXP_SPI_ERROR_ENCODE_FAIL);
        return false;
    }
    dct_kernel_us = comp->dct_kernel_us;
    dct_kernel_calls = (comp->dct_kernel_calls > UINT16_MAX) ?
                       UINT16_MAX : (uint16_t)comp->dct_kernel_calls;

    t2 = esp_timer_get_time();
    frame_len = jpeg_frame_payload_encode(comp, s_frame_buf,
                                          EXP_JPEG_RLE_WORST_CASE_BYTES);
    t3 = esp_timer_get_time();
    if (frame_len <= 0) {
        ESP_LOGE(TAG, "%s: payload_encode falhou ou excedeu o limite de %u bytes",
                 exp_method_name(method_id), (unsigned)EXP_JPEG_RLE_WORST_CASE_BYTES);
        jpeg_free_compressed(comp);
        spi_mark_error(EXP_SPI_ERROR_ENCODE_FAIL);
        return false;
    }

    {
        jpeg_image_t recon = {
            .width = IMG_WIDTH,
            .height = IMG_HEIGHT,
            .colorspace = JPEG_COLORSPACE_RGB,
            .data = s_recon_buf,
        };
        t4 = esp_timer_get_time();
        err = jpeg_decompress_into(comp, &recon);
        t5 = esp_timer_get_time();
        local_decompress_us = (uint32_t)(t5 - t4);
        if (err == JPEG_SUCCESS) {
            local_psnr_x100 = psnr_x100_rgb888(s_rgb888_buf, s_recon_buf, RGB888_BUF_SIZE);
        } else {
            ESP_LOGW(TAG, "%s: decompress local falhou: %s",
                     exp_method_name(method_id), jpeg_error_string(err));
            local_psnr_x100 = 0u;
        }
    }

    hdr.magic = EXP_RECORD_MAGIC;
    hdr.version = EXP_RECORD_VERSION;
    hdr.header_size = EXP_RECORD_HEADER_SIZE;
    hdr.record_type = EXP_RECORD_JPEG_FRAME;
    hdr.flags = EXP_RECORD_FLAG_CRC_PAYLOAD;
    hdr.run_id = s_run_id;
    hdr.seq_num = method_id_to_seq(method_id);
    hdr.order_index = order_index;
    hdr.method_id = method_id;
    hdr.mode_type = EXP_MODE_JPEG;
    hdr.width = IMG_WIDTH;
    hdr.height = IMG_HEIGHT;
    hdr.colorspace = JPEG_COLORSPACE_RGB;
    hdr.subsampling = JPEG_SUBSAMP_444;
    hdr.k_idx = s_k_idx;
    hdr.records_expected = exp_records_expected_for_method(s_all_mode ? EXP_METHOD_ALL : s_method);
    hdr.payload_len = (uint32_t)frame_len;
    hdr.t_capture_us = t_capture_us;
    hdr.t_rgb565_to_rgb888_us = t_rgb_us;
    hdr.t_prepare_us = (uint32_t)(t3 - t2);
    hdr.t_algorithm_us = (uint32_t)(t1 - t0);
    hdr.t_decompress_us = local_decompress_us;
    hdr.psnr_x100 = local_psnr_x100;
    hdr.t_dct_kernel_us = dct_kernel_us;
    hdr.dct_kernel_calls = dct_kernel_calls;
    hdr.crc32 = exp_crc32_payload(s_frame_buf, (size_t)frame_len);

    jpeg_free_compressed(comp);

    ESP_LOGI(TAG, "%s: run=%lu order=%u bytes=%u comp_us=%lu dct_us=%lu dct_calls=%u decomp_us=%lu psnr=%.2f frame_us=%lu",
             exp_method_name(method_id),
             (unsigned long)hdr.run_id,
             (unsigned)order_index,
             (unsigned)hdr.payload_len,
             (unsigned long)hdr.t_algorithm_us,
             (unsigned long)hdr.t_dct_kernel_us,
             (unsigned)hdr.dct_kernel_calls,
             (unsigned long)hdr.t_decompress_us,
             (double)hdr.psnr_x100 / 100.0,
             (unsigned long)hdr.t_prepare_us);

    log_psram_state("jpeg: apos frame_encode");
    if (!send_record_payload(&hdr, s_frame_buf)) {
        if (!spi_is_abort_requested()) {
            spi_mark_error(EXP_SPI_ERROR_PROTO);
        }
        return false;
    }
    return true;
}

static void do_capture_and_send_all(void)
{
    uint32_t t_capture_us = 0;
    uint32_t t_rgb_us = 0;
    uint8_t order = 0;
    uint8_t saved_k_idx = s_k_idx;
    float saved_quality = s_quality;

    ESP_LOGI(TAG, "ALL: inicio run=%lu k=todos raw_first=%d records=%u",
             (unsigned long)s_run_id,
             (int)s_raw_first,
             (unsigned)exp_records_expected_for_method(EXP_METHOD_ALL));

    camera_warmup_discard_frames(CAMERA_PRECAPTURE_DISCARD_FRAMES);
    camera_wait_aec_stable();

    bool captured = false;
    for (unsigned retry = 0u; retry <= EXPOSURE_MAX_RETRIES; retry++) {
        if (!capture_rgb888_once(&t_capture_us, &t_rgb_us)) {
            s_k_idx   = saved_k_idx;
            s_quality = saved_quality;
            return;
        }
        uint8_t mean = rgb888_mean_brightness(s_rgb888_buf,
                                              (size_t)IMG_WIDTH * (size_t)IMG_HEIGHT);
        ESP_LOGI(TAG, "ALL: captura brilho_medio=%u (min=%u max=%u)",
                 mean, EXPOSURE_MEAN_MIN, EXPOSURE_MEAN_MAX);
        if (mean >= EXPOSURE_MEAN_MIN && mean <= EXPOSURE_MEAN_MAX) {
            captured = true;
            break;
        }
        if (retry < EXPOSURE_MAX_RETRIES) {
            ESP_LOGW(TAG, "ALL: exposicao fora da faixa (mean=%u) retry %u/%u",
                     mean, retry + 1u, EXPOSURE_MAX_RETRIES);
            camera_warmup_discard_frames(CAMERA_PRECAPTURE_DISCARD_FRAMES);
            camera_wait_aec_stable();
        }
    }
    if (!captured) {
        ESP_LOGW(TAG, "ALL: procedendo com exposicao subotima apos retries");
    }
    if (spi_is_abort_requested()) {
        ESP_LOGW(TAG, "ALL: abort detectado apos captura");
        s_k_idx   = saved_k_idx;
        s_quality = saved_quality;
        return;
    }

    ESP_LOGI(TAG, "ALL: captura_us=%lu rgb_us=%lu",
             (unsigned long)t_capture_us, (unsigned long)t_rgb_us);

    if (s_raw_first) {
        if (!send_raw_record(order++, t_capture_us, t_rgb_us)) goto out_restore;
    }
    for (uint8_t k = 0; k < EXP_K_FACTOR_COUNT; k++) {
        s_k_idx = k;
        s_quality = exp_k_factor_from_idx(k);
        ESP_LOGI(TAG, "ALL: processando k=%s", exp_k_label_from_idx(k));
        for (uint8_t i = 0; i < EXP_JPEG_METHOD_COUNT; i++) {
            if (!send_jpeg_record(exp_jpeg_method_at(i), order++, t_capture_us, t_rgb_us)) {
                goto out_restore;
            }
        }
    }
    if (!s_raw_first) {
        if (!send_raw_record(order++, t_capture_us, t_rgb_us)) goto out_restore;
    }

    spi_mark_done(s_run_id);
    ESP_LOGI(TAG, "ALL: fim run=%lu", (unsigned long)s_run_id);
    spi_mark_idle_after_done(s_run_id, "ALL");

out_restore:
    s_k_idx = saved_k_idx;
    s_quality = saved_quality;
}

static void do_capture_and_send(void)
{
    uint32_t t_capture_us = 0;
    uint32_t t_rgb_us = 0;

    ESP_LOGI(TAG, "single: inicio run=%lu method=%s k=%s",
             (unsigned long)s_run_id, exp_method_name(s_method), exp_k_label_from_idx(s_k_idx));

    camera_warmup_discard_frames(CAMERA_PRECAPTURE_DISCARD_FRAMES);

    if (!capture_rgb888_once(&t_capture_us, &t_rgb_us)) {
        return;
    }
    if (spi_is_abort_requested()) {
        ESP_LOGW(TAG, "single: abort detectado apos captura");
        return;
    }
    if (!send_jpeg_record(s_method, 0u, t_capture_us, t_rgb_us)) {
        return;
    }

    spi_mark_done(s_run_id);
    ESP_LOGI(TAG, "single: fim run=%lu", (unsigned long)s_run_id);
    spi_mark_idle_after_done(s_run_id, "single");
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  app_main
 * ═══════════════════════════════════════════════════════════════════════════ */

#define CAM_TASK_STACK_SIZE (16 * 1024)

static void cam_task(void *arg)
{
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "IC-JPEG CAM %s - cam_task started", jpeg_version());
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

    /* Plataforma de bancada pode exigir brownout desligado para a camera,
     * mas isso fica explicito e reversivel em compile-time. */
#if CAM_DISABLE_BROWNOUT
    ESP_LOGW(TAG, "brownout detector: desabilitado por CAM_DISABLE_BROWNOUT=1");
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#else
    ESP_LOGI(TAG, "brownout detector: mantido habilitado");
#endif

    /* ── Câmera ── */
    if (camera_init() != ESP_OK) {
        fatal_halt("Camera init falhou - verifique OV2640, SCCB (IO26/27), XCLK (IO0) e alimentacao");
    }
    ESP_LOGI(TAG, "Camera pronta (QVGA RGB565, %dx%d)", IMG_WIDTH, IMG_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_BOOT_STABILIZE_MS));
    camera_warmup_discard_frames(CAMERA_WARMUP_FRAMES);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!ensure_work_buffers()) {
        fatal_halt("Buffers de trabalho nao puderam ser alocados - halting");
    }
    ESP_LOGI(TAG, "Buffers de trabalho OK: rgb888=%u B recon=%u B frame=%u B",
	             (unsigned)RGB888_BUF_SIZE,
	             (unsigned)RGB888_BUF_SIZE,
	             (unsigned)JPEG_FRAME_MAX_PAYLOAD_BYTES);
    s_main_task = xTaskGetCurrentTaskHandle();
    gpio_init_cam();
    ESP_LOGI(TAG, "SPI GPIO OK - MISO=IO%d MOSI=IO%d SCLK=IO%d CS=IO%d READY=IO%d",
             SPI_MISO_PIN, SPI_MOSI_PIN, SPI_SCLK_PIN, SPI_CS_PIN, SPI_READY_PIN);
    xTaskCreatePinnedToCore(spi_service_task, "cam_spi",
                            6144, NULL,
                            6,
                            &s_spi_task,
                            0);

    ESP_LOGI(TAG, "=== PRONTO - aguardando comando SPI ===");

    /* ── Loop principal ─────────────────────────────────────────────────── */
    while (1) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50)) > 0) {
            if (spi_is_abort_requested()) {
                spi_clear_abort();
                spi_mark_idle();
                continue;
            }
            ESP_LOGI(TAG, "Disparo! method=%s k=%s",
                     s_all_mode ? "ALL" : exp_method_name(s_method),
                     exp_k_label_from_idx(s_k_idx));
            if (s_all_mode)
                do_capture_and_send_all();
            else
                do_capture_and_send();

            if (spi_is_abort_requested()) {
                spi_clear_abort();
                spi_mark_idle();
            }
        }
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "IC-JPEG CAM %s - boot", jpeg_version());
    ESP_LOGI(TAG, "boot provenance: version=%s proto_v=%u spi=%u",
             app ? app->version : "unknown",
             (unsigned)EXP_RECORD_VERSION,
             (unsigned)EXP_SPI_FREQ_HZ);

    xTaskCreatePinnedToCore(cam_task, "cam_tsk",
                            CAM_TASK_STACK_SIZE, NULL,
                            5,      /* prioridade */
                            NULL,   /* handle */
                            1);     /* core 1 - libera core 0 para driver da câmera e ISRs */
}
