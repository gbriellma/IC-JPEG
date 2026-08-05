#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpeg_codec.h"

#define EXP_READY_MAGIC_0     0xC3
#define EXP_READY_MAGIC_1     0x5A
#define EXP_CTRL_MAGIC        0xCF
#define EXP_HANDSHAKE_BYTE    0xAA

#define EXP_CTRL_VERSION      1
#define EXP_RECORD_VERSION    5
#define EXP_RECORD_MAGIC      0x314a4349u  /* "ICJ1" little-endian */
#define EXP_UART_BAUD         115200u
#ifndef EXP_SPI_FREQ_HZ
/* Diagnostico do stream SPI: comece em 1 MHz e suba para 2/5/10 MHz
 * somente depois de confirmar payload completo sem timeout. */
#define EXP_SPI_FREQ_HZ       1000000u
#endif
#ifndef EXP_SPI_MODE
/* ESP32 SPI slave entrega MISO estavel para este link em CPHA=1.
 * Se o master ler status como A0 A6 04 02, isso e 50 53 02 01
 * deslocado 1 bit e indica modo/CPHA errado. */
#define EXP_SPI_MODE          1u
#endif
#define EXP_SPI_CAM_READY_PIN 2
#define EXP_SPI_S3_READY_PIN  9

#define EXP_SPI_VERSION       3u
#define EXP_SPI_MAGIC         0x5350u  /* "PS" little-endian */

#define EXP_SPI_CMD_NOP       0u
#define EXP_SPI_CMD_SET_CONFIG 1u
#define EXP_SPI_CMD_TRIGGER   2u
#define EXP_SPI_CMD_ABORT     3u
#define EXP_SPI_CMD_READ_HEADER 4u
#define EXP_SPI_CMD_READ_PAYLOAD 5u
#define EXP_SPI_CMD_READ_TELEMETRY 6u

#define EXP_SPI_FLAG_RAW_FIRST  (1u << 0)

#define EXP_SPI_STATE_BOOTING      0u
#define EXP_SPI_STATE_IDLE         1u
#define EXP_SPI_STATE_CAPTURING    2u
#define EXP_SPI_STATE_RECORD_READY 3u
#define EXP_SPI_STATE_DONE         4u
#define EXP_SPI_STATE_ERROR        5u

#define EXP_SPI_ERROR_NONE             0u
#define EXP_SPI_ERROR_BUSY             1u
#define EXP_SPI_ERROR_BAD_CONFIG       2u
#define EXP_SPI_ERROR_CAPTURE_FAIL     3u
#define EXP_SPI_ERROR_ENCODE_FAIL      4u
#define EXP_SPI_ERROR_ABORTED          5u
#define EXP_SPI_ERROR_PROTO            6u
#define EXP_SPI_ERROR_CAM_OVF          7u

#define EXP_SPI_CTRL_BODY_SIZE     16u
#define EXP_SPI_STATUS_BODY_SIZE   16u
#define EXP_SPI_CTRL_FRAME_SIZE    18u
#define EXP_SPI_STATUS_FRAME_SIZE  18u
#define EXP_SPI_GUARD_BYTES        1u
#define EXP_SPI_CTRL_WIRE_SIZE     (EXP_SPI_CTRL_FRAME_SIZE + EXP_SPI_GUARD_BYTES)
#define EXP_SPI_STATUS_WIRE_SIZE   (EXP_SPI_STATUS_FRAME_SIZE + EXP_SPI_GUARD_BYTES)
/* Chunk conservador enquanto o link SPI CAM<->S3 nao tem ACK por bloco.
 * Reduz a janela para CS glitches/transacoes curtas em payload grande. */
#define EXP_SPI_PAYLOAD_CHUNK      512u
#define EXP_SPI_DMA_ALIGNED_SIZE(n) (((n) + 3u) & ~3u)

#define EXP_FRAME_MAGIC            JPEG_FRAME_MAGIC
#define EXP_RAW_FRAME_MAGIC        0xA6u
#define EXP_EMBEDDED_FRAME_HEADER_SIZE 21u
#define EXP_RAW_FRAME_HEADER_SIZE      10u

