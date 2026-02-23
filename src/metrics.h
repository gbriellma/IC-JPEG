#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include "jpeg_codec.h"

double calc_psnr(const uint8_t *orig, const uint8_t *recon, int width, int height);
double calc_bitrate(const jpeg_compressed_t *comp);

#endif /* METRICS_H */
