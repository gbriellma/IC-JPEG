/**
 * @file main.c
 * @brief ESP32-CAM — Web interface for DCT compression comparison
 *
 * Fluxo:
 *   camera_init() → wifi_init_apsta() → webserver_start()
 *   Browser GET /capture?method=X&quality=Y
 *       → esp32-camera (PIXFORMAT_RGB565)
 *       → convert_rgb565_to_rgb888()
 *       → jpeg_compress() / jpeg_decompress()
 *       → BMP + métricas via HTTP
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_camera.h"

#include "jpeg_codec.h"
#include "wifi.h"
#include "webserver.h"

static const char *TAG = "ic-jpeg";

/* ═══════════════════════════════════════════════════════════════════════════
 *  Pin mapping — AI-Thinker ESP32-CAM (OV2640)
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1  /* software reset */
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  Configurações de captura
 * ═══════════════════════════════════════════════════════════════════════════ */
#define FRAME_WIDTH   FRAMESIZE_QVGA   /* 320 × 240 */
#define XCLK_FREQ_HZ  20000000

/* ═══════════════════════════════════════════════════════════════════════════
 *  Inicialização da câmera
 * ═══════════════════════════════════════════════════════════════════════════ */
static esp_err_t camera_init(void)
{
    camera_config_t cfg = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,

        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href  = CAM_PIN_HREF,
        .pin_pclk  = CAM_PIN_PCLK,

        .xclk_freq_hz = XCLK_FREQ_HZ,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_RGB565,
        .frame_size   = FRAME_WIDTH,
        .fb_count     = 1,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location  = CAMERA_FB_IN_PSRAM,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
    }
    return err;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Entry point
 * ═══════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando camera...");
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicializacao da camera. Abortando.");
        return;
    }
    ESP_LOGI(TAG, "Camera pronta.");

    /* Delay entre inicializacoes pesadas para evitar pico de corrente */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Inicializando WiFi...");
    if (wifi_init_apsta() != ESP_OK) {
        ESP_LOGE(TAG, "Falha na inicializacao do WiFi. Abortando.");
        return;
    }

    ESP_LOGI(TAG, "Iniciando servidor web...");
    if (webserver_start() != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor web.");
        return;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  IC-JPEG v%s — Web UI", jpeg_version());
    ESP_LOGI(TAG, "  AP: http://192.168.4.1");
    ESP_LOGI(TAG, "  (Verifique logs para IP STA)");
    ESP_LOGI(TAG, "========================================");
}
