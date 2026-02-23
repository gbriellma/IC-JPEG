    #include "webserver.h"
#include "index_html.h"
#include "metrics.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"

#include "jpeg_codec.h"
#include "internal.h"

static const char *TAG = "webserver";

static SemaphoreHandle_t camera_mutex;

/* ========================================================================== *
 *  BMP conversion (RGB888 -> BMP24)
 * ========================================================================== */

#pragma pack(push, 1)
typedef struct {
    uint8_t  signature[2];
    uint32_t file_size;
    uint32_t reserved;
    uint32_t data_offset;
} bmp_file_header_t;

typedef struct {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;
#pragma pack(pop)

#define BMP_HEADER_SIZE (sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))

/**
 * Convert RGB888 buffer to BMP24 in-memory.
 * Caller must free() the returned buffer.
 * Returns NULL on allocation failure.
 */
static uint8_t *rgb888_to_bmp(const uint8_t *rgb, int width, int height,
                               size_t *out_size)
{
    int row_bytes = width * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int padded_row = row_bytes + pad;
    size_t pixel_size = (size_t)padded_row * height;
    size_t total = BMP_HEADER_SIZE + pixel_size;

    uint8_t *buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    /* File header */
    bmp_file_header_t *fh = (bmp_file_header_t *)buf;
    fh->signature[0] = 'B';
    fh->signature[1] = 'M';
    fh->file_size = (uint32_t)total;
    fh->reserved = 0;
    fh->data_offset = BMP_HEADER_SIZE;

    /* Info header (negative height = top-to-bottom scanlines) */
    bmp_info_header_t *ih = (bmp_info_header_t *)(buf + sizeof(bmp_file_header_t));
    ih->header_size = 40;
    ih->width = width;
    ih->height = -height;
    ih->planes = 1;
    ih->bpp = 24;
    ih->compression = 0;
    ih->image_size = (uint32_t)pixel_size;
    ih->x_ppm = 2835;
    ih->y_ppm = 2835;
    ih->colors_used = 0;
    ih->colors_important = 0;

    /* Pixel data: RGB -> BGR */
    uint8_t *dst = buf + BMP_HEADER_SIZE;
    const uint8_t *src = rgb;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            dst[0] = src[2]; /* B */
            dst[1] = src[1]; /* G */
            dst[2] = src[0]; /* R */
            dst += 3;
            src += 3;
        }
        for (int p = 0; p < pad; p++)
            *dst++ = 0;
    }

    *out_size = total;
    return buf;
}

/* ========================================================================== *
 *  GET / -- serve HTML interface
 * ========================================================================== */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

/* ========================================================================== *
 *  GET /capture -- capture, compress, decompress, return BMP + metrics
 * ========================================================================== */

