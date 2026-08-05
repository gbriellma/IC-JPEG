/* frame.c - Minimal experimental frame transport
 *
 * Wire format v2:
 *   [0xA7][version=2][len_be32][payload...]
 *
 * Legacy v1 decode is kept for previously captured frames:
 *   [0xA5][len_be16][payload...]
 *
 * Payload layout:
 *   [Y_RLE...][Cb_RLE...][Cr_RLE...]
 *
 * The caller provides decode context out-of-band: width, height,
 * colorspace, subsampling, quality factor, method and flags.
 */

#include "../include/jpeg_codec.h"
#include "../include/internal.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *FRAME_TAG = "jpeg_frame";
#define FRAME_LOGI(...) ESP_LOGI(FRAME_TAG, __VA_ARGS__)
#else
#define FRAME_LOGI(...) ((void)0)
#endif

static inline uint16_t get_u16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline uint32_t get_u32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

int32_t jpeg_frame_encode(const jpeg_compressed_t *comp,
                          uint8_t *out, int32_t max_len)
{
    int32_t payload_total;

    if (!out) return -1;
    if (max_len < JPEG_FRAME_HEADER_SIZE) return -1;

    payload_total = jpeg_frame_payload_encode(comp, out + JPEG_FRAME_HEADER_SIZE,
                                              max_len - JPEG_FRAME_HEADER_SIZE);
    if (payload_total < 0) return -1;

    out[0] = JPEG_FRAME_MAGIC;
    out[1] = JPEG_FRAME_VERSION;
    put_u32be(out + 2, (uint32_t)payload_total);
    return JPEG_FRAME_HEADER_SIZE + payload_total;
}

int32_t jpeg_frame_payload_encode(const jpeg_compressed_t *comp,
                                  uint8_t *out, int32_t max_len)
{
    int32_t len_y, len_cb = 0, len_cr = 0, payload_total;
    int is_gray;

    if (!comp || !out || !comp->y_quantized) return -1;
    if (max_len <= 0) return -1;
    if (!jpeg_valid_colorspace(comp->colorspace)) return -1;
    if (!jpeg_valid_subsampling(comp->subsampling)) return -1;
    if (!jpeg_valid_quality_factor(comp->quality_factor)) return -1;

    is_gray = (comp->colorspace == JPEG_COLORSPACE_GRAYSCALE);

    len_y = jpeg_rle_encode(comp->y_quantized, comp->num_blocks_y,
                            out, max_len);
    if (len_y < 0) return -1;

    if (!is_gray) {
        if (!comp->cb_quantized || !comp->cr_quantized) return -1;

        len_cb = jpeg_rle_encode(comp->cb_quantized, comp->num_blocks_chroma,
                                 out + len_y, max_len - len_y);
        if (len_cb < 0) return -1;

        len_cr = jpeg_rle_encode(comp->cr_quantized, comp->num_blocks_chroma,
                                 out + len_y + len_cb, max_len - len_y - len_cb);
        if (len_cr < 0) return -1;
    }

    payload_total = len_y + len_cb + len_cr;
    if (payload_total > (int32_t)JPEG_FRAME_MAX_PAYLOAD_BYTES) return -1;

    return payload_total;
}

int32_t jpeg_frame_decode(const uint8_t *in, int32_t in_len,
                          jpeg_compressed_t *comp)
{
    int32_t payload_total, header_size, decoded_len;
    const uint8_t *payload;

    if (!in || !comp || !comp->y_quantized) return -1;
    if (in_len < (int32_t)JPEG_FRAME_V1_HEADER_SIZE) return -1;
    if (!jpeg_valid_colorspace(comp->colorspace)) return -1;
    if (!jpeg_valid_subsampling(comp->subsampling)) return -1;

    if (in[0] == JPEG_FRAME_MAGIC_V1) {
        header_size = (int32_t)JPEG_FRAME_V1_HEADER_SIZE;
        payload_total = (int32_t)get_u16be(in + 1);
    } else if (in[0] == JPEG_FRAME_MAGIC_V2) {
        uint32_t payload_u32;

        if (in_len < (int32_t)JPEG_FRAME_HEADER_SIZE) return -1;
        if (in[1] != JPEG_FRAME_VERSION) return -1;
        payload_u32 = get_u32be(in + 2);
        if (payload_u32 > JPEG_FRAME_MAX_PAYLOAD_BYTES) return -1;
        payload_total = (int32_t)payload_u32;
        header_size = (int32_t)JPEG_FRAME_HEADER_SIZE;
    } else {
        return -1;
    }

    if (payload_total > (int32_t)JPEG_FRAME_MAX_PAYLOAD_BYTES) return -1;
    if (in_len != header_size + payload_total) return -1;

    payload = in + header_size;
    decoded_len = jpeg_frame_payload_decode(payload, payload_total, comp);
    if (decoded_len != payload_total) return -1;
    return header_size + payload_total;
}

