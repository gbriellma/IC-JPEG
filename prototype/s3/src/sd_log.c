/**
 * @file sd_log.c
 * @brief SD persistence for multi-record experimental runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "sd_log.h"

static const char *TAG = "sd";

static uint32_t s_counter = 1;

#pragma pack(push, 1)
typedef struct {
    uint8_t  sig[2];
    uint32_t size;
    uint32_t reserved;
    uint32_t off_bits;
} bmp_fh_t;

typedef struct {
    uint32_t size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t image_size;
    int32_t  xppm;
    int32_t  yppm;
    uint32_t clr_used;
    uint32_t clr_important;
} bmp_ih_t;
#pragma pack(pop)

#define BMP_HDR_SZ  (sizeof(bmp_fh_t) + sizeof(bmp_ih_t))

typedef enum {
    SD_MOUNT_STATE_UNMOUNTED = 0,
    SD_MOUNT_STATE_MOUNTING,
    SD_MOUNT_STATE_READY,
    SD_MOUNT_STATE_FAILED,
} sd_mount_state_t;

static volatile sd_mount_state_t s_mount_state = SD_MOUNT_STATE_UNMOUNTED;
static volatile uint8_t s_mount_gen    = 0;
static volatile bool s_mount_task_running = false;

typedef struct { uint8_t gen; } mount_arg_t;

static void counter_load(void)
{
    FILE *f = fopen(SD_MOUNT "/counter.txt", "r");
    if (!f) {
        s_counter = 1;
        return;
    }

    {
        char buf[32] = {0};
        char *end = NULL;
        unsigned long parsed = 0;

        if (!fgets(buf, sizeof(buf), f)) {
            fclose(f);
            ESP_LOGW(TAG, "counter_load: counter.txt vazio/ilegivel; reiniciando em 1");
            s_counter = 1;
            return;
        }

        errno = 0;
        parsed = strtoul(buf, &end, 10);
        while (end && *end != '\0' && isspace((unsigned char)*end)) {
            end++;
        }
        if (end == buf || (end && *end != '\0') || errno == ERANGE || parsed > UINT32_MAX) {
            fclose(f);
            ESP_LOGW(TAG, "counter_load: parse falhou em counter.txt conteudo='%s'; reiniciando em 1",
                     buf);
            s_counter = 1;
            return;
        }
        s_counter = (uint32_t)parsed;
        if (s_counter == 0u) s_counter = 1u;
    }
    fclose(f);
}

static void counter_backup_existing(void)
{
    FILE *src = fopen(SD_MOUNT "/counter.txt", "r");
    FILE *dst;
    char buf[64];
    size_t n;

    dst = fopen(SD_MOUNT "/counter.bak", "w");
    if (!dst) {
        ESP_LOGW(TAG, "counter_save: nao conseguiu abrir counter.bak");
        if (src) fclose(src);
        return;
    }

    if (!src) {
        fprintf(dst, "%04lu\n", (unsigned long)s_counter);
        fflush(dst);
        fclose(dst);
        return;
    }

    while ((n = fread(buf, 1, sizeof(buf), src)) > 0u) {
        if (fwrite(buf, 1, n, dst) != n) {
            ESP_LOGW(TAG, "counter_save: falha escrevendo counter.bak");
            break;
        }
    }
    fflush(dst);
    fclose(src);
    fclose(dst);
}

static void counter_save(void)
{
    counter_backup_existing();
    FILE *f = fopen(SD_MOUNT "/counter.txt", "w");
    if (!f) {
        ESP_LOGE(TAG, "counter_save: nao conseguiu abrir counter.txt para escrita");
        return;
    }
    fprintf(f, "%04lu\n", (unsigned long)s_counter);
    fflush(f);
    fclose(f);
}

static bool write_bmp(const char *path, const uint8_t *rgb888, int width, int height)
{
    int row_stride = width * 3;
    int pad = (4 - (row_stride & 3)) & 3;
    uint32_t pixel_bytes = (uint32_t)(row_stride + pad) * (uint32_t)height;
    uint32_t total_size = (uint32_t)BMP_HDR_SZ + pixel_bytes;
    const uint8_t zero[4] = {0, 0, 0, 0};
    const uint8_t *src = rgb888;
    int row;
    int col;
    FILE *f;

    if (!path || !rgb888 || width <= 0 || height <= 0) return false;

    f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    {
        bmp_fh_t fh = { {'B', 'M'}, total_size, 0u, (uint32_t)BMP_HDR_SZ };
        bmp_ih_t ih = {
            .size = 40u,
            .width = width,
            .height = -height,
            .planes = 1u,
            .bit_count = 24u,
            .compression = 0u,
            .image_size = pixel_bytes,
            .xppm = 2835,
            .yppm = 2835,
            .clr_used = 0u,
            .clr_important = 0u,
        };
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
    }

    for (row = 0; row < height; row++) {
        for (col = 0; col < width; col++) {
            uint8_t bgr[3] = { src[2], src[1], src[0] };
            fwrite(bgr, 3, 1, f);
            src += 3;
        }
        if (pad) fwrite(zero, pad, 1, f);
    }

    fclose(f);
    return true;
}

static bool run_uses_directory(const ExperimentRunResult *run)
{
    return run && run->records_expected == exp_records_expected_for_method(EXP_METHOD_ALL);
}

static bool ensure_run_directory(const ExperimentRunResult *run, char *run_dir, size_t run_dir_len)
{
    int needed;

    if (!run_dir || run_dir_len == 0u) return false;
    run_dir[0] = '\0';
    if (!run_uses_directory(run)) return true;

    needed = snprintf(run_dir, run_dir_len, SD_MOUNT "/run%04lu",
                      (unsigned long)run->run_id);
    if (needed < 0 || (size_t)needed >= run_dir_len) {
        ESP_LOGE(TAG, "run dir path too long for run %lu", (unsigned long)run->run_id);
        run_dir[0] = '\0';
        return false;
    }

    if (mkdir(run_dir, 0775) != 0) {
        if (errno == EEXIST) {
            /* sd_reserve_run_id should have already skipped occupied IDs.
             * Reaching here means an unexpected collision - refuse to
             * overwrite existing data rather than silently clobbering it. */
            ESP_LOGE(TAG, "run directory %s already exists; recusando sobrescrita de dados existentes",
                     run_dir);
        } else {
            ESP_LOGE(TAG, "Cannot create run directory %s: errno=%d", run_dir, errno);
        }
        run_dir[0] = '\0';
        return false;
    }

    ESP_LOGI(TAG, "ALL run artifacts directory: %s", run_dir);
    return true;
}

