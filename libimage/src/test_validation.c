/**
 * @file test_validation.c
 * @brief Validation suite for the canonical int32 codec.
 */

#include "jpeg_codec.h"
#include "internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, fmt, ...)                                                     \
    do {                                                                          \
        if (cond) {                                                               \
            printf("[ok] " fmt "\n", ##__VA_ARGS__);                              \
        } else {                                                                  \
            printf("[FAIL] " fmt "\n", ##__VA_ARGS__);                            \
            g_failures++;                                                         \
        }                                                                         \
    } while (0)

static jpeg_image_t *alloc_image(int32_t width, int32_t height,
                                 jpeg_colorspace_t colorspace)
{
    size_t nbytes;
    jpeg_image_t *img;

    img = (jpeg_image_t *)calloc(1, sizeof(*img));
    if (!img) return NULL;

    img->width = width;
    img->height = height;
    img->colorspace = colorspace;

    nbytes = (size_t)width * height * (colorspace == JPEG_COLORSPACE_GRAYSCALE ? 1u : 3u);
    img->data = (uint8_t *)calloc(nbytes, 1);
    if (!img->data) {
        free(img);
        return NULL;
    }

    return img;
}

static jpeg_image_t *make_rgb_pattern(int32_t width, int32_t height)
{
    jpeg_image_t *img = alloc_image(width, height, JPEG_COLORSPACE_RGB);
    int32_t x, y;

    if (!img) return NULL;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            size_t i = (size_t)(y * width + x) * 3u;
            img->data[i + 0] = (uint8_t)((13 * x + 7 * y) & 0xFF);
            img->data[i + 1] = (uint8_t)((3 * x + 17 * y + 40) & 0xFF);
            img->data[i + 2] = (uint8_t)((19 * x + 5 * y + 90) & 0xFF);
        }
    }

    return img;
}

static jpeg_image_t *make_gray_pattern(int32_t width, int32_t height)
{
    jpeg_image_t *img = alloc_image(width, height, JPEG_COLORSPACE_GRAYSCALE);
    int32_t x, y;

    if (!img) return NULL;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            img->data[y * width + x] = (uint8_t)((11 * x + 9 * y + 23) & 0xFF);
        }
    }

    return img;
}

static double calc_psnr(const jpeg_image_t *orig, const jpeg_image_t *recon)
{
    double mse = 0.0;
    int32_t i, total, channels;

    if (!orig || !recon) return 0.0;
    if (orig->width != recon->width || orig->height != recon->height) return 0.0;

    channels = (orig->colorspace == JPEG_COLORSPACE_GRAYSCALE) ? 1 : 3;
    total = orig->width * orig->height * channels;

    for (i = 0; i < total; i++) {
        double diff = (double)orig->data[i] - (double)recon->data[i];
        mse += diff * diff;
    }
    mse /= (double)total;

    if (mse <= 1e-12) return 100.0;
    return 10.0 * log10((255.0 * 255.0) / mse);
}

static int arrays_equal(const int32_t *a, const int32_t *b, size_t n)
{
    return memcmp(a, b, n * sizeof(*a)) == 0;
}

static int32_t abs_i32(int32_t v)
{
    return (v < 0) ? -v : v;
}

static void test_silveira_j7_roundtrip(void)
{
    int32_t input[64];
    int32_t coeff[64];
    int32_t recon[64];
    const int32_t j7_col0[8] = {2, 2, 2, 1, 2, 2, 0, 0};
    int32_t max_err = 0;
    int basis_ok = 1;

    for (int i = 0; i < 64; i++)
        input[i] = (int32_t)((i * 37 + 19) % 255) - 128;

    dct_silveira_j7_2d(input, coeff);
    idct_silveira_j7_2d(coeff, recon);

    for (int i = 0; i < 64; i++) {
        int32_t err = abs_i32(recon[i] - input[i]);
        if (err > max_err) max_err = err;
    }

    CHECK(max_err == 0,
          "Silveira j=7 2D inverse exact without quantization (got %d)",
          (int)max_err);

    memset(input, 0, sizeof(input));
    input[0] = 1;
    dct_silveira_j7_2d(input, coeff);

    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int32_t expected = j7_col0[y] * j7_col0[x];
            if (coeff[y * 8 + x] != expected) {
                basis_ok = 0;
            }
        }
    }

    CHECK(basis_ok,
          "Silveira j=7 forward matches 2*T(a) basis column from Table 1");
}