static esp_err_t capture_handler(httpd_req_t *req)
{
    /* 1. Parse query parameters */
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    jpeg_dct_method_t method = JPEG_DCT_LOEFFLER;
    const char *method_name = "Loeffler";
    float quality = 2.0f;

    char param_val[16];
    if (httpd_query_key_value(query, "method", param_val, sizeof(param_val)) == ESP_OK) {
        if (strcmp(param_val, "matrix") == 0) {
            method = JPEG_DCT_MATRIX;
            method_name = "Matrix";
        } else if (strcmp(param_val, "approx") == 0) {
            method = JPEG_DCT_APPROX;
            method_name = "Approx";
        } else if (strcmp(param_val, "identity") == 0) {
            method = JPEG_DCT_IDENTITY;
            method_name = "Identity";
        }
    }
    if (httpd_query_key_value(query, "quality", param_val, sizeof(param_val)) == ESP_OK) {
        float q = strtof(param_val, NULL);
        if (q >= 1.0f && q <= 8.0f) quality = q;
    }

    /* 2. Acquire camera */
    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera busy");
        return ESP_FAIL;
    }

    /* Discard stale DMA-buffered frame, then grab a fresh one */
    camera_fb_t *stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(camera_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
        return ESP_FAIL;
    }

    int w = fb->width;
    int h = fb->height;
    int npix = w * h;

    /* 3. RGB565 -> RGB888 */
    uint8_t *rgb888 = (uint8_t *)heap_caps_malloc(npix * 3, MALLOC_CAP_SPIRAM);
    if (!rgb888) {
        esp_camera_fb_return(fb);
        xSemaphoreGive(camera_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "malloc rgb888 failed");
        return ESP_FAIL;
    }
    convert_rgb565_to_rgb888((const uint16_t *)fb->buf, rgb888, w, h);

    /* Return framebuffer early */
    esp_camera_fb_return(fb);
    xSemaphoreGive(camera_mutex);

    /* 4. Compress */
    jpeg_image_t image = {
        .width = w,
        .height = h,
        .colorspace = JPEG_COLORSPACE_RGB,
        .data = rgb888,
    };
    jpeg_params_t params = {
        .quality_factor = quality,
        .dct_method = method,
        .use_standard_tables = 1,
        .skip_quantization = 0,
    };

    jpeg_compressed_t *comp = NULL;
    int64_t t0 = esp_timer_get_time();
    jpeg_error_t jerr = jpeg_compress(&image, &params, &comp);
    int64_t t_compress = esp_timer_get_time() - t0;

    if (jerr != JPEG_SUCCESS) {
        free(rgb888);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Compress failed");
        return ESP_FAIL;
    }

    /* Free raw DCT coefficients (not needed for decompress/metrics) to save ~900KB PSRAM */
    free(comp->y_coeffs);  comp->y_coeffs  = NULL;
    free(comp->cb_coeffs); comp->cb_coeffs = NULL;
    free(comp->cr_coeffs); comp->cr_coeffs = NULL;

    /* 5. Decompress */
    jpeg_image_t *recon = NULL;
    t0 = esp_timer_get_time();
    jerr = jpeg_decompress(comp, &recon);
    int64_t t_decompress = esp_timer_get_time() - t0;

    if (jerr != JPEG_SUCCESS) {
        jpeg_free_compressed(comp);
        free(rgb888);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Decompress failed");
        return ESP_FAIL;
    }

    /* 6. Metrics */
    double psnr = calc_psnr(rgb888, recon->data, w, h);
    double bitrate = calc_bitrate(comp);

    ESP_LOGI(TAG, "[%s] q=%.1f | PSNR %.2f dB | BR %.3f bpp | enc %lld us | dec %lld us",
             method_name, quality, psnr, bitrate,
             (long long)t_compress, (long long)t_decompress);

    /* 7. Free large buffers before BMP allocation */
    jpeg_free_compressed(comp);
    free(rgb888);

    /* 8. Convert reconstructed image to BMP */
    size_t bmp_size = 0;
    uint8_t *bmp_buf = rgb888_to_bmp(recon->data, w, h, &bmp_size);
    jpeg_free_image(recon);

    if (!bmp_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "BMP malloc failed");
        return ESP_FAIL;
    }

    /* 9. Set response headers (metrics) */
    char hdr_psnr[16], hdr_bitrate[16], hdr_enc[16], hdr_dec[16];
    char hdr_quality[8];

    snprintf(hdr_psnr, sizeof(hdr_psnr), "%.2f", psnr);
    snprintf(hdr_bitrate, sizeof(hdr_bitrate), "%.3f", bitrate);
    snprintf(hdr_enc, sizeof(hdr_enc), "%lld", (long long)t_compress);
    snprintf(hdr_dec, sizeof(hdr_dec), "%lld", (long long)t_decompress);
    snprintf(hdr_quality, sizeof(hdr_quality), "%.1f", quality);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "X-PSNR", hdr_psnr);
    httpd_resp_set_hdr(req, "X-Bitrate", hdr_bitrate);
    httpd_resp_set_hdr(req, "X-Compress-Time-Us", hdr_enc);
    httpd_resp_set_hdr(req, "X-Decompress-Time-Us", hdr_dec);
    httpd_resp_set_hdr(req, "X-Method", method_name);
    httpd_resp_set_hdr(req, "X-Quality", hdr_quality);
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers",
        "X-PSNR,X-Bitrate,X-Compress-Time-Us,X-Decompress-Time-Us,X-Method,X-Quality");

    /* 10. Send BMP */
    httpd_resp_send(req, (const char *)bmp_buf, bmp_size);
    free(bmp_buf);

    return ESP_OK;
}

