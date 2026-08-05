/**
 * @file metrics.c
 * @brief Metrics, summary formatting and reference-vs-reconstruction analysis.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "metrics.h"

static const char *TAG = "metrics";
static int32_t *s_ssim_work = NULL;
static int s_ssim_work_width = 0;

bool metrics_init_ssim_workspace(int max_width)
{
    size_t elems;
    size_t bytes;

    if (max_width <= 0) return false;
    if (s_ssim_work && s_ssim_work_width >= max_width) return true;

    elems = (size_t)max_width * 5u;
    bytes = elems * sizeof(*s_ssim_work);
    if (s_ssim_work) {
        heap_caps_free(s_ssim_work);
        s_ssim_work = NULL;
        s_ssim_work_width = 0;
    }

    s_ssim_work = (int32_t *)heap_caps_calloc(elems, sizeof(*s_ssim_work),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ssim_work) {
        s_ssim_work = (int32_t *)heap_caps_calloc(elems, sizeof(*s_ssim_work), MALLOC_CAP_8BIT);
    }
    if (!s_ssim_work) {
        ESP_LOGW(TAG, "SSIM workspace nao alocado (%u bytes)", (unsigned)bytes);
        return false;
    }

    s_ssim_work_width = max_width;
    ESP_LOGI(TAG, "SSIM workspace alocado (%u bytes, width=%d)", (unsigned)bytes, max_width);
    return true;
}

static double metrics_calc_ssim_channel_naive(const uint8_t *ref, const uint8_t *img,
                                              int width, int height, int stride)
{
    const double c1 = 6.5025;
    const double c2 = 58.5225;
    const int window_size = 7;
    const int half_window = window_size / 2;
    double ssim_sum = 0.0;
    int count = 0;
    int y, x, wy, wx;

    for (y = half_window; y < height - half_window; y++) {
        for (x = half_window; x < width - half_window; x++) {
            double mu_ref = 0.0;
            double mu_img = 0.0;
            double var_ref = 0.0;
            double var_img = 0.0;
            double cov = 0.0;
            int samples = 0;

            for (wy = -half_window; wy <= half_window; wy++) {
                for (wx = -half_window; wx <= half_window; wx++) {
                    int idx = ((y + wy) * width + (x + wx)) * stride;
                    mu_ref += (double)ref[idx];
                    mu_img += (double)img[idx];
                    samples++;
                }
            }

            mu_ref /= (double)samples;
            mu_img /= (double)samples;

            for (wy = -half_window; wy <= half_window; wy++) {
                for (wx = -half_window; wx <= half_window; wx++) {
                    int idx = ((y + wy) * width + (x + wx)) * stride;
                    double dr = (double)ref[idx] - mu_ref;
                    double di = (double)img[idx] - mu_img;
                    var_ref += dr * dr;
                    var_img += di * di;
                    cov += dr * di;
                }
            }

            if (samples > 1) {
                var_ref /= (double)(samples - 1);
                var_img /= (double)(samples - 1);
                cov /= (double)(samples - 1);
            }

            ssim_sum += ((2.0 * mu_ref * mu_img + c1) * (2.0 * cov + c2)) /
                        ((mu_ref * mu_ref + mu_img * mu_img + c1) * (var_ref + var_img + c2));
            count++;
        }
    }

    return count > 0 ? (ssim_sum / (double)count) : 1.0;
}

static double metrics_calc_ssim_channel(const uint8_t *ref, const uint8_t *img,
                                        int width, int height, int stride)
{
    const double c1 = 6.5025;
    const double c2 = 58.5225;
    const int window_size = 7;
    const int half_window = window_size / 2;
    const double inv_samples = 1.0 / 49.0;
    const double inv_samples_m1 = 1.0 / 48.0;
    double ssim_sum = 0.0;
    int count = 0;
    int32_t *work = s_ssim_work;
    int32_t *col_ref;
    int32_t *col_img;
    int32_t *col_ref2;
    int32_t *col_img2;
    int32_t *col_xy;
    int x;
    int y;

    if (width < window_size || height < window_size) return 1.0;
    if (!work || width > s_ssim_work_width) {
        return metrics_calc_ssim_channel_naive(ref, img, width, height, stride);
    }
    memset(work, 0, (size_t)width * 5u * sizeof(*work));

    col_ref = work;
    col_img = work + width;
    col_ref2 = work + (width * 2);
    col_img2 = work + (width * 3);
    col_xy = work + (width * 4);

    for (x = 0; x < width; x++) {
        int32_t sum_ref = 0;
        int32_t sum_img = 0;
        int32_t sum_ref2 = 0;
        int32_t sum_img2 = 0;
        int32_t sum_xy = 0;

        for (y = 0; y < window_size; y++) {
            int idx = ((y * width) + x) * stride;
            int32_t rv = ref[idx];
            int32_t iv = img[idx];

            sum_ref += rv;
            sum_img += iv;
            sum_ref2 += rv * rv;
            sum_img2 += iv * iv;
            sum_xy += rv * iv;
        }

        col_ref[x] = sum_ref;
        col_img[x] = sum_img;
        col_ref2[x] = sum_ref2;
        col_img2[x] = sum_img2;
        col_xy[x] = sum_xy;
    }

    for (y = half_window; y < height - half_window; y++) {
        int64_t sum_ref = 0;
        int64_t sum_img = 0;
        int64_t sum_ref2 = 0;
        int64_t sum_img2 = 0;
        int64_t sum_xy = 0;

        for (x = 0; x < window_size; x++) {
            sum_ref += col_ref[x];
            sum_img += col_img[x];
            sum_ref2 += col_ref2[x];
            sum_img2 += col_img2[x];
            sum_xy += col_xy[x];
        }

        for (x = half_window; x < width - half_window; x++) {
            double mu_ref = (double)sum_ref * inv_samples;
            double mu_img = (double)sum_img * inv_samples;
            double var_ref = ((double)sum_ref2 - ((double)sum_ref * (double)sum_ref * inv_samples)) *
                             inv_samples_m1;
            double var_img = ((double)sum_img2 - ((double)sum_img * (double)sum_img * inv_samples)) *
                             inv_samples_m1;
            double cov = ((double)sum_xy - ((double)sum_ref * (double)sum_img * inv_samples)) *
                         inv_samples_m1;

            ssim_sum += ((2.0 * mu_ref * mu_img + c1) * (2.0 * cov + c2)) /
                        ((mu_ref * mu_ref + mu_img * mu_img + c1) * (var_ref + var_img + c2));
            count++;

            if (x + 1 < width - half_window) {
                int drop_x = x - half_window;
                int add_x = x + half_window + 1;

                sum_ref += (int64_t)col_ref[add_x] - (int64_t)col_ref[drop_x];
                sum_img += (int64_t)col_img[add_x] - (int64_t)col_img[drop_x];
                sum_ref2 += (int64_t)col_ref2[add_x] - (int64_t)col_ref2[drop_x];
                sum_img2 += (int64_t)col_img2[add_x] - (int64_t)col_img2[drop_x];
                sum_xy += (int64_t)col_xy[add_x] - (int64_t)col_xy[drop_x];
            }
        }

        if (y + 1 < height - half_window) {
            int drop_y = y - half_window;
            int add_y = y + half_window + 1;

            for (x = 0; x < width; x++) {
                int drop_idx = ((drop_y * width) + x) * stride;
                int add_idx = ((add_y * width) + x) * stride;
                int32_t drop_ref = ref[drop_idx];
                int32_t drop_img = img[drop_idx];
                int32_t add_ref = ref[add_idx];
                int32_t add_img = img[add_idx];

                col_ref[x] += add_ref - drop_ref;
                col_img[x] += add_img - drop_img;
                col_ref2[x] += (add_ref * add_ref) - (drop_ref * drop_ref);
                col_img2[x] += (add_img * add_img) - (drop_img * drop_img);
                col_xy[x] += (add_ref * add_img) - (drop_ref * drop_img);
            }
        }
    }

    return count > 0 ? (ssim_sum / (double)count) : 1.0;
}

const char *metrics_mode_name(uint8_t mode_type)
{
    switch (mode_type) {
        case EXP_MODE_RAW:  return "RAW";
        case EXP_MODE_JPEG: return "JPEG";
        default:            return "UNKNOWN";
    }
}

float metrics_k_from_idx(uint8_t k_idx)
{
    return exp_k_factor_from_idx(k_idx);
}

const char *metrics_k_label_from_idx(uint8_t k_idx)
{
    return exp_k_label_from_idx(k_idx);
}

float metrics_calc_bpp(uint32_t stream_bytes, int width, int height)
{
    if (width <= 0 || height <= 0) return 0.0f;
    return (float)((double)stream_bytes * 8.0 / (double)(width * height));
}

float metrics_calc_compression_ratio(uint32_t raw_stream_bytes, uint32_t stream_bytes)
{
    if (raw_stream_bytes == 0u || stream_bytes == 0u) return 0.0f;
    return (float)((double)raw_stream_bytes / (double)stream_bytes);
}

double metrics_calc_psnr_rgb888(const uint8_t *ref, const uint8_t *img, int width, int height)
{
    double mse = 0.0;
    size_t total;
    size_t i;

    if (!ref || !img || width <= 0 || height <= 0) return 0.0;

    total = (size_t)width * (size_t)height * 3u;
    for (i = 0; i < total; i++) {
        double diff = (double)ref[i] - (double)img[i];
        mse += diff * diff;
    }

    mse /= (double)total;
    return (mse < 1e-10) ? 100.0 : (10.0 * log10((255.0 * 255.0) / mse));
}

double metrics_calc_ssim_rgb888(const uint8_t *ref, const uint8_t *img, int width, int height)
{
    double ssim_r;
    double ssim_g;
    double ssim_b;

    if (!ref || !img || width <= 0 || height <= 0) return 0.0;

    ssim_r = metrics_calc_ssim_channel(ref + 0, img + 0, width, height, 3);
    ssim_g = metrics_calc_ssim_channel(ref + 1, img + 1, width, height, 3);
    ssim_b = metrics_calc_ssim_channel(ref + 2, img + 2, width, height, 3);
    return (ssim_r + ssim_g + ssim_b) / 3.0;
}

bool metrics_set_reference_sha256_first8(ExperimentRecordResult *r, const uint8_t *reference_rgb888,
                                         size_t reference_len)
{
    uint8_t digest[32];
    size_t sample_len;
    mbedtls_sha256_context ctx;
    int rc = 0;

    if (!r) return false;
    r->sha256_input_first8[0] = '\0';
    if (!reference_rgb888 || reference_len == 0u) return false;

    sample_len = (reference_len < 8u) ? reference_len : 8u;
    mbedtls_sha256_init(&ctx);
    rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0) rc = mbedtls_sha256_update(&ctx, reference_rgb888, sample_len);
    if (rc == 0) rc = mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    if (rc != 0) return false;

    for (size_t i = 0; i < 8u; i++) {
        static const char hex[] = "0123456789abcdef";
        r->sha256_input_first8[i * 2u] = hex[(digest[i] >> 4) & 0x0F];
        r->sha256_input_first8[i * 2u + 1u] = hex[digest[i] & 0x0F];
    }
    r->sha256_input_first8[METRICS_SHA256_FIRST8_HEX_CHARS] = '\0';
    return true;
}

void metrics_fill_timestamp(char *buf, int buf_len)
{
    int64_t boot_us = esp_timer_get_time();

    if (!buf || buf_len <= 0) return;
    snprintf(buf, (size_t)buf_len, "boot+%lld.%03lld s",
             boot_us / 1000000LL, (boot_us % 1000000LL) / 1000LL);
}

void metrics_init_record(ExperimentRecordResult *r, const exp_record_header_t *hdr, uint32_t retry_count)
{
    if (!r || !hdr) return;

    memset(r, 0, sizeof(*r));
    r->present = true;
    r->payload_received = true;
    r->run_id = hdr->run_id;
    r->seq_num = hdr->seq_num;
    r->order_index = hdr->order_index;
    r->record_type = hdr->record_type;
    r->mode_type = hdr->mode_type;
    r->method_id = hdr->method_id;
    r->k_idx = hdr->k_idx;
    r->colorspace = hdr->colorspace;
    r->subsampling = hdr->subsampling;
    r->width = hdr->width;
    r->height = hdr->height;
    r->payload_len = hdr->payload_len;
    r->stream_bytes = hdr->payload_len;
    r->crc32 = hdr->crc32;
    r->retry_count = retry_count;
    r->t_capture_us = hdr->t_capture_us;
    r->t_rgb565_to_rgb888_us = hdr->t_rgb565_to_rgb888_us;
    r->t_prepare_us = hdr->t_prepare_us;
    r->t_algorithm_us = hdr->t_algorithm_us;
    r->t_cam_decompress_us = hdr->t_decompress_us;
    r->t_dct_kernel_us = hdr->t_dct_kernel_us;
    r->dct_kernel_calls = hdr->dct_kernel_calls;
    r->psnr_x100 = hdr->psnr_x100;
    r->frame_bytes = hdr->payload_len + EXP_RECORD_HEADER_SIZE;
    r->bpp = metrics_calc_bpp(r->stream_bytes, r->width, r->height);
    r->compression_ratio_vs_raw = 0.0f;
    r->psnr_db = (hdr->psnr_x100 > 0u) ? ((float)hdr->psnr_x100 / 100.0f) : METRICS_NA_F32;
    r->ssim = METRICS_NA_F32;
    strncpy(r->method_name, exp_method_name(hdr->method_id), sizeof(r->method_name) - 1);
    strncpy(r->mode_name, metrics_mode_name(hdr->mode_type), sizeof(r->mode_name) - 1);
    strncpy(r->status, "RECEIVED", sizeof(r->status) - 1);
    metrics_fill_timestamp(r->timestamp, (int)sizeof(r->timestamp));
}

void metrics_finalize_core_time(ExperimentRecordResult *r)
{
    if (!r) return;

    if (r->mode_type == EXP_MODE_RAW) {
        r->t_total_core_us = r->t_prepare_us + r->t_spi_rx_us + r->t_raw_unpack_us;
    } else {
        r->t_total_core_us = r->t_algorithm_us + r->t_prepare_us + r->t_spi_rx_us +
                             r->t_frame_decode_us + r->t_decompress_us;
    }
}

void metrics_finalize_reference_record(ExperimentRecordResult *r)
{
    if (!r) return;

    r->psnr_db = 0.0f;
    r->ssim = METRICS_NA_F32;
    strncpy(r->status, "REFERENCE", sizeof(r->status) - 1);
    metrics_finalize_core_time(r);
}

void metrics_compute_jpeg_distortion(ExperimentRecordResult *r, const uint8_t *reference_rgb888,
                                     const uint8_t *recon_rgb888)
{
    int64_t t0;
    int64_t t1;

    if (!r || !reference_rgb888 || !recon_rgb888) return;

    t0 = esp_timer_get_time();
    if (r->psnr_db < 0.0f) {
        r->psnr_db = (float)metrics_calc_psnr_rgb888(reference_rgb888, recon_rgb888, r->width, r->height);
    }
    r->ssim = (float)metrics_calc_ssim_rgb888(reference_rgb888, recon_rgb888, r->width, r->height);
    t1 = esp_timer_get_time();
    r->t_metrics_us = (uint32_t)(t1 - t0);
    if (strcmp(r->status, "CRC_FAIL") != 0 && strcmp(r->status, "DECODE_FAIL") != 0) {
        strncpy(r->status, "OK", sizeof(r->status) - 1);
    }
}

void metrics_set_note(ExperimentRecordResult *r, const char *note)
{
    if (!r) return;
    if (!note) {
        r->notes[0] = '\0';
        return;
    }
    strncpy(r->notes, note, sizeof(r->notes) - 1);
    r->notes[sizeof(r->notes) - 1] = '\0';
}

void metrics_append_note(ExperimentRecordResult *r, const char *note)
{
    size_t used;
    size_t left;

    if (!r || !note || note[0] == '\0') return;
    used = strlen(r->notes);
    left = sizeof(r->notes) - used - 1u;
    if (left == 0u) return;

    if (used > 0u && left > 2u) {
        strncat(r->notes, "; ", left);
        used = strlen(r->notes);
        left = sizeof(r->notes) - used - 1u;
    }
    if (left > 0u) {
        strncat(r->notes, note, left);
    }
}

static void metrics_format_value_or_na(float value, const char *fmt, char *buf, size_t buf_len)
{
    if (value < 0.0f) {
        strncpy(buf, "N/A", buf_len - 1u);
        buf[buf_len - 1u] = '\0';
    } else {
        snprintf(buf, buf_len, fmt, (double)value);
    }
}

static void metrics_csv_field_sanitize(const char *src, char *dst, size_t dst_len)
{
    size_t i;

    if (!dst || dst_len == 0u) return;
    dst[0] = '\0';
    if (!src) return;

    for (i = 0; i + 1u < dst_len && src[i] != '\0'; i++) {
        char c = src[i];
        if (c == ',' || c == '\n' || c == '\r') c = ' ';
        dst[i] = c;
    }
    dst[i] = '\0';
}

const char *metrics_summary_csv_header(void)
{
    /* CSV schema note: bpp_estimated was renamed to bpp because this is exact stream bpp. */
    return "image,method,k,psnr,ssim,bpp,compress_us,decompress_us,"
           "dct_kernel_us,idct_kernel_us,dct_kernel_calls,idct_kernel_calls,tx_us,"
           "frame_bytes,tx_raw_us,raw_bytes,compression_ratio,sha256_input_first8,"
           "status,status_detalhado\n";
}