static bool make_artifact_path(char *buf, size_t buf_len, const char *run_dir,
                               const char *file_name)
{
    int needed;

    if (!buf || buf_len == 0u || !file_name) return false;
    if (run_dir && run_dir[0] != '\0') {
        needed = snprintf(buf, buf_len, "%s/%s", run_dir, file_name);
    } else {
        needed = snprintf(buf, buf_len, SD_MOUNT "/%s", file_name);
    }
    return needed >= 0 && (size_t)needed < buf_len;
}

static bool make_run_path(char *buf, size_t buf_len, const char *run_dir,
                          uint32_t run_id, const char *suffix)
{
    char file_name[64];
    int needed;

    needed = snprintf(file_name, sizeof(file_name), "run%04lu_%s",
                      (unsigned long)run_id, suffix);
    if (needed < 0 || (size_t)needed >= sizeof(file_name)) return false;
    return make_artifact_path(buf, buf_len, run_dir, file_name);
}

static const char *record_suffix(uint8_t method_id)
{
    switch (method_id) {
        case EXP_METHOD_RAW:      return "raw";
        case EXP_METHOD_LOEFFLER: return "loeffler";
        case EXP_METHOD_MATRIX:   return "matrix";
        case EXP_METHOD_RDCT:        return "rdct";
        case EXP_METHOD_SILVEIRA_J3: return "silveira_j3";
        case EXP_METHOD_SILVEIRA_J7: return "silveira_j7";
        default:                  return "unknown";
    }
}

static bool write_text_file(const char *path, const char *text)
{
    FILE *f;
    size_t want;

    if (!path || !text) return false;

    f = fopen(path, "w");
    if (!f) return false;

    want = strlen(text);
    if (want > 0u && fwrite(text, 1, want, f) != want) {
        fclose(f);
        return false;
    }
    fflush(f);
    fclose(f);
    return true;
}

