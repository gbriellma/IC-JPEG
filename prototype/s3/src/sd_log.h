/**
 * @file sd_log.h
 * @brief SD card persistence for full RAW + JPEG experiment runs.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "metrics.h"

#define SD_CLK_PIN  39
#define SD_CMD_PIN  38
#define SD_D0_PIN   40

#define SD_MOUNT    "/sdcard"

bool sd_init(bool (*cancel_fn)(void));
bool sd_is_mounted(void);

uint32_t sd_reserve_run_id(void);
bool sd_save_experiment_run(ExperimentRunResult *run);
