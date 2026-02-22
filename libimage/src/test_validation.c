/**
 * @file test_validation.c
 */

#include "jpeg_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Zigzag order */
static const int zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Calculate PSNR */
double calc_psnr(const jpeg_image_t *orig, const jpeg_image_t *recon) {
    if (!orig || !recon) return 0.0;
    if (orig->width != recon->width || orig->height != recon->height) return 0.0;
    
    double mse = 0.0;
    int total = orig->width * orig->height * 3;
    
    for (int i = 0; i < total; i++) {
        double diff = (double)orig->data[i] - (double)recon->data[i];
        mse += diff * diff;
    }
    mse /= total;
    
    if (mse < 1e-10) return 100.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}

/* Calculate bitrate using last non-zero coefficient in zigzag order */
double calc_bitrate(const jpeg_compressed_t *comp) {
    if (!comp) return 0.0;
    
    double total_bits = 0.0;
    int total_blocks = 0;
    
    const int32_t *channels[3] = {comp->y_quantized, comp->cb_quantized, comp->cr_quantized};
    int num_blocks[3] = {comp->num_blocks_y, comp->num_blocks_chroma, comp->num_blocks_chroma};
    
    for (int ch = 0; ch < 3; ch++) {
        for (int b = 0; b < num_blocks[ch]; b++) {
            const int32_t *block = channels[ch] + (b * 64);
            
            /* Find last non-zero coefficient in zigzag order */
            int last_nonzero = -1;
            for (int i = 63; i >= 0; i--) {
                int zz_idx = zigzag[i];
                if (block[zz_idx] != 0) {
                    last_nonzero = i;
                    break;
                }
            }
            
            /* Bits for this block: (last_nonzero + 1) * 8 bits per coefficient */
            /* Each coefficient needs ~8 bits to represent values [-128, 127] */
            if (last_nonzero >= 0) {
                total_bits += (last_nonzero + 1) * 8.0;
            }
            total_blocks++;
        }
    }
    
    /* bpp = total_bits / total_pixels */
    /* total_pixels = total_blocks * 64 (each block is 8x8) */
    int total_pixels = total_blocks * 64;
    return total_pixels > 0 ? total_bits / total_pixels : 0.0;
}

/* Create test image */
jpeg_image_t* create_test_image(int w, int h, uint8_t value) {
    jpeg_image_t *img = (jpeg_image_t*)calloc(1, sizeof(jpeg_image_t));
    if (!img) return NULL;
    
    img->width = w;
    img->height = h;
    img->colorspace = JPEG_COLORSPACE_RGB;
    img->data = (uint8_t*)calloc(w * h * 3, sizeof(uint8_t));
    
    if (!img->data) {
        free(img);
        return NULL;
    }
    
    memset(img->data, value, w * h * 3);
    return img;
}

/* Create test image with random noise for maximum bitrate */
jpeg_image_t* create_random_image(int w, int h) {
    jpeg_image_t *img = (jpeg_image_t*)calloc(1, sizeof(jpeg_image_t));
    if (!img) return NULL;

    img->width = w;
    img->height = h;
    img->colorspace = JPEG_COLORSPACE_RGB;
    img->data = (uint8_t*)calloc(w * h * 3, sizeof(uint8_t));

    if (!img->data) {
        free(img);
        return NULL;
    }

    /* Fill with pseudo-random values to ensure non-zero coefficients */
    unsigned int seed = 12345;
    for (int i = 0; i < w * h * 3; i++) {
        seed = seed * 1103515245 + 12345;
        img->data[i] = (uint8_t)((seed >> 16) & 0xFF);
    }
    return img;
}

/* Create grayscale test image (RGB with R=G=B) for perfect reconstruction test */
jpeg_image_t* create_grayscale_image(int w, int h) {
    jpeg_image_t *img = (jpeg_image_t*)calloc(1, sizeof(jpeg_image_t));
    if (!img) return NULL;

    img->width = w;
    img->height = h;
    img->colorspace = JPEG_COLORSPACE_GRAYSCALE;
    img->data = (uint8_t*)calloc(w * h, sizeof(uint8_t));

    if (!img->data) {
        free(img);
        return NULL;
    }

    /* Fill with pseudo-random grayscale values */
    unsigned int seed = 54321;
    for (int i = 0; i < w * h; i++) {
        seed = seed * 1103515245 + 12345;
        img->data[i] = (uint8_t)((seed >> 16) & 0xFF);
    }
    return img;
}