int metrics_format_record_txt(const ExperimentRecordResult *r, char *buf, int buf_len)
{
    char psnr_str[16];
    char ssim_str[16];
    const char *sha_first8;

    metrics_format_value_or_na(r->psnr_db, "%.3f", psnr_str, sizeof(psnr_str));
    metrics_format_value_or_na(r->ssim, "%.4f", ssim_str, sizeof(ssim_str));
    sha_first8 = (r->sha256_input_first8[0] != '\0') ? r->sha256_input_first8 : "N/A";

    return snprintf(buf, (size_t)buf_len,
        "---\n"
        "run_id:               %lu\n"
        "timestamp:            %s\n"
        "mode_type:            %s\n"
        "method:               %s\n"
        "record_type:          %s\n"
        "order_index:          %u\n"
        "seq_num:              %u\n"
        "width:                %u\n"
        "height:               %u\n"
        "colorspace:           %u\n"
        "subsampling:          %u\n"
        "k:                    %s\n"
        "---\n"
        "stream_bytes:         %lu\n"
        "bpp:                  %.4f\n"
        "compression_ratio_vs_raw: %.4f\n"
        "psnr_db:              %s\n"
        "ssim:                 %s\n"
        "sha256_input_first8:  %s\n"
        "---\n"
        "t_capture_us:         %lu\n"
        "t_rgb565_to_rgb888_us:%lu\n"
        "t_raw_pack_us:        %lu\n"
        "t_compress_us:        %lu\n"
        "t_cam_decompress_us:  %lu\n"
        "t_frame_encode_us:    %lu\n"
        "t_spi_rx_us:         %lu\n"
        "t_raw_unpack_us:      %lu\n"
        "t_frame_decode_us:    %lu\n"
        "t_decompress_us:      %lu\n"
        "t_dct_kernel_us:      %lu\n"
        "t_idct_kernel_us:     %lu\n"
        "dct_kernel_calls:     %lu\n"
        "idct_kernel_calls:    %lu\n"
        "t_metrics_us:         %lu\n"
        "t_sd_save_us:         %010lu\n"
        "t_tft_draw_us:        %lu\n"
        "t_total_core_us:      %lu\n"
        "---\n"
        "crc_ok:               %d\n"
        "retry_count:          %lu\n"
        "status:               %s\n"
        "notes:                %s\n"
        "---\n",
        (unsigned long)r->run_id,
        r->timestamp,
        r->mode_name,
        r->method_name,
        r->record_type == EXP_RECORD_RAW_FRAME ? "RAW_FRAME" : "JPEG_FRAME",
        (unsigned)r->order_index,
        (unsigned)r->seq_num,
        (unsigned)r->width,
        (unsigned)r->height,
        (unsigned)r->colorspace,
        (unsigned)r->subsampling,
        metrics_k_label_from_idx(r->k_idx),
        (unsigned long)r->stream_bytes,
        (double)r->bpp,
        (double)r->compression_ratio_vs_raw,
        psnr_str,
        ssim_str,
        sha_first8,
        (unsigned long)r->t_capture_us,
        (unsigned long)r->t_rgb565_to_rgb888_us,
        (unsigned long)(r->mode_type == EXP_MODE_RAW ? r->t_prepare_us : 0u),
        (unsigned long)(r->mode_type == EXP_MODE_JPEG ? r->t_algorithm_us : 0u),
        (unsigned long)(r->mode_type == EXP_MODE_JPEG ? r->t_cam_decompress_us : 0u),
        (unsigned long)(r->mode_type == EXP_MODE_JPEG ? r->t_prepare_us : 0u),
        (unsigned long)r->t_spi_rx_us,
        (unsigned long)r->t_raw_unpack_us,
        (unsigned long)r->t_frame_decode_us,
        (unsigned long)r->t_decompress_us,
        (unsigned long)r->t_dct_kernel_us,
        (unsigned long)r->t_idct_kernel_us,
        (unsigned long)r->dct_kernel_calls,
        (unsigned long)r->idct_kernel_calls,
        (unsigned long)r->t_metrics_us,
        (unsigned long)r->t_sd_save_us,
        (unsigned long)r->t_tft_draw_us,
        (unsigned long)r->t_total_core_us,
        r->crc_ok ? 1 : 0,
        (unsigned long)r->retry_count,
        r->status,
        r->notes);
}

