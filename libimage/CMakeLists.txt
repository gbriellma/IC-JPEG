# CMakeLists.txt -- ESP-IDF component for libimage JPEG codec
#
# Single-variant build: int32 codec with float Loeffler/Matrix references.

set(LIBIMAGE_SRCS
    "src/codec.c"
    "src/codec_utils.c"
    "src/cobs.c"
    "src/colorspace.c"
    "src/dct_approx.c"
    "src/dct_class.c"
    "src/dct_identity.c"
    "src/dct_loeffler.c"
    "src/dct_matrix.c"
    "src/frame.c"
    "src/quantization.c"
    "src/utils.c"
    "src/zigzag_rle.c"
)

idf_component_register(
    SRCS ${LIBIMAGE_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES heap esp_timer
)

target_link_libraries(${COMPONENT_LIB} PUBLIC m)