/* ========================================================================== *
 *  Helper: stream large buffer in small chunks
 * ========================================================================== */

static esp_err_t send_chunked(httpd_req_t *req, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        size_t chunk = (len > 4096) ? 4096 : len;
        esp_err_t err = httpd_resp_send_chunk(req, p, chunk);
        if (err != ESP_OK) return err;
        p   += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

/* ========================================================================== *
 *  GET /capture_compressed -- send quantized coefficients as int16
 *
 *  Binary body (all little-endian, matching ESP32 native):
 *    [Y  quantized int16[]]     num_blocks*64*2 bytes
 *    [Cb quantized int16[]]     num_blocks*64*2 bytes
 *    [Cr quantized int16[]]     num_blocks*64*2 bytes
 *
 *  Only compression is done on-device.  Bitrate is computed from the
 *  quantized coefficients.  PSNR is NOT computed here — the PC side
 *  decompresses and calculates PSNR itself.
 * ========================================================================== */

/* Pack int32 quantized coefficients into int16 (values always fit) */
static int16_t* pack_int16(const int32_t *src, int count) {
    int16_t *dst = (int16_t *)heap_caps_malloc(count * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM);
    if (!dst) return NULL;
    for (int i = 0; i < count; i++)
        dst[i] = (int16_t)src[i];
    return dst;
}

static esp_err_t compressed_handler(httpd_req_t *req)
{
    /* 1. Parse query parameters */
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    jpeg_dct_method_t method = JPEG_DCT_LOEFFLER;
    const char *method_name = "loeffler";
    float quality = 2.0f;

    char param_val[16];
    if (httpd_query_key_value(query, "method", param_val, sizeof(param_val)) == ESP_OK) {
        if (strcmp(param_val, "matrix") == 0) {
            method = JPEG_DCT_MATRIX;
            method_name = "matrix";
        } else if (strcmp(param_val, "approx") == 0) {
            method = JPEG_DCT_APPROX;
            method_name = "approx";
        } else if (strcmp(param_val, "identity") == 0) {
            method = JPEG_DCT_IDENTITY;
            method_name = "identity";
        }
    }
    if (httpd_query_key_value(query, "quality", param_val, sizeof(param_val)) == ESP_OK) {
        float q = strtof(param_val, NULL);
        if (q >= 1.0f && q <= 8.0f) quality = q;
    }

    /* 2. Acquire camera */
    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera busy");
        return ESP_FAIL;
    }

    /* Discard stale DMA-buffered frame, then grab a fresh one */
    camera_fb_t *stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        xSemaphoreGive(camera_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
        return ESP_FAIL;
    }

    int w = fb->width;
    int h = fb->height;
    int npix = w * h;

    /* 3. RGB565 -> RGB888 */
    uint8_t *rgb888 = (uint8_t *)heap_caps_malloc(npix * 3, MALLOC_CAP_SPIRAM);
    if (!rgb888) {
        esp_camera_fb_return(fb);
        xSemaphoreGive(camera_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "malloc failed");
        return ESP_FAIL;
    }
    convert_rgb565_to_rgb888((const uint16_t *)fb->buf, rgb888, w, h);
    esp_camera_fb_return(fb);
    xSemaphoreGive(camera_mutex);

    /* 4. Compress */
    jpeg_image_t image = {
        .width = w, .height = h,
        .colorspace = JPEG_COLORSPACE_RGB,
        .data = rgb888,
    };
    jpeg_params_t params = {
        .quality_factor = quality,
        .dct_method = method,
        .use_standard_tables = 1,
        .skip_quantization = 0,
    };

    jpeg_compressed_t *comp = NULL;
    int64_t t0 = esp_timer_get_time();
    jpeg_error_t jerr = jpeg_compress(&image, &params, &comp);
    int64_t t_compress = esp_timer_get_time() - t0;

    if (jerr != JPEG_SUCCESS) {
        free(rgb888);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Compress failed");
        return ESP_FAIL;
    }

    /* Free raw DCT coefficients — only quantized are needed */
    free(comp->y_coeffs);  comp->y_coeffs  = NULL;
    free(comp->cb_coeffs); comp->cb_coeffs = NULL;
    free(comp->cr_coeffs); comp->cr_coeffs = NULL;

    /* 5. Bitrate only — no decompress on ESP32 for Method B */
    double bitrate = calc_bitrate(comp);

    free(rgb888);

    /* 6. Pack coefficients to int16 (halves payload) */
    int total_coefs = (int)comp->num_blocks_y * 64;
    int16_t *y16  = pack_int16(comp->y_quantized,  total_coefs);
    int16_t *cb16 = pack_int16(comp->cb_quantized, total_coefs);
    int16_t *cr16 = pack_int16(comp->cr_quantized, total_coefs);

    if (!y16 || !cb16 || !cr16) {
        free(y16); free(cb16); free(cr16);
        jpeg_free_compressed(comp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "int16 malloc failed");
        return ESP_FAIL;
    }

    /* 7. Set response headers with metadata */
    char hdr_w[8], hdr_h[8], hdr_q[8], hdr_nb[8];
    char hdr_tc[16], hdr_br[16];

    snprintf(hdr_w,  sizeof(hdr_w),  "%d", w);
    snprintf(hdr_h,  sizeof(hdr_h),  "%d", h);
    snprintf(hdr_q,  sizeof(hdr_q),  "%.1f", quality);
    snprintf(hdr_nb, sizeof(hdr_nb), "%d", (int)comp->num_blocks_y);
    snprintf(hdr_tc, sizeof(hdr_tc), "%lld", (long long)t_compress);
    snprintf(hdr_br, sizeof(hdr_br), "%.3f", bitrate);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Width",  hdr_w);
    httpd_resp_set_hdr(req, "X-Height", hdr_h);
    httpd_resp_set_hdr(req, "X-Method", method_name);
    httpd_resp_set_hdr(req, "X-Quality", hdr_q);
    httpd_resp_set_hdr(req, "X-Num-Blocks", hdr_nb);
    httpd_resp_set_hdr(req, "X-Compress-Time-Us", hdr_tc);
    httpd_resp_set_hdr(req, "X-Bitrate", hdr_br);
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers",
        "X-Width,X-Height,X-Method,X-Quality,X-Num-Blocks,"
        "X-Compress-Time-Us,X-Bitrate");

    /* 8. Stream binary body: 3 quantized channels as int16 */
    size_t ch_bytes = (size_t)total_coefs * sizeof(int16_t);
    size_t payload  = 3 * ch_bytes;

    send_chunked(req, y16,  ch_bytes);
    send_chunked(req, cb16, ch_bytes);
    send_chunked(req, cr16, ch_bytes);
    httpd_resp_send_chunk(req, NULL, 0);          /* end chunked */

    ESP_LOGI(TAG, "[compressed] %s q=%.1f | BR %.3f bpp | "
             "enc %lld us | payload %zu B",
             method_name, quality, bitrate,
             (long long)t_compress, payload);

    free(y16); free(cb16); free(cr16);
    jpeg_free_compressed(comp);
    return ESP_OK;
}