/* Calculate PSNR for grayscale images */
double calc_psnr_gray(const jpeg_image_t *orig, const jpeg_image_t *recon) {
    if (!orig || !recon) return 0.0;
    if (orig->width != recon->width || orig->height != recon->height) return 0.0;
    
    /* For grayscale, compare only Y channel (reconstruct to RGB, take first component) */
    double mse = 0.0;
    int total = orig->width * orig->height;
    
    for (int i = 0; i < total; i++) {
        /* Original is grayscale (1 byte per pixel) */
        /* Reconstructed is RGB (3 bytes per pixel), take average of RGB as Y */
        int orig_val = orig->data[i];
        int recon_val = (recon->data[i*3] + recon->data[i*3+1] + recon->data[i*3+2]) / 3;
        double diff = (double)orig_val - (double)recon_val;
        mse += diff * diff;
    }
    mse /= total;
    
    if (mse < 1e-10) return 100.0;
    return 10.0 * log10(255.0 * 255.0 / mse);
}/* Calculate bitrate for grayscale (only Y channel) */
double calc_bitrate_gray(const jpeg_compressed_t *comp) {
    if (!comp) return 0.0;
    
    double total_bits = 0.0;
    int total_blocks = 0;
    
    /* Only Y channel for grayscale */
    for (int b = 0; b < comp->num_blocks_y; b++) {
        const int32_t *block = comp->y_quantized + (b * 64);
        
        /* Find last non-zero coefficient in zigzag order */
        int last_nonzero = -1;
        for (int i = 63; i >= 0; i--) {
            int zz_idx = zigzag[i];
            if (block[zz_idx] != 0) {
                last_nonzero = i;
                break;
            }
        }
        
        if (last_nonzero >= 0) {
            total_bits += (last_nonzero + 1) * 8.0;
        }
        total_blocks++;
    }
    
    int total_pixels = total_blocks * 64;
    return total_pixels > 0 ? total_bits / total_pixels : 0.0;
}

/* Test identity mode */
void test_identity_mode(void) {
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║         IDENTITY MODE VALIDATION TEST                ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    /* Test 1: Random noise image (should give max bitrate ~8 bpp) */
    printf("Test 1: 8x8 random noise image\n");
    printf("----------------------------------------\n");
    
    jpeg_image_t *img1 = create_random_image(8, 8);
    if (!img1) {
        printf("✗ Failed to create test image\n");
        return;
    }
    
    jpeg_params_t params1;
    params1.quality_factor = 1.0;
    params1.dct_method = JPEG_DCT_IDENTITY;
    params1.use_standard_tables = 1;
    params1.skip_quantization = 1;
    
    jpeg_compressed_t *comp1 = NULL;
    jpeg_error_t err1 = jpeg_compress(img1, &params1, &comp1);
    
    if (err1 != JPEG_SUCCESS) {
        printf("✗ Compression failed: %s\n", jpeg_error_string(err1));
        jpeg_free_image(img1);
        return;
    }
    
    jpeg_image_t *recon1 = NULL;
    err1 = jpeg_decompress(comp1, &recon1);
    
    if (err1 != JPEG_SUCCESS) {
        printf("✗ Decompression failed: %s\n", jpeg_error_string(err1));
        jpeg_free_compressed(comp1);
        jpeg_free_image(img1);
        return;
    }
    
    double psnr1 = calc_psnr(img1, recon1);
    double bitrate1 = calc_bitrate(comp1);
    
    printf("  Bitrate: %.4f bpp ", bitrate1);
    if (bitrate1 > 7.5) printf("✓ (Expected ≈ 8.0)\n");
    else printf("✗ (Expected ≈ 8.0)\n");
    
    printf("  PSNR: %.2f dB ", psnr1);
    if (psnr1 > 90.0) printf("✓ (Expected ∞)\n");
    else printf("✗ (Expected ∞)\n");
    
    jpeg_free_image(img1);
    jpeg_free_image(recon1);
    jpeg_free_compressed(comp1);
    
    /* Test 2: Larger random image */
    printf("\nTest 2: 64x64 random noise image\n");
    printf("----------------------------------------\n");
    
    jpeg_image_t *img2 = create_random_image(64, 64);
    if (!img2) {
        printf("✗ Failed to create test image\n");
        return;
    }
    
    jpeg_params_t params2;
    params2.quality_factor = 1.0;
    params2.dct_method = JPEG_DCT_IDENTITY;
    params2.use_standard_tables = 1;
    params2.skip_quantization = 1;
    
    jpeg_compressed_t *comp2 = NULL;
    jpeg_error_t err2 = jpeg_compress(img2, &params2, &comp2);
    
    if (err2 != JPEG_SUCCESS) {
        printf("✗ Compression failed: %s\n", jpeg_error_string(err2));
        jpeg_free_image(img2);
        return;
    }
    
    jpeg_image_t *recon2 = NULL;
    err2 = jpeg_decompress(comp2, &recon2);
    
    if (err2 != JPEG_SUCCESS) {
        printf("✗ Decompression failed: %s\n", jpeg_error_string(err2));
        jpeg_free_compressed(comp2);
        jpeg_free_image(img2);
        return;
    }
    
    double psnr2 = calc_psnr(img2, recon2);
    double bitrate2 = calc_bitrate(comp2);
    
    printf("  Bitrate: %.4f bpp ", bitrate2);
    if (bitrate2 > 7.0) printf("✓ (Expected ≈ 8.0)\n");
    else printf("✗ (Expected ≈ 8.0)\n");
    
    printf("  PSNR: %.2f dB ", psnr2);
    if (psnr2 > 90.0) printf("✓ (Expected ∞)\n");
    else printf("✗ (Expected ∞)\n");
    
    jpeg_free_image(img2);
    jpeg_free_image(recon2);
    jpeg_free_compressed(comp2);
    
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║             VALIDATION COMPLETE                       ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n");
    
    /* Test 3: Grayscale image for perfect reconstruction */
    printf("\nTest 3: 64x64 grayscale image (no color conversion loss)\n");
    printf("----------------------------------------\n");
    
    jpeg_image_t *img3 = create_grayscale_image(64, 64);
    if (!img3) {
        printf("✗ Failed to create test image\n");
        return;
    }
    
    jpeg_params_t params3;
    params3.quality_factor = 1.0;
    params3.dct_method = JPEG_DCT_IDENTITY;
    params3.use_standard_tables = 1;
    params3.skip_quantization = 1;
    
    jpeg_compressed_t *comp3 = NULL;
    jpeg_error_t err3 = jpeg_compress(img3, &params3, &comp3);
    
    if (err3 != JPEG_SUCCESS) {
        printf("✗ Compression failed: %s\n", jpeg_error_string(err3));
        jpeg_free_image(img3);
        return;
    }
    
    jpeg_image_t *recon3 = NULL;
    err3 = jpeg_decompress(comp3, &recon3);
    
    if (err3 != JPEG_SUCCESS) {
        printf("✗ Decompression failed: %s\n", jpeg_error_string(err3));
        jpeg_free_compressed(comp3);
        jpeg_free_image(img3);
        return;
    }
    
    double psnr3 = calc_psnr_gray(img3, recon3);
    double bitrate3 = calc_bitrate_gray(comp3);  /* Use grayscale bitrate */
    
    printf("  Bitrate: %.4f bpp ", bitrate3);
    if (bitrate3 > 7.0) printf("✓ (Expected ≈ 8.0)\n");
    else printf("✗ (Expected ≈ 8.0)\n");
    
    printf("  PSNR: %.2f dB ", psnr3);
    if (psnr3 > 90.0) printf("✓ (Perfect reconstruction)\n");
    else printf("✗ (Expected ∞)\n");
    
    jpeg_free_image(img3);
    jpeg_free_image(recon3);
    jpeg_free_compressed(comp3);
    
    printf("\n  Note: RGB images have ~43dB PSNR due to integer color conversion\n");
    printf("        (RGB→YCbCr→RGB rounding). Grayscale has perfect PSNR.\n");
}