#define EXP_LINK_TYPE_HEADER  1u
#define EXP_LINK_TYPE_DATA    2u
#define EXP_LINK_TYPE_ACK     3u

#define EXP_LINK_ACK_OK       0x06u
#define EXP_LINK_ACK_NACK     0x15u

#define EXP_LINK_MAX_CHUNK          256u
#define EXP_LINK_DELIMITER          0x00u
#define EXP_LINK_SEQ_SIZE           2u
#define EXP_LINK_LEN_SIZE           2u
#define EXP_LINK_CRC16_SIZE         2u
#define EXP_LINK_STATUS_SIZE        1u
#define EXP_LINK_RAW_HEADER_SIZE    (1u + EXP_LINK_SEQ_SIZE + EXP_LINK_LEN_SIZE)
#define EXP_LINK_RAW_OVERHEAD       (EXP_LINK_RAW_HEADER_SIZE + EXP_LINK_CRC16_SIZE)
#define EXP_LINK_RAW_MAX_SIZE       (EXP_LINK_RAW_OVERHEAD + EXP_LINK_MAX_CHUNK)
#define EXP_LINK_COBS_MAX_SIZE      JPEG_COBS_MAX_ENCODED_LEN(EXP_LINK_RAW_MAX_SIZE)
#define EXP_LINK_MAX_RETRIES        3u

#define EXP_CTRL_FLAG_RAW_FIRST   (1u << 0)

#define EXP_RECORD_FLAG_CRC_PAYLOAD   (1u << 0)

#define EXP_RECORD_HEADER_SIZE    24
/* Fixed prototype resolution - not transmitted in the 24-byte wire header */
#define EXP_IMG_WIDTH   320u
#define EXP_IMG_HEIGHT  240u

/* Worst-case RLE payload for QVGA 4:4:4:
 *   3600 blocks × 191 bytes/block = 687 600 bytes
 *   (DC=2 + 63 ACs × 3 bytes, all non-zero - pathological noisy image)
 * Used to size s_frame_buf on CAM and rx_payload_buf on S3. */
#define EXP_JPEG_BLOCKS_PER_CHANNEL \
    (((EXP_IMG_WIDTH + 7u) / 8u) * ((EXP_IMG_HEIGHT + 7u) / 8u))
#define EXP_JPEG_RLE_WORST_BLOCK_BYTES  (2u + 63u * 3u)
#define EXP_JPEG_RLE_WORST_CASE_BYTES \
    ((size_t)EXP_JPEG_BLOCKS_PER_CHANNEL * 3u * EXP_JPEG_RLE_WORST_BLOCK_BYTES)
#define EXP_RECORD_TELEMETRY_MAGIC 0x544du  /* "MT" little-endian */
#define EXP_RECORD_TELEMETRY_VERSION 1u
#define EXP_RECORD_TELEMETRY_SIZE 36u
#define EXP_RECORD_TELEMETRY_BODY_SIZE 32u
#define EXP_CONTROL_PACKET_SIZE   9

typedef enum {
    EXP_RECORD_RAW_FRAME  = 1,
    EXP_RECORD_JPEG_FRAME = 2,
} exp_record_type_t;

typedef enum {
    EXP_MODE_RAW  = 1,
    EXP_MODE_JPEG = 2,
} exp_mode_type_t;

typedef enum {
    EXP_METHOD_RAW      = 0,
    EXP_METHOD_LOEFFLER = 1,
    EXP_METHOD_MATRIX   = 2,
    EXP_METHOD_RDCT     = 3,
    EXP_METHOD_SILVEIRA_J3 = 4,
    EXP_METHOD_SILVEIRA_J7 = 5,
    EXP_METHOD_ALL      = 6,
} exp_method_id_t;

typedef enum {
    EXP_SEQ_RAW         = 0,
    EXP_SEQ_LOEFFLER    = 1,
    EXP_SEQ_MATRIX      = 2,
    EXP_SEQ_RDCT        = 3,
    EXP_SEQ_SILVEIRA_J3 = 4,
    EXP_SEQ_SILVEIRA_J7 = 5,
} exp_sequence_id_t;