int metrics_format_summary_txt(const ExperimentRunResult *run, char *buf, int buf_len)
{
    int written = 0;
    int i;

    if (!run || !buf || buf_len <= 0) return 0;

    written += snprintf(buf + written, (size_t)(buf_len - written),
                        "run_id: %lu\nreference_method: RAW\nrecords_expected: %u\nrecords:\n",
                        (unsigned long)run->run_id,
                        (unsigned)run->records_expected);

    for (i = 0; i < EXP_RECORD_SLOT_COUNT && written < buf_len; i++) {
        const ExperimentRecordResult *r = &run->records[i];
        char psnr_str[16];

        if (!r->present) continue;
        metrics_format_value_or_na(r->psnr_db, "%.3f", psnr_str, sizeof(psnr_str));

        written += snprintf(buf + written, (size_t)(buf_len - written),
                            "- %s k=%s (%s): bytes=%lu bpp=%.4f psnr=%s "
                            "core_us=%lu dct_us=%lu idct_us=%lu status=%s crc_ok=%d\n",
                            r->method_name,
                            metrics_k_label_from_idx(r->k_idx),
                            r->mode_name,
                            (unsigned long)r->stream_bytes,
                            (double)r->bpp,
                            psnr_str,
                            (unsigned long)r->t_total_core_us,
                            (unsigned long)r->t_dct_kernel_us,
                            (unsigned long)r->t_idct_kernel_us,
                            r->status,
                            r->crc_ok ? 1 : 0);
    }

    return written;
}