static bool patch_sd_save_time_field(const char *path, long offset, uint32_t value)
{
    FILE *f;
    char digits[METRICS_SD_SAVE_FIELD_WIDTH + 1];

    if (!path || offset < 0) return false;

    snprintf(digits, sizeof(digits), "%010lu", (unsigned long)value);
    f = fopen(path, "r+");
    if (!f) return false;
    if (fseek(f, offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    if (fwrite(digits, 1, METRICS_SD_SAVE_FIELD_WIDTH, f) != METRICS_SD_SAVE_FIELD_WIDTH) {
        fclose(f);
        return false;
    }
    fflush(f);
    fclose(f);
    return true;
}

static bool status_invalid_for_global_summary(const char *status)
{
    static const char *invalid_prefixes[] = {
        "CRC_FAIL",
        "DECODE_FAIL",
        "DECOMP_FAIL",
        "JPEG_SIZE_FAIL",
        "RAW_SIZE_FAIL",
        "OOM",
        "SD_FAIL",
        "REF_MISSING",
        "MODE_FAIL",
    };

    if (!status) return false;
    for (size_t i = 0; i < sizeof(invalid_prefixes) / sizeof(invalid_prefixes[0]); i++) {
        size_t n = strlen(invalid_prefixes[i]);
        if (strncmp(status, invalid_prefixes[i], n) == 0) return true;
    }
    return false;
}

static bool append_summary_csv(const ExperimentRunResult *run)
{
    FILE *f;
    int i;
    int skipped_invalid = 0;

    if (!run) return false;
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        const ExperimentRecordResult *r = &run->records[i];
        if (r->present && status_invalid_for_global_summary(r->status)) {
            skipped_invalid++;
        }
    }
    ESP_LOGI(TAG, "summary.csv global: pulando %d registro(s) com status invalido",
             skipped_invalid);

    f = fopen(SD_MOUNT "/summary.csv", "a+");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open summary.csv");
        return false;
    }

    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
        fputs(metrics_summary_csv_header(), f);
    }

    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        const ExperimentRecordResult *r = &run->records[i];
        char row[768];

        if (!r->present) continue;
        if (status_invalid_for_global_summary(r->status)) continue;
        metrics_format_summary_csv_row(r, row, (int)sizeof(row));
        fputs(row, f);
    }

    fflush(f);
    fclose(f);
    return true;
}

static bool save_record_artifacts(uint32_t run_id, const char *run_dir,
                                  ExperimentRecordResult *r, const uint8_t *rgb888)
{
    const char *suffix;
    char suffix_with_k[40];
    char bmp_file_name[64];
    char txt_file_name[64];
    char bmp_path[96];
    char txt_path[96];
    char txt_buf[METRICS_RECORD_TXT_BUF_SIZE];
    int64_t t0;
    int64_t t1;
    int needed;
    char *save_field = NULL;
    long save_field_offset = -1;

    if (!r || !rgb888) return false;

    suffix = record_suffix(r->method_id);
    if (r->mode_type == EXP_MODE_JPEG) {
        snprintf(suffix_with_k, sizeof(suffix_with_k), "%s_k%s",
                 suffix, metrics_k_label_from_idx(r->k_idx));
        for (char *p = suffix_with_k; *p != '\0'; p++) {
            if (*p == '.') *p = 'p';
        }
        suffix = suffix_with_k;
    }
    snprintf(bmp_file_name, sizeof(bmp_file_name), "run%04lu_%s.bmp",
             (unsigned long)run_id, suffix);
    snprintf(txt_file_name, sizeof(txt_file_name), "run%04lu_%s.txt",
             (unsigned long)run_id, suffix);
    snprintf(r->bmp_name, sizeof(r->bmp_name), "%s", bmp_file_name);
    snprintf(r->txt_name, sizeof(r->txt_name), "%s", txt_file_name);
    if (!make_artifact_path(bmp_path, sizeof(bmp_path), run_dir, bmp_file_name) ||
        !make_artifact_path(txt_path, sizeof(txt_path), run_dir, txt_file_name)) {
        metrics_append_note(r, "artifact path too long");
        strncpy(r->status, "SD_FAIL", sizeof(r->status) - 1);
        return false;
    }

    t0 = esp_timer_get_time();
    if (!write_bmp(bmp_path, rgb888, r->width, r->height)) {
        metrics_append_note(r, "bmp save failed");
        strncpy(r->status, "SD_FAIL", sizeof(r->status) - 1);
        return false;
    }

    needed = metrics_format_record_txt(r, txt_buf, (int)sizeof(txt_buf));
    if (needed < 0 || needed >= (int)sizeof(txt_buf)) {
        metrics_append_note(r, "txt buffer too small");
        strncpy(r->status, "SD_FAIL", sizeof(r->status) - 1);
        return false;
    }
    save_field = strstr(txt_buf, METRICS_SD_SAVE_FIELD_PREFIX);
    if (!save_field) {
        metrics_append_note(r, "sd save field missing");
        strncpy(r->status, "SD_FAIL", sizeof(r->status) - 1);
        return false;
    }
    save_field_offset = (long)(save_field - txt_buf) + (long)strlen(METRICS_SD_SAVE_FIELD_PREFIX);
    if (!write_text_file(txt_path, txt_buf)) {
        metrics_append_note(r, "txt save failed");
        strncpy(r->status, "SD_FAIL", sizeof(r->status) - 1);
        return false;
    }
    t1 = esp_timer_get_time();
    r->t_sd_save_us = (uint32_t)(t1 - t0);
    if (!patch_sd_save_time_field(txt_path, save_field_offset, r->t_sd_save_us)) {
        metrics_append_note(r, "txt patch failed");
        strncpy(r->status, "SD_FAIL", sizeof(r->status) - 1);
        return false;
    }
    r->saved = true;
    return true;
}

