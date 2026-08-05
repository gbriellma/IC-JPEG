/* codec_utils.c - Shared utility functions for the canonical int32 codec.
 *
 * Contains: jpeg_free_compressed, jpeg_free_image, jpeg_version,
 *           jpeg_error_string.
 */

#include "../include/jpeg_codec.h"
#include "../include/internal.h"
#include <stdio.h>

void jpeg_free_compressed(jpeg_compressed_t *compressed) {
    if (compressed) {
        CODEC_FREE(compressed->y_coeffs);
        CODEC_FREE(compressed->y_quantized);
        CODEC_FREE(compressed->cb_coeffs);
        CODEC_FREE(compressed->cb_quantized);
        CODEC_FREE(compressed->cr_coeffs);
        CODEC_FREE(compressed->cr_quantized);
        CODEC_FREE(compressed);
    }
}

void jpeg_free_image(jpeg_image_t *image) {
    if (image) {
        CODEC_FREE(image->data);
        CODEC_FREE(image);
    }
}

/* Thread-safe: compile-time string literal, no static buffer */
const char* jpeg_version(void) {
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)
    return STRINGIFY(JPEG_VERSION_MAJOR) "."
           STRINGIFY(JPEG_VERSION_MINOR) "."
           STRINGIFY(JPEG_VERSION_PATCH);
#undef STRINGIFY
#undef STRINGIFY_
}

const char* jpeg_error_string(jpeg_error_t error) {
    switch (error) {
        case JPEG_SUCCESS:                  return "Success";
        case JPEG_ERROR_NULL_POINTER:       return "Null pointer";
        case JPEG_ERROR_INVALID_DIMENSIONS: return "Invalid dimensions";
        case JPEG_ERROR_ALLOCATION_FAILED:  return "Allocation failed";
        case JPEG_ERROR_INVALID_METHOD:     return "Invalid DCT method";
        case JPEG_ERROR_INVALID_COLORSPACE: return "Invalid colorspace";
        case JPEG_ERROR_INVALID_FRAME:      return "Invalid frame";
        case JPEG_ERROR_BUFFER_TOO_SMALL:   return "Buffer too small";
        case JPEG_ERROR_INVALID_SUBSAMPLING:return "Invalid subsampling";
        case JPEG_ERROR_INVALID_QUALITY:    return "Invalid quality factor";
        default:                            return "Unknown error";
    }
}