static void test_silveira_class_codec_smoke(void)
{
    jpeg_dct_method_t methods[2] = {
        JPEG_DCT_SILVEIRA_J3, JPEG_DCT_SILVEIRA_J7
    };
    const char *names[2] = {
        "silveira_j3", "silveira_j7"
    };
    jpeg_image_t *img = make_rgb_pattern(16, 16);

    CHECK(img != NULL, "allocate RGB image for Silveira class smoke tests");
    if (!img) return;

    CHECK(JPEG_DCT_IDENTITY == 5, "JPEG_DCT_IDENTITY enum value is 5");

    for (int i = 0; i < 2; i++) {
        jpeg_compressed_t *comp = NULL;
        jpeg_image_t *recon = NULL;
        jpeg_params_t params;
        jpeg_error_t err;

        params.quality_factor = 2.0f;
        params.dct_method = methods[i];
        params.subsampling = JPEG_SUBSAMP_444;
        params.flags = 0;

        err = jpeg_compress(img, &params, &comp);
        CHECK(err == JPEG_SUCCESS && comp != NULL,
              "compress RGB with %s", names[i]);
        if (err == JPEG_SUCCESS && comp) {
            err = jpeg_decompress(comp, &recon);
            CHECK(err == JPEG_SUCCESS && recon != NULL,
                  "decompress RGB with %s", names[i]);
        }

        jpeg_free_image(recon);
        jpeg_free_compressed(comp);
    }

    jpeg_free_image(img);
}

static void test_identity_gray_exact(void)
{
    jpeg_image_t *img = make_gray_pattern(19, 13);
    jpeg_compressed_t *comp = NULL;
    jpeg_image_t *recon = NULL;
    jpeg_params_t params;
    jpeg_error_t err;

    CHECK(img != NULL, "allocate grayscale test image");
    if (!img) return;

    params.quality_factor = 1.0f;
    params.dct_method = JPEG_DCT_IDENTITY;
    params.subsampling = JPEG_SUBSAMP_444;
    params.flags = JPEG_FLAG_SKIP_QUANTIZATION | JPEG_FLAG_KEEP_COEFFS;

    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_SUCCESS && comp != NULL, "compress grayscale identity");
    if (err == JPEG_SUCCESS && comp) {
        CHECK(comp->cb_quantized == NULL && comp->cr_quantized == NULL,
              "grayscale fast path omits chroma buffers");
        CHECK(comp->y_coeffs != NULL, "KEEP_COEFFS stores luma coefficients");
        err = jpeg_decompress(comp, &recon);
        CHECK(err == JPEG_SUCCESS && recon != NULL, "decompress grayscale identity");
        if (err == JPEG_SUCCESS && recon) {
            CHECK(recon->colorspace == JPEG_COLORSPACE_GRAYSCALE,
                  "grayscale decode preserves colorspace");
            CHECK(memcmp(img->data, recon->data, (size_t)img->width * img->height) == 0,
                  "grayscale identity + skip quantization is exact");
        }
    }

    jpeg_free_image(img);
    jpeg_free_image(recon);
    jpeg_free_compressed(comp);
}

