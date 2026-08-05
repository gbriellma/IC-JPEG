/**
 * @file metrics.h
 * @brief Experimental metrics, formatting and per-run result structures.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "experiment_protocol.h"

#define IMG_WIDTH   320
#define IMG_HEIGHT  240
#define IMG_RGB888_BYTES  ((size_t)IMG_WIDTH * (size_t)IMG_HEIGHT * 3u)

#define METRICS_NA_F32  (-1.0f)
#define METRICS_RECORD_TXT_BUF_SIZE   2304
#define METRICS_SUMMARY_TXT_BUF_SIZE  4096
#define METRICS_SD_SAVE_FIELD_PREFIX  "t_sd_save_us:         "
#define METRICS_SD_SAVE_FIELD_WIDTH   10
#define METRICS_SHA256_FIRST8_HEX_CHARS 16

typedef struct {
    bool     present;
    bool     payload_received;
    bool     crc_ok;
    bool     decode_ok;
    bool     saved;
    uint32_t run_id;
    uint8_t  seq_num;
    uint8_t  order_index;
    uint8_t  record_type;
    uint8_t  mode_type;
    uint8_t  method_id;
    uint8_t  k_idx;
    uint8_t  colorspace;
    uint8_t  subsampling;
    uint16_t width;
    uint16_t height;
    uint32_t payload_len;
    uint32_t stream_bytes;
    uint32_t crc32;
    uint32_t retry_count;
    uint32_t t_capture_us;
    uint32_t t_rgb565_to_rgb888_us;
    uint32_t t_prepare_us;
    uint32_t t_algorithm_us;
    uint32_t t_cam_decompress_us;
    uint32_t t_spi_rx_us;
    uint32_t t_raw_unpack_us;
    uint32_t t_frame_decode_us;
    uint32_t t_decompress_us;
    uint32_t t_dct_kernel_us;
    uint32_t t_idct_kernel_us;
    uint32_t t_metrics_us;
    uint32_t t_sd_save_us;
    uint32_t t_tft_draw_us;
    uint32_t t_total_core_us;
    uint32_t dct_kernel_calls;
    uint32_t idct_kernel_calls;
    uint32_t frame_bytes;
    uint32_t raw_bytes;
    uint32_t tx_raw_us;
    uint16_t psnr_x100;
    float    bpp;
    float    compression_ratio_vs_raw;
    float    psnr_db;
    float    ssim;
    char     sha256_input_first8[METRICS_SHA256_FIRST8_HEX_CHARS + 1];
    char     timestamp[32];
    char     method_name[16];
    char     mode_name[8];
    char     status[24];
    char     notes[128];
    char     bmp_name[64];
    char     txt_name[64];
} ExperimentRecordResult;

typedef struct {
    uint32_t run_id;
    uint8_t  k_idx;
    uint8_t  records_expected;
    bool     raw_first;
    uint32_t retry_count;
    bool     has_reference;
    ExperimentRecordResult records[EXP_RECORD_SLOT_COUNT];
    uint8_t *images[EXP_RECORD_SLOT_COUNT]; /* indexed by exp_record_slot_t */
} ExperimentRunResult;

const char *metrics_mode_name(uint8_t mode_type);
float metrics_k_from_idx(uint8_t k_idx);
const char *metrics_k_label_from_idx(uint8_t k_idx);

float metrics_calc_bpp(uint32_t stream_bytes, int width, int height);
float metrics_calc_compression_ratio(uint32_t raw_stream_bytes, uint32_t stream_bytes);
double metrics_calc_psnr_rgb888(const uint8_t *ref, const uint8_t *img, int width, int height);
double metrics_calc_ssim_rgb888(const uint8_t *ref, const uint8_t *img, int width, int height);
bool metrics_init_ssim_workspace(int max_width);
bool metrics_set_reference_sha256_first8(ExperimentRecordResult *r, const uint8_t *reference_rgb888,
                                         size_t reference_len);

void metrics_fill_timestamp(char *buf, int buf_len);
void metrics_init_record(ExperimentRecordResult *r, const exp_record_header_t *hdr, uint32_t retry_count);
void metrics_finalize_core_time(ExperimentRecordResult *r);
void metrics_finalize_reference_record(ExperimentRecordResult *r);
void metrics_compute_jpeg_distortion(ExperimentRecordResult *r, const uint8_t *reference_rgb888,
                                     const uint8_t *recon_rgb888);
void metrics_set_note(ExperimentRecordResult *r, const char *note);
void metrics_append_note(ExperimentRecordResult *r, const char *note);

const char *metrics_summary_csv_header(void);
int metrics_format_record_txt(const ExperimentRecordResult *r, char *buf, int buf_len);
int metrics_format_summary_txt(const ExperimentRunResult *run, char *buf, int buf_len);
int metrics_format_summary_csv_row(const ExperimentRecordResult *r, char *buf, int buf_len);
