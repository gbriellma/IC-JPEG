/* cobs.c - Consistent Overhead Byte Stuffing (COBS) for UART framing
 *
 * COBS eliminates all 0x00 bytes from the payload, allowing 0x00 to be
 * used as an unambiguous frame delimiter on UART.
 *
 * Protocol on wire:
 *   [0x00]                    - start delimiter
 *   [COBS-encoded payload]    - no 0x00 bytes inside
 *   [0x00]                    - end delimiter
 *
 * Overhead: max +1 byte per 254 input bytes (~0.4%).
 *
 * Reference: Cheshire & Baker, "Consistent Overhead Byte Stuffing",
 *            IEEE/ACM Transactions on Networking, 1999.
 */

#include "../include/jpeg_codec.h"

int32_t jpeg_cobs_encode(const uint8_t *input, int32_t input_len,
                         uint8_t *output, int32_t max_len)
{
    if (!input || !output || input_len < 0) return -1;

    int32_t out_pos = 0;
    int32_t code_pos = out_pos++;  /* reserve space for first overhead byte */
    uint8_t code = 1;

    if (out_pos > max_len) return -1;

    for (int32_t i = 0; i < input_len; i++) {
        if (input[i] == 0) {
            /* End of a non-zero run: write the overhead byte */
            if (code_pos >= max_len) return -1;
            output[code_pos] = code;
            code_pos = out_pos++;
            code = 1;
            if (out_pos > max_len) return -1;
        } else {
            if (out_pos >= max_len) return -1;
            output[out_pos++] = input[i];
            code++;

            if (code == 0xFF) {
                /* Max run length reached - emit overhead and start new run */
                if (code_pos >= max_len) return -1;
                output[code_pos] = code;
                code_pos = out_pos++;
                code = 1;
                if (out_pos > max_len) return -1;
            }
        }
    }

    /* Final overhead byte */
    if (code_pos >= max_len) return -1;
    output[code_pos] = code;

    return out_pos;
}

int32_t jpeg_cobs_decode(const uint8_t *input, int32_t input_len,
                         uint8_t *output, int32_t max_len)
{
    if (!input || !output || input_len < 1) return -1;

    int32_t in_pos = 0;
    int32_t out_pos = 0;

    while (in_pos < input_len) {
        uint8_t code = input[in_pos++];
        if (code == 0) return -1;  /* 0x00 should not appear in COBS data */

        for (uint8_t i = 1; i < code; i++) {
            if (in_pos >= input_len) return -1;
            if (out_pos >= max_len) return -1;
            output[out_pos++] = input[in_pos++];
        }

        /* If code < 0xFF, the implicit zero is restored */
        if (code < 0xFF && in_pos < input_len) {
            if (out_pos >= max_len) return -1;
            output[out_pos++] = 0;
        }
    }

    return out_pos;
}