static void test_rgb_roundtrip_subsampling(jpeg_subsampling_t subsampling)
{
    jpeg_image_t *img = make_rgb_pattern(23, 17);
    jpeg_compressed_t *comp = NULL;
    jpeg_image_t *recon = NULL;
    jpeg_params_t params;
    jpeg_error_t err;
    uint32_t expected_kernel_calls;
    double psnr;
    double min_psnr;
    const char *label = subsampling == JPEG_SUBSAMP_444 ? "444"
                      : subsampling == JPEG_SUBSAMP_422 ? "422"
                      : "420";

    CHECK(img != NULL, "allocate RGB image for %s", label);
    if (!img) return;

    params.quality_factor = 2.0f;
    params.dct_method = JPEG_DCT_LOEFFLER;
    params.subsampling = subsampling;
    params.flags = 0;
    min_psnr = subsampling == JPEG_SUBSAMP_444 ? 17.0
             : subsampling == JPEG_SUBSAMP_422 ? 15.5
             : 14.5;

    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_SUCCESS && comp != NULL, "compress RGB Loeffler %s", label);
    if (err == JPEG_SUCCESS && comp) {
        expected_kernel_calls = (uint32_t)(comp->num_blocks_y + 2 * comp->num_blocks_chroma);
        CHECK(comp->subsampling == subsampling, "compressed metadata stores subsampling %s", label);
        CHECK(comp->dct_kernel_calls == expected_kernel_calls,
              "DCT profiling counts all 8x8 blocks for %s", label);
        err = jpeg_decompress(comp, &recon);
        CHECK(err == JPEG_SUCCESS && recon != NULL, "decompress RGB Loeffler %s", label);
        CHECK(comp->idct_kernel_calls == expected_kernel_calls,
              "IDCT profiling counts all 8x8 blocks for %s", label);
        if (err == JPEG_SUCCESS && recon) {
            psnr = calc_psnr(img, recon);
            CHECK(psnr > min_psnr,
                  "RGB round-trip PSNR > %.1f dB for %s (got %.2f)",
                  min_psnr, label, psnr);
        }
    }

    jpeg_free_image(img);
    jpeg_free_image(recon);
    jpeg_free_compressed(comp);
}

static void test_skip_quantization_consistency(void)
{
    jpeg_image_t *img = make_rgb_pattern(16, 16);
    jpeg_compressed_t *comp = NULL;
    jpeg_params_t params;
    jpeg_error_t err;
    size_t coeff_count;

    CHECK(img != NULL, "allocate RGB image for skip quantization");
    if (!img) return;

    params.quality_factor = 4.0f;
    params.dct_method = JPEG_DCT_MATRIX;
    params.subsampling = JPEG_SUBSAMP_444;
    params.flags = JPEG_FLAG_SKIP_QUANTIZATION | JPEG_FLAG_KEEP_COEFFS;

    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_SUCCESS && comp != NULL, "compress with skip quantization");
    if (err == JPEG_SUCCESS && comp) {
        coeff_count = (size_t)comp->num_blocks_y * 64u;
        CHECK(comp->y_coeffs != NULL, "KEEP_COEFFS allocates Y coefficients");
        CHECK(arrays_equal(comp->y_coeffs, comp->y_quantized, coeff_count),
              "skip quantization keeps Y coefficients identical");
    }

    jpeg_free_image(img);
    jpeg_free_compressed(comp);
}

