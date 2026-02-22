/**
 * @file example.c
 * @brief Practical usage example of JPEG Codec Library
 * 
 * Demonstrates:
 * - Image creation and compression
 * - Quality factor comparison
 * - DCT method comparison
 * - Memory management
 */

#include "jpeg_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Calculate PSNR between two images */
double calculate_psnr(const jpeg_image_t *orig, const jpeg_image_t *recon) {
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

/* Create a gradient test image */
jpeg_image_t* create_gradient_image(int width, int height) {
    jpeg_image_t *img = (jpeg_image_t*)calloc(1, sizeof(jpeg_image_t));
    if (!img) return NULL;
    
    img->width = width;
    img->height = height;
    img->colorspace = JPEG_COLORSPACE_RGB;
    img->data = (uint8_t*)calloc(width * height * 3, sizeof(uint8_t));
    
    if (!img->data) {
        free(img);
        return NULL;
    }
    
    /* Create horizontal gradient */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t value = (uint8_t)((x * 255) / (width - 1));
            int idx = (y * width + x) * 3;
            img->data[idx + 0] = value;  /* R */
            img->data[idx + 1] = value;  /* G */
            img->data[idx + 2] = value;  /* B */
        }
    }
    
    return img;
}

/* Example 1: Basic compression */
void example_basic_compression(void) {
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║        Example 1: Basic Compression                  ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    /* Create test image */
    jpeg_image_t *original = create_gradient_image(64, 64);
    if (!original) {
        printf("✗ Failed to create image\n");
        return;
    }
    printf("✓ Created 64x64 gradient image\n");
    
    /* Set compression parameters */
    jpeg_params_t params;
    params.quality_factor = 2.0;
    params.dct_method = JPEG_DCT_LOEFFLER;
    params.use_standard_tables = 1;
    params.skip_quantization = 0;
    
    /* Compress */
    jpeg_compressed_t *compressed = NULL;
    jpeg_error_t err = jpeg_compress(original, &params, &compressed);
    
    if (err != JPEG_SUCCESS) {
        printf("✗ Compression failed: %s\n", jpeg_error_string(err));
        jpeg_free_image(original);
        return;
    }
    printf("✓ Compressed with Loeffler DCT (k=%.1f)\n", params.quality_factor);
    
    /* Decompress */
    jpeg_image_t *reconstructed = NULL;
    err = jpeg_decompress(compressed, &reconstructed);
    
    if (err != JPEG_SUCCESS) {
        printf("✗ Decompression failed: %s\n", jpeg_error_string(err));
        jpeg_free_compressed(compressed);
        jpeg_free_image(original);
        return;
    }
    printf("✓ Decompressed successfully\n");
    
    /* Calculate quality */
    double psnr = calculate_psnr(original, reconstructed);
    printf("\nQuality metrics:\n");
    printf("  PSNR: %.2f dB\n", psnr);
    printf("  Blocks: %d\n", compressed->num_blocks_y);
    
    /* Cleanup */
    jpeg_free_image(original);
    jpeg_free_image(reconstructed);
    jpeg_free_compressed(compressed);
}

/* Example 2: Quality factor comparison */
void example_quality_comparison(void) {
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║        Example 2: Quality Factor Comparison          ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    jpeg_image_t *original = create_gradient_image(64, 64);
    if (!original) return;
    
    float k_values[] = {1.0, 2.0, 4.0, 8.0};
    
    printf("%-12s | %12s | %12s\n", "k Factor", "PSNR (dB)", "Quality");
    printf("-------------|--------------|-------------\n");
    
    for (int i = 0; i < 4; i++) {
        jpeg_params_t params;
        params.quality_factor = k_values[i];
        params.dct_method = JPEG_DCT_LOEFFLER;
        params.use_standard_tables = 1;
        params.skip_quantization = 0;
        
        jpeg_compressed_t *compressed = NULL;
        jpeg_compress(original, &params, &compressed);
        
        jpeg_image_t *reconstructed = NULL;
        jpeg_decompress(compressed, &reconstructed);
        
        double psnr = calculate_psnr(original, reconstructed);
        
        const char *quality;
        if (psnr > 40) quality = "Excellent";
        else if (psnr > 35) quality = "Good";
        else if (psnr > 30) quality = "Fair";
        else quality = "Poor";
        
        printf("%-12.1f | %12.2f | %12s\n", k_values[i], psnr, quality);
        
        jpeg_free_image(reconstructed);
        jpeg_free_compressed(compressed);
    }
    
    jpeg_free_image(original);
}