/* ========================================================================== *
 *  Server init
 * ========================================================================== */

/* ---- Helper: parse method + quality from query string ---- */
static void parse_method_quality(httpd_req_t *req,
                                 jpeg_dct_method_t *method,
                                 const char **method_name,
                                 float *quality)
{
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    *method      = JPEG_DCT_LOEFFLER;
    *method_name = "loeffler";
    *quality     = 2.0f;

    char val[16];
    if (httpd_query_key_value(query, "method", val, sizeof(val)) == ESP_OK) {
        if (strcmp(val, "matrix") == 0) {
            *method = JPEG_DCT_MATRIX;  *method_name = "matrix";
        } else if (strcmp(val, "approx") == 0) {
            *method = JPEG_DCT_APPROX;  *method_name = "approx";
        } else if (strcmp(val, "identity") == 0) {
            *method = JPEG_DCT_IDENTITY;  *method_name = "identity";
        }
    }
    if (httpd_query_key_value(query, "quality", val, sizeof(val)) == ESP_OK) {
        float q = strtof(val, NULL);
        if (q >= 1.0f && q <= 8.0f) *quality = q;
    }
}

/* ---- Helper: parse width + height from query string ---- */
static void parse_dimensions(httpd_req_t *req, int *w, int *h)
{
    char query[64] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    *w = 320;
    *h = 240;

    char val[16];
    if (httpd_query_key_value(query, "width", val, sizeof(val)) == ESP_OK) {
        int v = atoi(val);
        if (v > 0 && v <= 1600) *w = v;
    }
    if (httpd_query_key_value(query, "height", val, sizeof(val)) == ESP_OK) {
        int v = atoi(val);
        if (v > 0 && v <= 1200) *h = v;
    }
}