static bool save_reference_artifacts(const ExperimentRunResult *run, const char *run_dir)
{
    char bmp_path[96];
    char txt_path[96];
    char txt_buf[METRICS_RECORD_TXT_BUF_SIZE];
    ExperimentRecordResult ref_record;
    const int raw_slot = exp_method_to_slot(EXP_METHOD_RAW);
    const ExperimentRecordResult *raw = &run->records[raw_slot];
    const uint8_t *ref_rgb888 = run->images[raw_slot];
    int needed;

    if (!run->has_reference || !raw->present || !ref_rgb888) return false;

    ref_record = *raw;
    strncpy(ref_record.status, "REFERENCE", sizeof(ref_record.status) - 1);
    metrics_set_note(&ref_record, "reference image derived from RAW record");

    if (!make_run_path(bmp_path, sizeof(bmp_path), run_dir, run->run_id, "ref.bmp") ||
        !make_run_path(txt_path, sizeof(txt_path), run_dir, run->run_id, "ref.txt")) {
        return false;
    }
    if (!write_bmp(bmp_path, ref_rgb888, raw->width, raw->height)) {
        return false;
    }

    needed = metrics_format_record_txt(&ref_record, txt_buf, (int)sizeof(txt_buf));
    if (needed < 0 || needed >= (int)sizeof(txt_buf)) return false;
    if (!write_text_file(txt_path, txt_buf)) return false;

    return true;
}

static bool save_run_summary(const ExperimentRunResult *run, const char *run_dir)
{
    char txt_path[96];
    char csv_path[96];
    char txt_buf[METRICS_SUMMARY_TXT_BUF_SIZE];
    FILE *csv;
    int i;
    int needed;

    if (!make_run_path(txt_path, sizeof(txt_path), run_dir, run->run_id, "summary.txt") ||
        !make_run_path(csv_path, sizeof(csv_path), run_dir, run->run_id, "summary.csv")) {
        return false;
    }

    needed = metrics_format_summary_txt(run, txt_buf, (int)sizeof(txt_buf));
    if (needed < 0 || needed >= (int)sizeof(txt_buf)) return false;
    if (!write_text_file(txt_path, txt_buf)) return false;

    csv = fopen(csv_path, "w");
    if (!csv) return false;
    fputs(metrics_summary_csv_header(), csv);
    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        const ExperimentRecordResult *r = &run->records[i];
        char row[768];

        if (!r->present) continue;
        metrics_format_summary_csv_row(r, row, (int)sizeof(row));
        fputs(row, csv);
    }
    fflush(csv);
    fclose(csv);
    return true;
}

static void sd_mount_task(void *arg)
{
    uint8_t my_gen = ((mount_arg_t *)arg)->gen;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t ret;

    free(arg);

    host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.command_timeout_ms = 200;
    slot.width = 1;
    slot.clk = SD_CLK_PIN;
    slot.cmd = SD_CMD_PIN;
    slot.d0 = SD_D0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "mount: iniciando SDMMC 1-bit clk=%d cmd=%d d0=%d freq=%u kHz",
             slot.clk, slot.cmd, slot.d0, (unsigned)host.max_freq_khz);
    ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount_cfg, &card);
    if (my_gen != s_mount_gen) {
        if (ret == ESP_OK) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
        }
        s_mount_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (ret == ESP_OK) {
        s_mount_state = SD_MOUNT_STATE_READY;
        ESP_LOGI(TAG, "mount: SD montado em %s", SD_MOUNT);
        sdmmc_card_print_info(stdout, card);
        counter_load();
    } else {
        s_mount_state = SD_MOUNT_STATE_FAILED;
        ESP_LOGE(TAG, "mount: falhou em %s: %s (0x%x)",
                 SD_MOUNT, esp_err_to_name(ret), (unsigned)ret);
    }
    s_mount_task_running = false;
    vTaskDelete(NULL);
}