int32_t jpeg_frame_payload_decode(const uint8_t *payload, int32_t payload_len,
                                  jpeg_compressed_t *comp)
{
    int32_t consumed = 0, n;
    int is_gray;

    if (!payload || !comp || !comp->y_quantized) return -1;
    if (payload_len <= 0) return -1;
    if (payload_len > (int32_t)JPEG_FRAME_MAX_PAYLOAD_BYTES) return -1;
    if (!jpeg_valid_colorspace(comp->colorspace)) return -1;
    if (!jpeg_valid_subsampling(comp->subsampling)) return -1;

    is_gray = (comp->colorspace == JPEG_COLORSPACE_GRAYSCALE);

    FRAME_LOGI("decode Y inicio blocks=%ld payload_total=%ld", (long)comp->num_blocks_y, (long)payload_len);
    n = jpeg_rle_decode(payload + consumed, payload_len - consumed,
                        comp->y_quantized, comp->num_blocks_y);
    if (n < 0) return -1;
    consumed += n;
    FRAME_LOGI("decode Y fim consumed=%ld/%ld", (long)consumed, (long)payload_len);

    if (!is_gray) {
        if (!comp->cb_quantized || !comp->cr_quantized || comp->num_blocks_chroma <= 0)
            return -1;

        FRAME_LOGI("decode Cb inicio blocks=%ld rem=%ld", (long)comp->num_blocks_chroma,
                   (long)(payload_len - consumed));
        n = jpeg_rle_decode(payload + consumed, payload_len - consumed,
                            comp->cb_quantized, comp->num_blocks_chroma);
        if (n < 0) return -1;
        consumed += n;
        FRAME_LOGI("decode Cb fim consumed=%ld/%ld", (long)consumed, (long)payload_len);

        FRAME_LOGI("decode Cr inicio blocks=%ld rem=%ld", (long)comp->num_blocks_chroma,
                   (long)(payload_len - consumed));
        n = jpeg_rle_decode(payload + consumed, payload_len - consumed,
                            comp->cr_quantized, comp->num_blocks_chroma);
        if (n < 0) return -1;
        consumed += n;
        FRAME_LOGI("decode Cr fim consumed=%ld/%ld", (long)consumed, (long)payload_len);
    }

    if (consumed != payload_len) return -1;
    FRAME_LOGI("decode frame fim total=%ld", (long)consumed);
    return payload_len;
}

jpeg_compressed_t *jpeg_frame_alloc_compressed(int32_t width, int32_t height,
                                               jpeg_subsampling_t subsampling,
                                               jpeg_colorspace_t colorspace)
{
    jpeg_compressed_t *comp;
    int32_t num_luma, num_chroma;
    size_t luma_elems, chroma_elems, luma_bytes, chroma_bytes;

    if (width <= 0 || height <= 0) return NULL;
    if (!jpeg_valid_colorspace(colorspace)) return NULL;
    if (!jpeg_valid_subsampling(subsampling)) return NULL;
    if (!codec_compute_block_geometry(width, height, colorspace, subsampling,
                                      NULL, NULL, &num_luma, &num_chroma)) {
        return NULL;
    }
    if (!codec_checked_mul_size((size_t)num_luma, 64u, &luma_elems)) return NULL;
    if (!codec_checked_mul_size((size_t)num_chroma, 64u, &chroma_elems)) return NULL;
    if (!codec_checked_mul_size(luma_elems, sizeof(int32_t), &luma_bytes)) return NULL;
    if (!codec_checked_mul_size(chroma_elems, sizeof(int32_t), &chroma_bytes)) return NULL;

    FRAME_LOGI("alloc req w=%ld h=%ld subs=%d colorspace=%d y_blocks=%ld c_blocks=%ld y_bytes=%lu c_bytes=%lu free_psram=%lu max_psram=%lu",
               (long)width, (long)height, (int)subsampling, (int)colorspace,
               (long)num_luma, (long)num_chroma,
               (unsigned long)luma_bytes, (unsigned long)chroma_bytes,
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    FRAME_LOGI("alloc struct inicio bytes=%u", (unsigned)sizeof(jpeg_compressed_t));
    comp = (jpeg_compressed_t *)codec_alloc_zero_small_bytes(sizeof(jpeg_compressed_t));
    FRAME_LOGI("alloc struct fim ptr=%p", (void *)comp);
    if (!comp) return NULL;

    comp->width = width;
    comp->height = height;
    comp->colorspace = colorspace;
    comp->subsampling = (colorspace == JPEG_COLORSPACE_GRAYSCALE) ? JPEG_SUBSAMP_444 : subsampling;
    comp->num_blocks_y = num_luma;
    comp->num_blocks_chroma = num_chroma;

    FRAME_LOGI("alloc y inicio bytes=%lu", (unsigned long)luma_bytes);
    comp->y_quantized = (int32_t *)codec_alloc_zero_bytes(luma_bytes);
    FRAME_LOGI("alloc y fim ptr=%p", (void *)comp->y_quantized);
    if (!comp->y_quantized) {
        jpeg_free_compressed(comp);
        return NULL;
    }

    if (num_chroma > 0) {
        FRAME_LOGI("alloc cb inicio bytes=%lu", (unsigned long)chroma_bytes);
        comp->cb_quantized = (int32_t *)codec_alloc_zero_bytes(chroma_bytes);
        FRAME_LOGI("alloc cb fim ptr=%p", (void *)comp->cb_quantized);
        FRAME_LOGI("alloc cr inicio bytes=%lu", (unsigned long)chroma_bytes);
        comp->cr_quantized = (int32_t *)codec_alloc_zero_bytes(chroma_bytes);
        FRAME_LOGI("alloc cr fim ptr=%p", (void *)comp->cr_quantized);
        if (!comp->cb_quantized || !comp->cr_quantized) {
            jpeg_free_compressed(comp);
            return NULL;
        }
    }

    return comp;
}