/* Compare DCT methods */
void compare_methods(void) {
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║         DCT METHODS COMPARISON                        ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    /* Use random image to ensure all coefficients are non-zero */
    jpeg_image_t *img = create_random_image(64, 64);
    if (!img) {
        printf("✗ Failed to create test image\n");
        return;
    }
    
    const char *method_names[] = {"Loeffler", "Matrix", "Approximate", "Identity"};
    jpeg_dct_method_t methods[] = {
        JPEG_DCT_LOEFFLER, JPEG_DCT_MATRIX, 
        JPEG_DCT_APPROX, JPEG_DCT_IDENTITY
    };
    
    printf("%-12s | %10s | %10s\n", "Method", "PSNR (dB)", "Bitrate");
    printf("-------------|------------|------------\n");
    
    for (int m = 0; m < 4; m++) {
        jpeg_params_t params;
        params.quality_factor = 1.0;  /* Best quality */
        params.dct_method = methods[m];
        params.use_standard_tables = 1;
        params.skip_quantization = 1;  /* Skip quantization for fair comparison */
        
        jpeg_compressed_t *comp = NULL;
        jpeg_compress(img, &params, &comp);
        
        jpeg_image_t *recon = NULL;
        jpeg_decompress(comp, &recon);
        
        double psnr = calc_psnr(img, recon);
        double bitrate = calc_bitrate(comp);
        
        printf("%-12s | %10.2f | %10.4f\n", method_names[m], psnr, bitrate);
        
        jpeg_free_image(recon);
        jpeg_free_compressed(comp);
    }
    
    jpeg_free_image(img);
    printf("\n");
}

int main(void) {
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║     JPEG CODEC - VALIDATION & TESTING SUITE          ║\n");
    printf("║     Version: %s                                  ║\n", jpeg_version());
    printf("╚═══════════════════════════════════════════════════════╝\n");
    
    test_identity_mode();
    compare_methods();
    
    return 0;
}