static inline uint8_t exp_method_to_seq(uint8_t method_id)
{
    switch (method_id) {
        case EXP_METHOD_RAW:         return EXP_SEQ_RAW;
        case EXP_METHOD_MATRIX:      return EXP_SEQ_MATRIX;
        case EXP_METHOD_RDCT:        return EXP_SEQ_RDCT;
        case EXP_METHOD_SILVEIRA_J3: return EXP_SEQ_SILVEIRA_J3;
        case EXP_METHOD_SILVEIRA_J7: return EXP_SEQ_SILVEIRA_J7;
        case EXP_METHOD_ALL:
            /* ALL is handled by the outer iteration loop in cam/main.c;
             * this function is never called with ALL in normal flow.
             * Fall through to LOEFFLER as a safe sentinel. */
        case EXP_METHOD_LOEFFLER:
        default:                     return EXP_SEQ_LOEFFLER;
    }
}

#define EXP_K_FACTOR_COUNT 4u
#define EXP_JPEG_METHOD_COUNT 5u

typedef enum {
    EXP_RECORD_SLOT_RAW = 0,
    EXP_RECORD_SLOT_LOEFFLER = 1,
    EXP_RECORD_SLOT_MATRIX = 2,
    EXP_RECORD_SLOT_RDCT = 3,
    EXP_RECORD_SLOT_SILVEIRA_J3 = 4,
    EXP_RECORD_SLOT_SILVEIRA_J7 = 5,
} exp_record_slot_t;

#define EXP_RECORD_SLOT_JPEG_FIRST 1u
#define EXP_RECORD_SLOT_LEGACY_COUNT 6u
#define EXP_RECORD_SLOT_COUNT (EXP_RECORD_SLOT_JPEG_FIRST + \
                               (EXP_JPEG_METHOD_COUNT * EXP_K_FACTOR_COUNT))
#define EXP_RECORDS_PER_RUN EXP_RECORD_SLOT_COUNT

#define EXP_SUBSAMPLING_NONE  0xFFu

typedef struct {
    uint8_t  version;
    uint8_t  method_id;
    uint8_t  k_idx;
    uint8_t  flags;
    uint32_t run_id;
} exp_control_packet_t;

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  cmd;
    uint8_t  flags;
    uint8_t  method_id;
    uint8_t  k_idx;
    uint8_t  reserved0;
    uint32_t run_id;
    uint32_t arg0;
    uint16_t crc16;
} exp_spi_ctrl_t;

typedef struct {
    uint16_t magic;
    uint8_t  version;
    uint8_t  state;
    uint8_t  error;
    uint8_t  records_expected;
    uint8_t  current_order;
    uint8_t  reserved0;
    uint32_t run_id;
    uint32_t payload_len;
    uint16_t crc16;
} exp_spi_status_t;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  header_size;
    uint8_t  record_type;
    uint8_t  flags;
    uint32_t run_id;
    uint8_t  seq_num;
    uint8_t  order_index;
    uint8_t  method_id;
    uint8_t  mode_type;
    uint16_t width;
    uint16_t height;
    uint8_t  colorspace;
    uint8_t  subsampling;
    uint8_t  k_idx;
    uint8_t  records_expected;
    uint32_t payload_len;
    uint32_t t_capture_us;
    uint32_t t_rgb565_to_rgb888_us;
    uint32_t t_prepare_us;
    uint32_t t_algorithm_us;
    uint32_t t_decompress_us;
    uint16_t psnr_x100;
    uint16_t reserved0;
    uint32_t t_dct_kernel_us;
    uint16_t dct_kernel_calls;
    uint16_t reserved1;
    uint32_t crc32;
} exp_record_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint32_t payload_len;
    uint8_t  dct_method;
    uint8_t  k_fixed8;
    uint16_t width;
    uint16_t height;
    uint32_t compress_us;
    uint32_t decompress_us;
    uint16_t psnr_x100;
} embedded_frame_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint16_t width;
    uint16_t height;
    uint8_t  channels;
    uint32_t total_bytes;
} raw_frame_header_t;