static void test_frame_roundtrip(void)
{
    jpeg_image_t *img = make_rgb_pattern(24, 16);
    jpeg_compressed_t *comp = NULL;
    jpeg_compressed_t *decoded = NULL;
    jpeg_compressed_t *decoded_payload = NULL;
    jpeg_image_t *recon = NULL;
    jpeg_params_t params;
    jpeg_error_t err;
    int32_t frame_len;
    int32_t payload_len;
    uint8_t *frame_buf = NULL;
    uint8_t *payload_buf = NULL;
    size_t y_count, c_count;

    CHECK(img != NULL, "allocate RGB image for frame round-trip");
    if (!img) return;

    params.quality_factor = 2.0f;
    params.dct_method = JPEG_DCT_LOEFFLER;
    params.subsampling = JPEG_SUBSAMP_422;
    params.flags = 0;

    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_SUCCESS && comp != NULL, "compress for frame round-trip");
    if (err != JPEG_SUCCESS || !comp) {
        jpeg_free_image(img);
        return;
    }

    frame_buf = (uint8_t *)calloc(JPEG_FRAME_MAX_TOTAL_BYTES, 1);
    CHECK(frame_buf != NULL, "allocate frame buffer");
    if (!frame_buf) goto cleanup;
    payload_buf = (uint8_t *)calloc(JPEG_FRAME_MAX_PAYLOAD_BYTES, 1);
    CHECK(payload_buf != NULL, "allocate payload buffer");
    if (!payload_buf) goto cleanup;

    frame_len = jpeg_frame_encode(comp, frame_buf, JPEG_FRAME_MAX_TOTAL_BYTES);
    CHECK(frame_len > JPEG_FRAME_HEADER_SIZE, "frame encode succeeds");
    if (frame_len <= JPEG_FRAME_HEADER_SIZE) goto cleanup;

    CHECK(frame_buf[0] == JPEG_FRAME_MAGIC, "frame header starts with magic");
    CHECK(frame_buf[1] == JPEG_FRAME_VERSION, "frame header version matches");
    CHECK((int32_t)(((uint32_t)frame_buf[2] << 24) |
                    ((uint32_t)frame_buf[3] << 16) |
                    ((uint32_t)frame_buf[4] << 8) |
                    (uint32_t)frame_buf[5]) == frame_len - JPEG_FRAME_HEADER_SIZE,
          "frame header payload length matches encoded size");

    decoded = jpeg_frame_alloc_compressed(img->width, img->height,
                                          params.subsampling, img->colorspace);
    CHECK(decoded != NULL, "frame alloc compressed context");
    if (!decoded) goto cleanup;

    decoded->quality_factor = params.quality_factor;
    decoded->dct_method = params.dct_method;
    decoded->flags = params.flags;

    CHECK(jpeg_frame_decode(frame_buf, frame_len, decoded) == frame_len,
          "frame decode succeeds with matching context");

    payload_len = jpeg_frame_payload_encode(comp, payload_buf,
                                            JPEG_FRAME_MAX_PAYLOAD_BYTES);
    CHECK(payload_len == frame_len - JPEG_FRAME_HEADER_SIZE,
          "payload-only encode length matches framed payload");
    decoded_payload = jpeg_frame_alloc_compressed(img->width, img->height,
                                                  params.subsampling, img->colorspace);
    CHECK(decoded_payload != NULL, "payload alloc compressed context");
    if (!decoded_payload) goto cleanup;
    decoded_payload->quality_factor = params.quality_factor;
    decoded_payload->dct_method = params.dct_method;
    decoded_payload->flags = params.flags;

    CHECK(jpeg_frame_payload_decode(payload_buf,
                                    payload_len, decoded_payload) == payload_len,
          "payload-only decode succeeds with matching context");

    y_count = (size_t)comp->num_blocks_y * 64u;
    c_count = (size_t)comp->num_blocks_chroma * 64u;
    CHECK(arrays_equal(comp->y_quantized, decoded->y_quantized, y_count),
          "frame decode preserves Y quantized blocks");
    CHECK(arrays_equal(comp->cb_quantized, decoded->cb_quantized, c_count),
          "frame decode preserves Cb quantized blocks");
    CHECK(arrays_equal(comp->cr_quantized, decoded->cr_quantized, c_count),
          "frame decode preserves Cr quantized blocks");
    CHECK(arrays_equal(comp->y_quantized, decoded_payload->y_quantized, y_count),
          "payload decode preserves Y quantized blocks");
    CHECK(arrays_equal(comp->cb_quantized, decoded_payload->cb_quantized, c_count),
          "payload decode preserves Cb quantized blocks");
    CHECK(arrays_equal(comp->cr_quantized, decoded_payload->cr_quantized, c_count),
          "payload decode preserves Cr quantized blocks");

    err = jpeg_decompress(decoded, &recon);
    CHECK(err == JPEG_SUCCESS && recon != NULL, "decompress decoded frame");

