idf_component_register(
    SRCS "src/codec.c"
         "src/colorspace.c"
         "src/dct_approx.c"
         "src/dct_identity.c"
         "src/dct_loeffler.c"
         "src/dct_matrix.c"
         "src/quantization.c"
         "src/utils.c"
    INCLUDE_DIRS "include"
    REQUIRES heap
)