int metrics_format_summary_csv_row(const ExperimentRecordResult *r, char *buf, int buf_len)
{
    char psnr_str[16];
    char ssim_str[16];
    char detail[128];
    const char *sha_first8;
    uint32_t raw_bytes;
    uint32_t tx_raw_us;
    uint32_t frame_bytes;
    uint32_t compress_us;
    uint32_t decompress_us;
    uint32_t dct_kernel_us;
    uint32_t idct_kernel_us;
    uint32_t dct_kernel_calls;
    uint32_t idct_kernel_calls;
    uint32_t tx_us;

    metrics_format_value_or_na(r->psnr_db, "%.6f", psnr_str, sizeof(psnr_str));
    metrics_format_value_or_na(r->ssim, "%.6f", ssim_str, sizeof(ssim_str));
    metrics_csv_field_sanitize(r->notes[0] != '\0' ? r->notes : r->status,
                               detail, sizeof(detail));
    sha_first8 = (r->sha256_input_first8[0] != '\0') ? r->sha256_input_first8 : "N/A";

    raw_bytes = (r->raw_bytes > 0u) ? r->raw_bytes :
                ((r->mode_type == EXP_MODE_RAW) ? r->stream_bytes : 0u);
    tx_raw_us = (r->tx_raw_us > 0u) ? r->tx_raw_us :
                ((r->mode_type == EXP_MODE_RAW) ? r->t_spi_rx_us : 0u);
    frame_bytes = (r->frame_bytes > 0u) ? r->frame_bytes :
                  (r->stream_bytes + EXP_RECORD_HEADER_SIZE);
    compress_us = (r->mode_type == EXP_MODE_JPEG) ? r->t_algorithm_us : 0u;
    decompress_us = (r->mode_type == EXP_MODE_JPEG) ? r->t_decompress_us : 0u;
    dct_kernel_us = (r->mode_type == EXP_MODE_JPEG) ? r->t_dct_kernel_us : 0u;
    idct_kernel_us = (r->mode_type == EXP_MODE_JPEG) ? r->t_idct_kernel_us : 0u;
    dct_kernel_calls = (r->mode_type == EXP_MODE_JPEG) ? r->dct_kernel_calls : 0u;
    idct_kernel_calls = (r->mode_type == EXP_MODE_JPEG) ? r->idct_kernel_calls : 0u;
    tx_us = (r->mode_type == EXP_MODE_JPEG) ? r->t_spi_rx_us : 0u;

    return snprintf(buf, (size_t)buf_len,
        "frame_%04lu,%s,%s,%s,%s,%.6f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.6f,%s,%s,%s\n",
        (unsigned long)r->run_id,
        r->method_name,
        metrics_k_label_from_idx(r->k_idx),
        psnr_str,
        ssim_str,
        (double)r->bpp,
        (unsigned long)compress_us,
        (unsigned long)decompress_us,
        (unsigned long)dct_kernel_us,
        (unsigned long)idct_kernel_us,
        (unsigned long)dct_kernel_calls,
        (unsigned long)idct_kernel_calls,
        (unsigned long)tx_us,
        (unsigned long)frame_bytes,
        (unsigned long)tx_raw_us,
        (unsigned long)raw_bytes,
        (double)r->compression_ratio_vs_raw,
        sha_first8,
        r->status,
        detail);
}