cleanup:
    jpeg_free_image(img);
    jpeg_free_image(recon);
    jpeg_free_compressed(decoded);
    jpeg_free_compressed(decoded_payload);
    jpeg_free_compressed(comp);
    free(frame_buf);
    free(payload_buf);
}

static void test_frame_rejects_invalid_inputs(void)
{
    jpeg_image_t *img = make_rgb_pattern(16, 16);
    jpeg_compressed_t *comp = NULL;
    jpeg_compressed_t *wrong_ctx = NULL;
    jpeg_params_t params;
    jpeg_error_t err;
    int32_t frame_len;
    uint8_t *frame_buf = NULL;

    CHECK(img != NULL, "allocate RGB image for invalid frame tests");
    if (!img) return;

    params.quality_factor = 2.0f;
    params.dct_method = JPEG_DCT_LOEFFLER;
    params.subsampling = JPEG_SUBSAMP_444;
    params.flags = 0;

    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_SUCCESS && comp != NULL, "compress for invalid frame tests");
    if (err != JPEG_SUCCESS || !comp) {
        jpeg_free_image(img);
        return;
    }

    frame_buf = (uint8_t *)calloc(JPEG_FRAME_MAX_TOTAL_BYTES, 1);
    CHECK(frame_buf != NULL, "allocate invalid-frame buffer");
    if (!frame_buf) goto cleanup;

    frame_len = jpeg_frame_encode(comp, frame_buf, JPEG_FRAME_MAX_TOTAL_BYTES);
    CHECK(frame_len > 0, "encode frame for invalid input checks");
    if (frame_len <= 0) goto cleanup;

    wrong_ctx = jpeg_frame_alloc_compressed(img->width, img->height,
                                            JPEG_SUBSAMP_444, JPEG_COLORSPACE_GRAYSCALE);
    CHECK(wrong_ctx != NULL, "allocate mismatched frame context");
    if (wrong_ctx) {
        CHECK(jpeg_frame_decode(frame_buf, frame_len, wrong_ctx) < 0,
              "frame decode rejects mismatched grayscale context");
        jpeg_free_compressed(wrong_ctx);
        wrong_ctx = NULL;
    }

    frame_buf[0] ^= 0x01;
    wrong_ctx = jpeg_frame_alloc_compressed(img->width, img->height,
                                            params.subsampling, img->colorspace);
    CHECK(wrong_ctx != NULL, "allocate valid frame context");
    if (wrong_ctx) {
        CHECK(jpeg_frame_decode(frame_buf, frame_len, wrong_ctx) < 0,
              "frame decode rejects invalid magic");
        jpeg_free_compressed(wrong_ctx);
        wrong_ctx = NULL;
    }
    frame_buf[0] ^= 0x01;

    wrong_ctx = jpeg_frame_alloc_compressed(img->width, img->height,
                                            params.subsampling, img->colorspace);
    CHECK(wrong_ctx != NULL, "allocate valid frame context for truncation");
    if (wrong_ctx) {
        CHECK(jpeg_frame_decode(frame_buf, frame_len - 1, wrong_ctx) < 0,
              "frame decode rejects truncated payload");
        jpeg_free_compressed(wrong_ctx);
    }

cleanup:
    jpeg_free_image(img);
    jpeg_free_compressed(comp);
    free(frame_buf);
}

