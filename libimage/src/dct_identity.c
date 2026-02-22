/* dct_identity.c - Pass-through for validation (no compression) */

#include "../include/internal.h"
#include <string.h>

void dct_identity_2d(const int32_t *input, int32_t *output) {
    memcpy(output, input, 64 * sizeof(int32_t));
}

void idct_identity_2d(const int32_t *input, int32_t *output) {
    memcpy(output, input, 64 * sizeof(int32_t));
}