/* ---- Helper: receive full POST body into a PSRAM buffer ---- */
static uint8_t *receive_post_body(httpd_req_t *req, size_t expected)
{
    if ((size_t)req->content_len != expected) {
        ESP_LOGE(TAG, "POST body size %d != expected %zu",
                 req->content_len, expected);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(expected, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    size_t received = 0;
    while (received < expected) {
        int ret = httpd_req_recv(req, (char *)(buf + received),
                                 expected - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return NULL;
        }
        received += (size_t)ret;
    }
    return buf;
}

/* ========================================================================== *
 *  POST /process -- receive RGB888, compress+decompress, return BMP + metrics
 *
 *  Query params: method, quality, width, height
 *  POST body:    raw RGB888 (width * height * 3 bytes)
 *  Response:     BMP image  + same headers as GET /capture
 * ========================================================================== */

static esp_err_t process_handler(httpd_req_t *req)
{
    jpeg_dct_method_t method;  const char *method_name;  float quality;
    parse_method_quality(req, &method, &method_name, &quality);

    int w, h;
    parse_dimensions(req, &w, &h);

    size_t rgb_size = (size_t)w * h * 3;
    uint8_t *rgb888 = receive_post_body(req, rgb_size);
    if (!rgb888) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Bad body size (expected width*height*3 RGB888)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[process] %dx%d %s q=%.1f, received %zu B",
             w, h, method_name, quality, rgb_size);

    /* Compress */
    jpeg_image_t image = {
        .width = w, .height = h,
        .colorspace = JPEG_COLORSPACE_RGB,
        .data = rgb888,
    };
    jpeg_params_t params = {
        .quality_factor = quality,
        .dct_method = method,
        .use_standard_tables = 1,
        .skip_quantization = 0,
    };

    jpeg_compressed_t *comp = NULL;
    int64_t t0 = esp_timer_get_time();
    jpeg_error_t jerr = jpeg_compress(&image, &params, &comp);
    int64_t t_compress = esp_timer_get_time() - t0;

    if (jerr != JPEG_SUCCESS) {
        free(rgb888);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Compress failed");
        return ESP_FAIL;
    }

    free(comp->y_coeffs);  comp->y_coeffs  = NULL;
    free(comp->cb_coeffs); comp->cb_coeffs = NULL;
    free(comp->cr_coeffs); comp->cr_coeffs = NULL;

    /* Decompress */
    jpeg_image_t *recon = NULL;
    t0 = esp_timer_get_time();
    jerr = jpeg_decompress(comp, &recon);
    int64_t t_decompress = esp_timer_get_time() - t0;

    if (jerr != JPEG_SUCCESS) {
        jpeg_free_compressed(comp);
        free(rgb888);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Decompress failed");
        return ESP_FAIL;
    }

    /* Metrics */
    double psnr = calc_psnr(rgb888, recon->data, w, h);
    double bitrate = calc_bitrate(comp);

    ESP_LOGI(TAG, "[process] %s q=%.1f | PSNR %.2f dB | BR %.3f bpp | "
             "enc %lld us | dec %lld us",
             method_name, quality, psnr, bitrate,
             (long long)t_compress, (long long)t_decompress);

    jpeg_free_compressed(comp);
    free(rgb888);

    /* BMP */
    size_t bmp_size = 0;
    uint8_t *bmp_buf = rgb888_to_bmp(recon->data, w, h, &bmp_size);
    jpeg_free_image(recon);

    if (!bmp_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "BMP malloc failed");
        return ESP_FAIL;
    }

    /* Headers */
    char hdr_psnr[16], hdr_br[16], hdr_tc[16], hdr_td[16], hdr_q[8];
    snprintf(hdr_psnr, sizeof(hdr_psnr), "%.2f", psnr);
    snprintf(hdr_br,   sizeof(hdr_br),   "%.3f", bitrate);
    snprintf(hdr_tc,   sizeof(hdr_tc),   "%lld", (long long)t_compress);
    snprintf(hdr_td,   sizeof(hdr_td),   "%lld", (long long)t_decompress);
    snprintf(hdr_q,    sizeof(hdr_q),    "%.1f", quality);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "X-PSNR", hdr_psnr);
    httpd_resp_set_hdr(req, "X-Bitrate", hdr_br);
    httpd_resp_set_hdr(req, "X-Compress-Time-Us", hdr_tc);
    httpd_resp_set_hdr(req, "X-Decompress-Time-Us", hdr_td);
    httpd_resp_set_hdr(req, "X-Method", method_name);
    httpd_resp_set_hdr(req, "X-Quality", hdr_q);
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers",
        "X-PSNR,X-Bitrate,X-Compress-Time-Us,X-Decompress-Time-Us,"
        "X-Method,X-Quality");

    httpd_resp_send(req, (const char *)bmp_buf, bmp_size);
    free(bmp_buf);
    return ESP_OK;
}