bool sd_is_mounted(void)
{
    return s_mount_state == SD_MOUNT_STATE_READY;
}

bool sd_init(bool (*cancel_fn)(void))
{
    mount_arg_t *marg;
    BaseType_t created;
    int i;

    if (s_mount_state == SD_MOUNT_STATE_READY) return true;
    if (s_mount_task_running) {
        ESP_LOGW(TAG, "sd_init: montagem anterior ainda em execucao");
        return false;
    }

    s_mount_gen++;
    s_mount_state = SD_MOUNT_STATE_MOUNTING;
    s_mount_task_running = true;

    marg = (mount_arg_t *)malloc(sizeof(*marg));
    if (!marg) {
        s_mount_task_running = false;
        s_mount_state = SD_MOUNT_STATE_UNMOUNTED;
        return false;
    }
    marg->gen = s_mount_gen;

    created = xTaskCreatePinnedToCore(sd_mount_task, "sd_mnt", 4096, marg, 5, NULL, 1);
    if (created != pdPASS) {
        free(marg);
        s_mount_task_running = false;
        s_mount_state = SD_MOUNT_STATE_UNMOUNTED;
        return false;
    }

    for (i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_mount_state == SD_MOUNT_STATE_READY) return true;
        if (s_mount_state == SD_MOUNT_STATE_FAILED) return false;
        if (cancel_fn && cancel_fn()) {
            s_mount_gen++;
            s_mount_state = SD_MOUNT_STATE_UNMOUNTED;
            return false;
        }
    }

    ESP_LOGE(TAG, "sd_init: timeout aguardando montagem em %s", SD_MOUNT);
    s_mount_gen++;
    s_mount_state = SD_MOUNT_STATE_UNMOUNTED;
    return false;
}

uint32_t sd_reserve_run_id(void)
{
    uint32_t run_id = s_counter;
    char dir[64];
    struct stat st;
    unsigned int skipped = 0;

    if (run_id == 0u) run_id = 1u;

    /* Skip IDs whose run directory already exists on the SD card.
     * Protects existing ALL-mode data when counter.txt is missing or stale
     * (e.g. after SD swap, power loss, or manual reset of the counter). */
    while (skipped < 9999u) {
        snprintf(dir, sizeof(dir), SD_MOUNT "/run%04lu", (unsigned long)run_id);
        if (stat(dir, &st) != 0) break;   /* directory absent - safe to use */
        ESP_LOGW(TAG, "sd_reserve_run_id: run%04lu ja existe, avancando",
                 (unsigned long)run_id);
        run_id++;
        if (run_id == 0u) run_id = 1u;
        skipped++;
    }
    if (skipped > 0u) {
        ESP_LOGW(TAG, "sd_reserve_run_id: pulou %u ID(s) com diretorio existente; usando %lu",
                 skipped, (unsigned long)run_id);
    }

    s_counter = run_id + 1u;
    counter_save();
    return run_id;
}

bool sd_save_experiment_run(ExperimentRunResult *run)
{
    char run_dir[64];
    int i;

    if (s_mount_state != SD_MOUNT_STATE_READY || !run) return false;

    if (!ensure_run_directory(run, run_dir, sizeof(run_dir))) {
        return false;
    }

    if (!save_reference_artifacts(run, run_dir)) {
        ESP_LOGW(TAG, "Reference artifacts not saved for run %lu", (unsigned long)run->run_id);
    }

    for (i = 0; i < EXP_RECORD_SLOT_COUNT; i++) {
        ExperimentRecordResult *r = &run->records[i];
        const uint8_t *img = run->images[i];

        if (!r->present || !img) continue;
        if (!save_record_artifacts(run->run_id, run_dir, r, img)) {
            ESP_LOGW(TAG, "Record artifacts not fully saved: %s", r->method_name);
        }
    }

    if (!save_run_summary(run, run_dir)) {
        ESP_LOGE(TAG, "Failed to save per-run summary for %lu", (unsigned long)run->run_id);
        return false;
    }

    if (!append_summary_csv(run)) {
        ESP_LOGE(TAG, "Failed to append global summary.csv");
        return false;
    }

    return true;
}