uint32_t exp_crc32_payload(const uint8_t *data, size_t len);
uint16_t exp_crc16_block(const uint8_t *data, size_t len);

static inline void exp_put_u16le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static inline void exp_put_u32le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static inline uint16_t exp_get_u16le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static inline uint32_t exp_get_u32le(const uint8_t *src)
{
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static inline void exp_control_packet_encode(const exp_control_packet_t *pkt,
                                             uint8_t out[EXP_CONTROL_PACKET_SIZE])
{
    out[0] = EXP_CTRL_MAGIC;
    out[1] = pkt->version;
    out[2] = pkt->method_id;
    out[3] = pkt->k_idx;
    out[4] = pkt->flags;
    exp_put_u32le(out + 5, pkt->run_id);
}

static inline bool exp_control_packet_decode(const uint8_t in[EXP_CONTROL_PACKET_SIZE],
                                             exp_control_packet_t *pkt)
{
    if (!in || !pkt) return false;
    if (in[0] != EXP_CTRL_MAGIC) return false;
    if (in[1] != EXP_CTRL_VERSION) return false;

    pkt->version = in[1];
    pkt->method_id = in[2];
    pkt->k_idx = in[3];
    pkt->flags = in[4];
    pkt->run_id = exp_get_u32le(in + 5);
    return true;
}

static inline void exp_record_header_encode(const exp_record_header_t *hdr,
                                            uint8_t out[EXP_RECORD_HEADER_SIZE])
{
    exp_put_u32le(out + 0, hdr->magic);
    out[4] = hdr->version;
    out[5] = hdr->header_size;
    out[6] = hdr->record_type;
    out[7] = hdr->flags;
    exp_put_u32le(out + 8, hdr->run_id);
    out[12] = hdr->order_index;
    out[13] = hdr->method_id;
    out[14] = hdr->mode_type;
    out[15] = hdr->k_idx;
    exp_put_u32le(out + 16, hdr->payload_len);
    exp_put_u32le(out + 20, hdr->crc32);
}

static inline bool exp_record_header_decode(const uint8_t in[EXP_RECORD_HEADER_SIZE],
                                            exp_record_header_t *hdr)
{
    if (!in || !hdr) return false;

    *hdr = (exp_record_header_t){0};
    hdr->magic = exp_get_u32le(in + 0);
    hdr->version = in[4];
    hdr->header_size = in[5];
    hdr->record_type = in[6];
    hdr->flags = in[7];
    hdr->run_id = exp_get_u32le(in + 8);
    hdr->order_index = in[12];
    hdr->method_id = in[13];
    hdr->mode_type = in[14];
    hdr->k_idx = in[15];
    hdr->seq_num = exp_method_to_seq(hdr->method_id);
    hdr->payload_len = exp_get_u32le(in + 16);
    hdr->crc32 = exp_get_u32le(in + 20);
    hdr->colorspace = JPEG_COLORSPACE_RGB;
    hdr->subsampling = (hdr->record_type == EXP_RECORD_RAW_FRAME) ?
                       EXP_SUBSAMPLING_NONE : JPEG_SUBSAMP_444;
    /* width, height and records_expected are NOT in the 24-byte wire header.
     * Populate with prototype constants so validate_record_against_run can
     * compare them without treating 0 as a valid value. */
    hdr->width  = (uint16_t)EXP_IMG_WIDTH;
    hdr->height = (uint16_t)EXP_IMG_HEIGHT;
    /* records_expected depends on run context - left 0 here, filled by the
     * receiver from run->records_expected before validation. */

    if (hdr->magic != EXP_RECORD_MAGIC) return false;
    if (hdr->version != EXP_RECORD_VERSION) return false;
    if (hdr->header_size != EXP_RECORD_HEADER_SIZE) return false;
    return true;
}

static inline void exp_record_telemetry_encode(const exp_record_header_t *hdr,
                                               uint8_t out[EXP_RECORD_TELEMETRY_SIZE])
{
    exp_put_u16le(out + 0, EXP_RECORD_TELEMETRY_MAGIC);
    out[2] = EXP_RECORD_TELEMETRY_VERSION;
    out[3] = EXP_RECORD_TELEMETRY_SIZE;
    exp_put_u32le(out + 4, hdr->t_capture_us);
    exp_put_u32le(out + 8, hdr->t_rgb565_to_rgb888_us);
    exp_put_u32le(out + 12, hdr->t_prepare_us);
    exp_put_u32le(out + 16, hdr->t_algorithm_us);
    exp_put_u32le(out + 20, hdr->t_decompress_us);
    exp_put_u32le(out + 24, hdr->t_dct_kernel_us);
    exp_put_u16le(out + 28, hdr->psnr_x100);
    exp_put_u16le(out + 30, hdr->dct_kernel_calls);
    exp_put_u16le(out + 32, exp_crc16_block(out, EXP_RECORD_TELEMETRY_BODY_SIZE));
    exp_put_u16le(out + 34, 0u);
}

static inline bool exp_record_telemetry_decode(const uint8_t in[EXP_RECORD_TELEMETRY_SIZE],
                                               exp_record_header_t *hdr)
{
    uint16_t crc_rx;
    uint16_t crc_calc;

    if (!in || !hdr) return false;
    if (exp_get_u16le(in + 0) != EXP_RECORD_TELEMETRY_MAGIC) return false;
    if (in[2] != EXP_RECORD_TELEMETRY_VERSION) return false;
    if (in[3] != EXP_RECORD_TELEMETRY_SIZE) return false;

    crc_rx = exp_get_u16le(in + 32);
    crc_calc = exp_crc16_block(in, EXP_RECORD_TELEMETRY_BODY_SIZE);
    if (crc_rx != crc_calc) return false;

    hdr->t_capture_us = exp_get_u32le(in + 4);
    hdr->t_rgb565_to_rgb888_us = exp_get_u32le(in + 8);
    hdr->t_prepare_us = exp_get_u32le(in + 12);
    hdr->t_algorithm_us = exp_get_u32le(in + 16);
    hdr->t_decompress_us = exp_get_u32le(in + 20);
    hdr->t_dct_kernel_us = exp_get_u32le(in + 24);
    hdr->psnr_x100 = exp_get_u16le(in + 28);
    hdr->dct_kernel_calls = exp_get_u16le(in + 30);
    return true;
}

static inline void exp_spi_ctrl_encode(const exp_spi_ctrl_t *ctrl,
                                       uint8_t out[EXP_SPI_CTRL_FRAME_SIZE])
{
    exp_put_u16le(out + 0, ctrl->magic);
    out[2] = ctrl->version;
    out[3] = ctrl->cmd;
    out[4] = ctrl->flags;
    out[5] = ctrl->method_id;
    out[6] = ctrl->k_idx;
    out[7] = ctrl->reserved0;
    exp_put_u32le(out + 8, ctrl->run_id);
    exp_put_u32le(out + 12, ctrl->arg0);
    exp_put_u16le(out + 16, exp_crc16_block(out, EXP_SPI_CTRL_BODY_SIZE));
}

static inline bool exp_spi_ctrl_decode(const uint8_t in[EXP_SPI_CTRL_FRAME_SIZE],
                                       exp_spi_ctrl_t *ctrl)
{
    if (!in || !ctrl) return false;

    ctrl->magic = exp_get_u16le(in + 0);
    ctrl->version = in[2];
    ctrl->cmd = in[3];
    ctrl->flags = in[4];
    ctrl->method_id = in[5];
    ctrl->k_idx = in[6];
    ctrl->reserved0 = in[7];
    ctrl->run_id = exp_get_u32le(in + 8);
    ctrl->arg0 = exp_get_u32le(in + 12);
    ctrl->crc16 = exp_get_u16le(in + 16);

    if (ctrl->magic != EXP_SPI_MAGIC) return false;
    if (ctrl->version != EXP_SPI_VERSION) return false;
    if (ctrl->crc16 != exp_crc16_block(in, EXP_SPI_CTRL_BODY_SIZE)) return false;
    return true;
}

static inline void exp_spi_status_encode(const exp_spi_status_t *st,
                                         uint8_t out[EXP_SPI_STATUS_FRAME_SIZE])
{
    exp_put_u16le(out + 0, st->magic);
    out[2] = st->version;
    out[3] = st->state;
    out[4] = st->error;
    out[5] = st->records_expected;
    out[6] = st->current_order;
    out[7] = st->reserved0;
    exp_put_u32le(out + 8, st->run_id);
    exp_put_u32le(out + 12, st->payload_len);
    exp_put_u16le(out + 16, exp_crc16_block(out, EXP_SPI_STATUS_BODY_SIZE));
}

static inline bool exp_spi_status_decode(const uint8_t in[EXP_SPI_STATUS_FRAME_SIZE],
                                         exp_spi_status_t *st)
{
    if (!in || !st) return false;

    st->magic = exp_get_u16le(in + 0);
    st->version = in[2];
    st->state = in[3];
    st->error = in[4];
    st->records_expected = in[5];
    st->current_order = in[6];
    st->reserved0 = in[7];
    st->run_id = exp_get_u32le(in + 8);
    st->payload_len = exp_get_u32le(in + 12);
    st->crc16 = exp_get_u16le(in + 16);

    if (st->magic != EXP_SPI_MAGIC) return false;
    if (st->version != EXP_SPI_VERSION) return false;
    if (st->crc16 != exp_crc16_block(in, EXP_SPI_STATUS_BODY_SIZE)) return false;
    return true;
}

static inline const char *exp_method_name(uint8_t method_id)
{
    switch (method_id) {
        case EXP_METHOD_RAW:         return "raw";
        case EXP_METHOD_LOEFFLER:    return "loeffler";
        case EXP_METHOD_MATRIX:      return "matrix";
        case EXP_METHOD_RDCT:        return "rdct";
        case EXP_METHOD_SILVEIRA_J3: return "silveira_j3";
        case EXP_METHOD_SILVEIRA_J7: return "silveira_j7";
        case EXP_METHOD_ALL:         return "all";
        default:                     return "unknown";
    }
}

static inline bool exp_method_is_jpeg(uint8_t method_id)
{
    return method_id == EXP_METHOD_LOEFFLER ||
           method_id == EXP_METHOD_MATRIX ||
           method_id == EXP_METHOD_RDCT ||
           method_id == EXP_METHOD_SILVEIRA_J3 ||
           method_id == EXP_METHOD_SILVEIRA_J7;
}

static inline int exp_jpeg_method_index(uint8_t method_id)
{
    switch (method_id) {
        case EXP_METHOD_LOEFFLER:    return 0;
        case EXP_METHOD_MATRIX:      return 1;
        case EXP_METHOD_RDCT:        return 2;
        case EXP_METHOD_SILVEIRA_J3: return 3;
        case EXP_METHOD_SILVEIRA_J7: return 4;
        default:                     return -1;
    }
}

static inline int exp_method_k_to_slot(uint8_t method_id, uint8_t k_idx)
{
    int method_index;

    if (method_id == EXP_METHOD_RAW) return EXP_RECORD_SLOT_RAW;
    if (k_idx >= EXP_K_FACTOR_COUNT) return -1;

    method_index = exp_jpeg_method_index(method_id);
    if (method_index < 0) return -1;

    return (int)EXP_RECORD_SLOT_JPEG_FIRST +
           ((int)k_idx * (int)EXP_JPEG_METHOD_COUNT) +
           method_index;
}

static inline int exp_method_to_slot(uint8_t method_id)
{
    return exp_method_k_to_slot(method_id, 0u);
}

static inline uint8_t exp_slot_to_method(int slot)
{
    int idx;

    if (slot == EXP_RECORD_SLOT_RAW) return EXP_METHOD_RAW;
    if (slot < (int)EXP_RECORD_SLOT_JPEG_FIRST || slot >= (int)EXP_RECORD_SLOT_COUNT) {
        return EXP_METHOD_RAW;
    }

    idx = (slot - (int)EXP_RECORD_SLOT_JPEG_FIRST) % (int)EXP_JPEG_METHOD_COUNT;
    switch (idx) {
        case 0: return EXP_METHOD_LOEFFLER;
        case 1: return EXP_METHOD_MATRIX;
        case 2: return EXP_METHOD_RDCT;
        case 3: return EXP_METHOD_SILVEIRA_J3;
        case 4: return EXP_METHOD_SILVEIRA_J7;
        default: return EXP_METHOD_RAW;
    }
}

static inline uint8_t exp_slot_to_k_idx(int slot)
{
    if (slot < (int)EXP_RECORD_SLOT_JPEG_FIRST || slot >= (int)EXP_RECORD_SLOT_COUNT) {
        return 0u;
    }
    return (uint8_t)((slot - (int)EXP_RECORD_SLOT_JPEG_FIRST) /
                     (int)EXP_JPEG_METHOD_COUNT);
}

static inline uint8_t exp_records_expected_for_method(uint8_t method_id)
{
    return (method_id == EXP_METHOD_ALL) ? (uint8_t)EXP_RECORD_SLOT_COUNT : 1u;
}

static inline float exp_k_factor_from_idx(uint8_t k_idx)
{
    switch (k_idx) {
        case 0: return 0.1f;
        case 1: return 0.2f;
        case 2: return 0.5f;
        case 3: return 0.8f;
        default: return 0.1f;
    }
}

static inline uint8_t exp_k_fixed8_from_idx(uint8_t k_idx)
{
    float k = exp_k_factor_from_idx(k_idx);
    return (uint8_t)(k * 100.0f + 0.5f);
}

static inline const char *exp_k_label_from_idx(uint8_t k_idx)
{
    switch (k_idx) {
        case 0: return "0.1";
        case 1: return "0.2";
        case 2: return "0.5";
        case 3: return "0.8";
        default: return "0.1";
    }
}

static inline jpeg_dct_method_t exp_method_to_jpeg_dct(uint8_t method_id)
{
    switch (method_id) {
        case EXP_METHOD_MATRIX:      return JPEG_DCT_MATRIX;
        case EXP_METHOD_RDCT:        return JPEG_DCT_RDCT;
        case EXP_METHOD_SILVEIRA_J3: return JPEG_DCT_SILVEIRA_J3;
        case EXP_METHOD_SILVEIRA_J7: return JPEG_DCT_SILVEIRA_J7;
        case EXP_METHOD_LOEFFLER:
        default:                     return JPEG_DCT_LOEFFLER;
    }
}

static inline uint8_t exp_jpeg_method_at(uint8_t idx)
{
    switch (idx) {
        case 0: return EXP_METHOD_LOEFFLER;
        case 1: return EXP_METHOD_MATRIX;
        case 2: return EXP_METHOD_RDCT;
        case 3: return EXP_METHOD_SILVEIRA_J3;
        case 4: return EXP_METHOD_SILVEIRA_J7;
        default: return EXP_METHOD_LOEFFLER;
    }
}

static inline void exp_rgb565be_to_rgb888_pixel(uint8_t hi, uint8_t lo,
                                                uint8_t out_rgb888[3])
{
    uint8_t r5 = (uint8_t)((hi >> 3) & 0x1Fu);
    uint8_t g6 = (uint8_t)(((hi & 0x07u) << 3) | (lo >> 5));
    uint8_t b5 = (uint8_t)(lo & 0x1Fu);

    out_rgb888[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out_rgb888[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    out_rgb888[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static inline void exp_convert_rgb565be_to_rgb888(const uint8_t *src565,
                                                  uint8_t *dst888,
                                                  int width, int height)
{
    int total_pixels;
    int i;

    if (!src565 || !dst888 || width <= 0 || height <= 0) return;

    total_pixels = width * height;
    for (i = 0; i < total_pixels; i++) {
        exp_rgb565be_to_rgb888_pixel(src565[i * 2], src565[i * 2 + 1],
                                     &dst888[i * 3]);
    }
}