/* Example 3: DCT method comparison */
void example_method_comparison(void) {
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║        Example 3: DCT Method Comparison              ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    jpeg_image_t *original = create_gradient_image(64, 64);
    if (!original) return;
    
    const char *method_names[] = {"Loeffler", "Matrix", "Approximate"};
    jpeg_dct_method_t methods[] = {
        JPEG_DCT_LOEFFLER,
        JPEG_DCT_MATRIX,
        JPEG_DCT_APPROX
    };
    
    printf("%-12s | %12s | %15s\n", "Method", "PSNR (dB)", "Multiplications");
    printf("-------------|--------------|----------------\n");
    
    for (int i = 0; i < 3; i++) {
        jpeg_params_t params;
        params.quality_factor = 2.0;
        params.dct_method = methods[i];
        params.use_standard_tables = 1;
        params.skip_quantization = 0;
        
        jpeg_compressed_t *compressed = NULL;
        jpeg_compress(original, &params, &compressed);
        
        jpeg_image_t *reconstructed = NULL;
        jpeg_decompress(compressed, &reconstructed);
        
        double psnr = calculate_psnr(original, reconstructed);
        
        const char *mults;
        if (methods[i] == JPEG_DCT_LOEFFLER) mults = "11";
        else if (methods[i] == JPEG_DCT_MATRIX) mults = "64";
        else mults = "0";
        
        printf("%-12s | %12.2f | %15s\n", method_names[i], psnr, mults);
        
        jpeg_free_image(reconstructed);
        jpeg_free_compressed(compressed);
    }
    
    jpeg_free_image(original);
}

/* Example 4: Error handling */
void example_error_handling(void) {
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║        Example 4: Error Handling                     ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    /* Test 1: Null pointer */
    jpeg_compressed_t *comp = NULL;
    jpeg_error_t err = jpeg_compress(NULL, NULL, &comp);
    printf("Test 1 - Null pointer: %s ", jpeg_error_string(err));
    if (err == JPEG_ERROR_NULL_POINTER) printf("✓\n");
    else printf("✗\n");
    
    /* Test 2: Invalid dimensions */
    jpeg_image_t invalid_img;
    invalid_img.width = -1;
    invalid_img.height = 0;
    invalid_img.data = NULL;
    
    jpeg_params_t params;
    params.quality_factor = 2.0;
    params.dct_method = JPEG_DCT_LOEFFLER;
    
    err = jpeg_compress(&invalid_img, &params, &comp);
    printf("Test 2 - Invalid dimensions: %s ", jpeg_error_string(err));
    if (err == JPEG_ERROR_INVALID_DIMENSIONS) printf("✓\n");
    else printf("✗\n");
    
    /* Test 3: Successful operation */
    jpeg_image_t *valid_img = create_gradient_image(8, 8);
    err = jpeg_compress(valid_img, &params, &comp);
    printf("Test 3 - Valid compression: %s ", jpeg_error_string(err));
    if (err == JPEG_SUCCESS) printf("✓\n");
    else printf("✗\n");
    
    if (comp) jpeg_free_compressed(comp);
    jpeg_free_image(valid_img);
}

int main(void) {
    printf("╔═══════════════════════════════════════════════════════╗\n");
    printf("║     JPEG Codec Library - Usage Examples              ║\n");
    printf("║     Version: %s                                  ║\n", jpeg_version());
    printf("╚═══════════════════════════════════════════════════════╝\n");
    
    example_basic_compression();
    example_quality_comparison();
    example_method_comparison();
    example_error_handling();
    
    printf("\n╔═══════════════════════════════════════════════════════╗\n");
    printf("║                All Examples Complete                 ║\n");
    printf("╚═══════════════════════════════════════════════════════╝\n\n");
    
    return 0;
}