/* ========================================================================== *
 *  POST /process_compressed -- receive RGB888, compress, return int16 coeffs
 *
 *  Query params: method, quality, width, height
 *  POST body:    raw RGB888 (width * height * 3 bytes)
 *  Response:     3× int16 quantized channels + metrics in headers
 * ========================================================================== */

static esp_err_t process_compressed_handler(httpd_req_t *req)
{
    jpeg_dct_method_t method;  const char *method_name;  float quality;
    parse_method_quality(req, &method, &method_name, &quality);

    int w, h;
    parse_dimensions(req, &w, &h);

    size_t rgb_size = (size_t)w * h * 3;
    uint8_t *rgb888 = receive_post_body(req, rgb_size);
    if (!rgb888) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Bad body size (expected width*height*3 RGB888)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[process_compressed] %dx%d %s q=%.1f, received %zu B",
             w, h, method_name, quality, rgb_size);

    /* Compress */
    jpeg_image_t image = {
        .width = w, .height = h,
        .colorspace = JPEG_COLORSPACE_RGB,
        .data = rgb888,
    };
    jpeg_params_t params = {
        .quality_factor = quality,
        .dct_method = method,
        .use_standard_tables = 1,
        .skip_quantization = 0,
    };

    jpeg_compressed_t *comp = NULL;
    int64_t t0 = esp_timer_get_time();
    jpeg_error_t jerr = jpeg_compress(&image, &params, &comp);
    int64_t t_compress = esp_timer_get_time() - t0;

    if (jerr != JPEG_SUCCESS) {
        free(rgb888);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Compress failed");
        return ESP_FAIL;
    }

    free(comp->y_coeffs);  comp->y_coeffs  = NULL;
    free(comp->cb_coeffs); comp->cb_coeffs = NULL;
    free(comp->cr_coeffs); comp->cr_coeffs = NULL;

    /* Bitrate only — no decompress on ESP32 for Method B */
    double bitrate = calc_bitrate(comp);

    free(rgb888);

    /* Pack int16 */
    int total_coefs = (int)comp->num_blocks_y * 64;
    int16_t *y16  = pack_int16(comp->y_quantized,  total_coefs);
    int16_t *cb16 = pack_int16(comp->cb_quantized, total_coefs);
    int16_t *cr16 = pack_int16(comp->cr_quantized, total_coefs);

    if (!y16 || !cb16 || !cr16) {
        free(y16); free(cb16); free(cr16);
        jpeg_free_compressed(comp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "int16 malloc failed");
        return ESP_FAIL;
    }

    /* Headers */
    char hdr_w[8], hdr_h[8], hdr_q[8], hdr_nb[8];
    char hdr_tc[16], hdr_br[16];

    snprintf(hdr_w,  sizeof(hdr_w),  "%d", w);
    snprintf(hdr_h,  sizeof(hdr_h),  "%d", h);
    snprintf(hdr_q,  sizeof(hdr_q),  "%.1f", quality);
    snprintf(hdr_nb, sizeof(hdr_nb), "%d", (int)comp->num_blocks_y);
    snprintf(hdr_tc, sizeof(hdr_tc), "%lld", (long long)t_compress);
    snprintf(hdr_br, sizeof(hdr_br), "%.3f", bitrate);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Width",  hdr_w);
    httpd_resp_set_hdr(req, "X-Height", hdr_h);
    httpd_resp_set_hdr(req, "X-Method", method_name);
    httpd_resp_set_hdr(req, "X-Quality", hdr_q);
    httpd_resp_set_hdr(req, "X-Num-Blocks", hdr_nb);
    httpd_resp_set_hdr(req, "X-Compress-Time-Us", hdr_tc);
    httpd_resp_set_hdr(req, "X-Bitrate", hdr_br);
    httpd_resp_set_hdr(req, "Access-Control-Expose-Headers",
        "X-Width,X-Height,X-Method,X-Quality,X-Num-Blocks,"
        "X-Compress-Time-Us,X-Bitrate");

    /* Stream int16 body */
    size_t ch_bytes = (size_t)total_coefs * sizeof(int16_t);
    size_t payload  = 3 * ch_bytes;

    send_chunked(req, y16,  ch_bytes);
    send_chunked(req, cb16, ch_bytes);
    send_chunked(req, cr16, ch_bytes);
    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "[process_compressed] %s q=%.1f | BR %.3f bpp | "
             "enc %lld us | payload %zu B",
             method_name, quality, bitrate,
             (long long)t_compress, payload);

    free(y16); free(cb16); free(cr16);
    jpeg_free_compressed(comp);
    return ESP_OK;
}

/* ========================================================================== */

esp_err_t webserver_start(void)
{
    camera_mutex = xSemaphoreCreateMutex();
    if (!camera_mutex) {
        ESP_LOGE(TAG, "Failed to create camera mutex");
        return ESP_FAIL;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 16384;
    config.max_uri_handlers = 8;
    config.max_resp_headers = 12;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
    };
    httpd_register_uri_handler(server, &capture_uri);

    httpd_uri_t compressed_uri = {
        .uri = "/capture_compressed",
        .method = HTTP_GET,
        .handler = compressed_handler,
    };
    httpd_register_uri_handler(server, &compressed_uri);

    httpd_uri_t process_uri = {
        .uri = "/process",
        .method = HTTP_POST,
        .handler = process_handler,
    };
    httpd_register_uri_handler(server, &process_uri);

    httpd_uri_t process_compressed_uri = {
        .uri = "/process_compressed",
        .method = HTTP_POST,
        .handler = process_compressed_handler,
    };
    httpd_register_uri_handler(server, &process_compressed_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d  (%d URI handlers)",
             config.server_port, 5);
    return ESP_OK;
}
