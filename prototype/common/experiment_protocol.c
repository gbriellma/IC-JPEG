#include "experiment_protocol.h"

static uint32_t s_crc32_table[256];
static bool s_crc32_table_ready = false;
static uint16_t s_crc16_table[256];
static bool s_crc16_table_ready = false;

static void exp_crc32_init_table(void)
{
    uint32_t i;

    if (s_crc32_table_ready) return;

    for (i = 0; i < 256u; i++) {
        uint32_t crc = i;
        int bit;

        for (bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
        s_crc32_table[i] = crc;
    }

    s_crc32_table_ready = true;
}

uint32_t exp_crc32_payload(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    if (!data && len != 0u) return 0u;

    exp_crc32_init_table();

    for (i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)((crc ^ data[i]) & 0xFFu);
        crc = (crc >> 8) ^ s_crc32_table[idx];
    }

    return ~crc;
}

static void exp_crc16_init_table(void)
{
    uint32_t i;

    if (s_crc16_table_ready) return;

    for (i = 0; i < 256u; i++) {
        uint16_t crc = (uint16_t)(i << 8);
        int bit;

        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
        s_crc16_table[i] = crc;
    }

    s_crc16_table_ready = true;
}

uint16_t exp_crc16_block(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;

    if (!data && len != 0u) return 0u;

    exp_crc16_init_table();

    for (i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(((crc >> 8) ^ data[i]) & 0xFFu);
        crc = (uint16_t)((crc << 8) ^ s_crc16_table[idx]);
    }

    return crc;
}