static void test_invalid_parameter_validation(void)
{
    jpeg_image_t *img = make_rgb_pattern(16, 16);
    jpeg_compressed_t *comp = NULL;
    jpeg_params_t params;
    jpeg_error_t err;

    CHECK(img != NULL, "allocate RGB image for invalid-parameter tests");
    if (!img) return;

    params.quality_factor = 2.0f;
    params.dct_method = JPEG_DCT_LOEFFLER;
    params.subsampling = JPEG_SUBSAMP_444;
    params.flags = 0;

    params.subsampling = (jpeg_subsampling_t)99;
    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_ERROR_INVALID_SUBSAMPLING,
          "compress rejects invalid subsampling with explicit error");

    params.subsampling = JPEG_SUBSAMP_444;
    params.quality_factor = NAN;
    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_ERROR_INVALID_QUALITY,
          "compress rejects NaN quality factor");

    params.quality_factor = -1.0f;
    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_ERROR_INVALID_QUALITY,
          "compress rejects non-positive quality factor");

    params.quality_factor = 2.0f;
    err = jpeg_compress(img, &params, &comp);
    CHECK(err == JPEG_SUCCESS && comp != NULL, "compress succeeds for validation baseline");
    if (err == JPEG_SUCCESS && comp) {
        jpeg_subsampling_t saved_subsampling = comp->subsampling;
        jpeg_colorspace_t saved_colorspace = comp->colorspace;
        float saved_quality = comp->quality_factor;

        comp->subsampling = (jpeg_subsampling_t)99;
        err = jpeg_decompress(comp, NULL);
        CHECK(err == JPEG_ERROR_NULL_POINTER, "decompress still checks null output pointer first");

        {
            jpeg_image_t *recon = NULL;
            err = jpeg_decompress(comp, &recon);
            CHECK(err == JPEG_ERROR_INVALID_SUBSAMPLING,
                  "decompress rejects invalid subsampling");
            jpeg_free_image(recon);
        }

        comp->subsampling = saved_subsampling;
        comp->colorspace = (jpeg_colorspace_t)99;
        {
            jpeg_image_t *recon = NULL;
            err = jpeg_decompress(comp, &recon);
            CHECK(err == JPEG_ERROR_INVALID_COLORSPACE,
                  "decompress rejects invalid colorspace");
            jpeg_free_image(recon);
        }

        comp->colorspace = saved_colorspace;
        comp->quality_factor = NAN;
        {
            jpeg_image_t *recon = NULL;
            err = jpeg_decompress(comp, &recon);
            CHECK(err == JPEG_ERROR_INVALID_QUALITY,
                  "decompress rejects invalid quality factor");
            jpeg_free_image(recon);
        }

        comp->quality_factor = saved_quality;
    }

    CHECK(jpeg_frame_alloc_compressed(16, 16, (jpeg_subsampling_t)99, JPEG_COLORSPACE_RGB) == NULL,
          "frame alloc rejects invalid subsampling");
    CHECK(jpeg_frame_alloc_compressed(16, 16, JPEG_SUBSAMP_444, (jpeg_colorspace_t)99) == NULL,
          "frame alloc rejects invalid colorspace");
    CHECK(jpeg_frame_alloc_compressed(INT32_MAX, INT32_MAX,
                                      JPEG_SUBSAMP_444, JPEG_COLORSPACE_RGB) == NULL,
          "frame alloc rejects overflowing block geometry");

    jpeg_free_compressed(comp);
    jpeg_free_image(img);
}

int main(void)
{
    printf("libimage validation suite %s\n", jpeg_version());

    test_identity_gray_exact();
    test_silveira_j7_roundtrip();
    test_silveira_class_codec_smoke();
    test_rgb_roundtrip_subsampling(JPEG_SUBSAMP_444);
    test_rgb_roundtrip_subsampling(JPEG_SUBSAMP_422);
    test_rgb_roundtrip_subsampling(JPEG_SUBSAMP_420);
    test_skip_quantization_consistency();
    test_frame_roundtrip();
    test_frame_rejects_invalid_inputs();
    test_invalid_parameter_validation();

    if (g_failures == 0) {
        printf("All validation tests passed.\n");
        return 0;
    }

    printf("%d validation test(s) failed.\n", g_failures);
    return 1;
}
