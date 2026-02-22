/**
 * @file process_images.c
 * @brief Process BMP images with JPEG Codec Library
 */

#include "../include/jpeg_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* BMP file structures */
#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint32_t size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t offset;
} BMPHeader;

typedef struct {
    uint32_t size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bits;
    uint32_t compression;
    uint32_t imagesize;
    int32_t xresolution;
    int32_t yresolution;
    uint32_t ncolours;
    uint32_t importantcolours;
} BMPInfoHeader;
#pragma pack(pop)

/* Load BMP image */
jpeg_image_t* load_bmp(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "ERROR: Cannot open %s\n", filename);
        return NULL;
    }
    
    BMPHeader header;
    if (fread(&header, sizeof(BMPHeader), 1, file) != 1 || header.type != 0x4D42) {
        fprintf(stderr, "ERROR: Invalid BMP file\n");
        fclose(file);
        return NULL;
    }
    
    BMPInfoHeader info;
    if (fread(&info, sizeof(BMPInfoHeader), 1, file) != 1 || info.bits != 24) {
        fprintf(stderr, "ERROR: Only 24-bit BMP supported\n");
        fclose(file);
        return NULL;
    }
    
    printf("  Image: %dx%d, %d bits\n", info.width, info.height, info.bits);
    
    jpeg_image_t *image = malloc(sizeof(jpeg_image_t));
    if (!image) {
        fclose(file);
        return NULL;
    }
    
    image->width = info.width;
    image->height = info.height;
    image->colorspace = JPEG_COLORSPACE_RGB;
    image->data = malloc(info.width * info.height * 3);
    
    if (!image->data) {
        free(image);
        fclose(file);
        return NULL;
    }
    
    fseek(file, header.offset, SEEK_SET);
    
    int row_size = ((info.width * 3 + 3) / 4) * 4;
    uint8_t *row_buffer = malloc(row_size);
    
    for (int y = info.height - 1; y >= 0; y--) {
        if (fread(row_buffer, 1, row_size, file) != (size_t)row_size) {
            fprintf(stderr, "ERROR: Failed to read image data\n");
            free(row_buffer);
            free(image->data);
            free(image);
            fclose(file);
            return NULL;
        }
        
        for (int x = 0; x < info.width; x++) {
            int src = x * 3;
            int dst = (y * info.width + x) * 3;
            image->data[dst + 0] = row_buffer[src + 2];  /* R */
            image->data[dst + 1] = row_buffer[src + 1];  /* G */
            image->data[dst + 2] = row_buffer[src + 0];  /* B */
        }
    }
    
    free(row_buffer);
    fclose(file);
    return image;
}

/* Save BMP image */
int save_bmp(const char *filename, const jpeg_image_t *image) {
    FILE *file = fopen(filename, "wb");
    if (!file) return -1;
    
    int row_size = ((image->width * 3 + 3) / 4) * 4;
    int image_size = row_size * image->height;
    
    BMPHeader header = {0};
    header.type = 0x4D42;
    header.size = sizeof(BMPHeader) + sizeof(BMPInfoHeader) + image_size;
    header.offset = sizeof(BMPHeader) + sizeof(BMPInfoHeader);
    fwrite(&header, sizeof(BMPHeader), 1, file);
    
    BMPInfoHeader info = {0};
    info.size = sizeof(BMPInfoHeader);
    info.width = image->width;
    info.height = image->height;
    info.planes = 1;
    info.bits = 24;
    info.compression = 0;
    info.imagesize = image_size;
    fwrite(&info, sizeof(BMPInfoHeader), 1, file);
    
    uint8_t *row_buffer = calloc(row_size, 1);
    
    for (int y = image->height - 1; y >= 0; y--) {
        for (int x = 0; x < image->width; x++) {
            int src = (y * image->width + x) * 3;
            int dst = x * 3;
            row_buffer[dst + 0] = image->data[src + 2];  /* B */
            row_buffer[dst + 1] = image->data[src + 1];  /* G */
            row_buffer[dst + 2] = image->data[src + 0];  /* R */
        }
        fwrite(row_buffer, 1, row_size, file);
    }
    
    free(row_buffer);
    fclose(file);
    return 0;
}

/* Calculate PSNR */
double calculate_psnr(const jpeg_image_t *orig, const jpeg_image_t *recon) {
    if (!orig || !recon || orig->width != recon->width || orig->height != recon->height)
        return 0.0;
    
    double mse = 0.0;
    int total = orig->width * orig->height * 3;
    
    for (int i = 0; i < total; i++) {
        double diff = (double)orig->data[i] - (double)recon->data[i];
        mse += diff * diff;
    }
    mse /= total;
    
    return (mse < 1e-10) ? 100.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

/* Calculate SSIM channel */
double calculate_ssim_channel(const uint8_t *orig, const uint8_t *recon, 
                              int width, int height, int stride) {
    const double C1 = 6.5025, C2 = 58.5225;
    const int ws = 7, hw = 3;
    double ssim_sum = 0.0;
    int count = 0;
    
    for (int y = hw; y < height - hw; y++) {
        for (int x = hw; x < width - hw; x++) {
            double mo = 0.0, mr = 0.0, vo = 0.0, vr = 0.0, cov = 0.0;
            int wc = 0;
            
            for (int wy = -hw; wy <= hw; wy++) {
                for (int wx = -hw; wx <= hw; wx++) {
                    int idx = ((y + wy) * width + (x + wx)) * stride;
                    mo += (double)orig[idx];
                    mr += (double)recon[idx];
                    wc++;
                }
            }
            mo /= wc; mr /= wc;
            
            for (int wy = -hw; wy <= hw; wy++) {
                for (int wx = -hw; wx <= hw; wx++) {
                    int idx = ((y + wy) * width + (x + wx)) * stride;
                    double do_ = (double)orig[idx] - mo;
                    double dr = (double)recon[idx] - mr;
                    vo += do_ * do_;
                    vr += dr * dr;
                    cov += do_ * dr;
                }
            }
            vo /= (wc - 1); vr /= (wc - 1); cov /= (wc - 1);
            
            ssim_sum += (2.0 * mo * mr + C1) * (2.0 * cov + C2) /
                       ((mo*mo + mr*mr + C1) * (vo + vr + C2));
            count++;
        }
    }
    return count > 0 ? ssim_sum / count : 1.0;
}

/* Calculate SSIM */
double calculate_ssim(const jpeg_image_t *orig, const jpeg_image_t *recon) {
    if (!orig || !recon || orig->width != recon->width || orig->height != recon->height)
        return 0.0;
    
    double ssim_r = calculate_ssim_channel(orig->data + 0, recon->data + 0, 
                                          orig->width, orig->height, 3);
    double ssim_g = calculate_ssim_channel(orig->data + 1, recon->data + 1, 
                                          orig->width, orig->height, 3);
    double ssim_b = calculate_ssim_channel(orig->data + 2, recon->data + 2, 
                                          orig->width, orig->height, 3);
    
    return (ssim_r + ssim_g + ssim_b) / 3.0;
}

/* Calculate bitrate using last non-zero in zigzag order */
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

double calculate_bitrate(const jpeg_compressed_t *comp) {
    if (!comp) return 0.0;
    
    double total_bits = 0.0;
    int total_blocks = 0;
    
    const int32_t *channels[3] = {comp->y_quantized, comp->cb_quantized, comp->cr_quantized};
    int blocks[3] = {comp->num_blocks_y, comp->num_blocks_chroma, comp->num_blocks_chroma};
    
    for (int ch = 0; ch < 3; ch++) {
        for (int b = 0; b < blocks[ch]; b++) {
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
            
            /* Bits = (last_nonzero + 1) * 8 bits per coefficient */
            if (last_nonzero >= 0) {
                total_bits += (last_nonzero + 1) * 8.0;
            }
            total_blocks++;
        }
    }
    
    /* bpp = total_bits / total_pixels (each block = 64 pixels) */
    int total_pixels = total_blocks * 64;
    return total_pixels > 0 ? total_bits / total_pixels : 0.0;
}

/* Image result structure */
typedef struct {
    char filename[256];
    int width, height;
    double psnr, ssim, bitrate;
} ImageResult;

/* Process one image */
void process_image(const char *path, const char *outdir, float k,
                   jpeg_dct_method_t method, ImageResult *result) {
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    
    printf("\n┌─────────────────────────────────────────────────────────┐\n");
    printf("│ Processing: %-43s │\n", basename);
    printf("└─────────────────────────────────────────────────────────┘\n");
    
    if (result) {
        strncpy(result->filename, basename, sizeof(result->filename) - 1);
        result->filename[sizeof(result->filename) - 1] = '\0';
    }
    
    printf("  [1/5] Loading BMP...\n");
    jpeg_image_t *orig = load_bmp(path);
    if (!orig) {
        printf("  ✗ Failed\n");
        if (result) result->psnr = result->ssim = result->bitrate = -1.0;
        return;
    }
    printf("  ✓ %dx%d (%d bytes)\n", orig->width, orig->height, 
           orig->width * orig->height * 3);
    
    if (result) {
        result->width = orig->width;
        result->height = orig->height;
    }
    
    printf("\n  [2/5] Compressing (k=%.1f)...\n", k);
    jpeg_params_t params;
    params.quality_factor = k;
    params.dct_method = method;
    params.use_standard_tables = 1;
    params.skip_quantization = (method == JPEG_DCT_IDENTITY) ? 1 : 0;
    
    jpeg_compressed_t *comp = NULL;
    jpeg_error_t err = jpeg_compress(orig, &params, &comp);
    
    if (err != JPEG_SUCCESS) {
        printf("  ✗ %s\n", jpeg_error_string(err));
        jpeg_free_image(orig);
        return;
    }
    printf("  ✓ Success (%d blocks)\n", comp->num_blocks_y);
    
    printf("\n  [3/5] Decompressing...\n");
    jpeg_image_t *recon = NULL;
    err = jpeg_decompress(comp, &recon);
    
    if (err != JPEG_SUCCESS) {
        printf("  ✗ %s\n", jpeg_error_string(err));
        jpeg_free_compressed(comp);
        jpeg_free_image(orig);
        return;
    }
    printf("  ✓ Success\n");
    
    printf("\n  [4/5] Calculating metrics...\n");
    double psnr = calculate_psnr(orig, recon);
    double ssim = calculate_ssim(orig, recon);
    double bitrate = calculate_bitrate(comp);
    
    if (result) {
        result->psnr = psnr;
        result->ssim = ssim;
        result->bitrate = bitrate;
    }
    
    printf("  ✓ PSNR: %.2f dB %s\n", psnr, 
           psnr > 40 ? "(Excellent)" : psnr > 30 ? "(Good)" : psnr > 20 ? "(Fair)" : "(Poor)");
    printf("  ✓ SSIM: %.4f\n", ssim);
    printf("  ✓ Bitrate: %.3f bpp\n", bitrate);
    
    printf("\n  [5/5] Saving...\n");
    char outfile[512];
    snprintf(outfile, sizeof(outfile), "%s/%s_k%.0f.bmp", outdir, basename, k);
    
    if (save_bmp(outfile, recon) == 0) {
        printf("  ✓ %s\n", outfile);
    } else {
        printf("  ✗ Failed to save\n");
    }
    
    printf("\n  Summary: %dx%d | k=%.1f | PSNR=%.2f dB | SSIM=%.4f | %.3f bpp\n",
           orig->width, orig->height, k, psnr, ssim, bitrate);
    
    jpeg_free_image(orig);
    jpeg_free_image(recon);
    jpeg_free_compressed(comp);
}

/* Process batch */
void process_batch(const char *images[], int n, float k, 
                   jpeg_dct_method_t method, const char *name) {
    char outdir[256];
    const char *method_str[] = {"loeffler", "matrix", "approx", "identity"};
    snprintf(outdir, sizeof(outdir), "example/output_%s_k%.0f", method_str[method], k);
    
    printf("\n╔═══════════════════════════════════════════════════════════╗\n");
    printf("║  Processing: %-44s ║\n", name);
    printf("╚═══════════════════════════════════════════════════════════╝\n");
    
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", outdir);
    system(cmd);
    
    printf("Output: %s/\n", outdir);
    printf("Images: %d\n", n);
    
    ImageResult *results = malloc(n * sizeof(ImageResult));
    if (!results) {
        fprintf(stderr, "ERROR: Memory allocation failed\n");
        return;
    }
    
    for (int i = 0; i < n; i++) {
        process_image(images[i], outdir, k, method, &results[i]);
    }
    
    char resfile[300];
    snprintf(resfile, sizeof(resfile), "%s/results.txt", outdir);
    FILE *f = fopen(resfile, "w");
    if (f) {
        fprintf(f, "═══════════════════════════════════════════════════════════════════════════════════\n");
        fprintf(f, "   JPEG Codec Library - Results\n");
        fprintf(f, "═══════════════════════════════════════════════════════════════════════════════════\n");
        fprintf(f, "Version: %s\n", jpeg_version());
        fprintf(f, "Method: %s\n", name);
        fprintf(f, "Quality: %.1f (1.0=high, 8.0=low)\n\n", k);
        fprintf(f, "%-35s %10s %10s %12s %10s %12s\n", 
               "Image", "Width", "Height", "PSNR (dB)", "SSIM", "Bitrate (bpp)");
        fprintf(f, "───────────────────────────────────────────────────────────────────────────────────\n");
        
        for (int i = 0; i < n; i++) {
            if (results[i].psnr >= 0) {
                fprintf(f, "%-35s %10d %10d %12.2f %10.4f %12.3f\n", 
                       results[i].filename, results[i].width, results[i].height,
                       results[i].psnr, results[i].ssim, results[i].bitrate);
            }
        }
        
        fprintf(f, "═══════════════════════════════════════════════════════════════════════════════════\n");
        fclose(f);
        printf("\n✓ Results: %s\n", resfile);
    }
    
    free(results);
}

int main(int argc, char **argv) {
    printf("═══════════════════════════════════════════════════════════\n");
    printf("   JPEG Codec Library - Image Processing\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("Version: %s\n\n", jpeg_version());
    
    float k = (argc >= 2) ? atof(argv[1]) : 2.0;
    printf("Quality: %.1f (1.0=high, 8.0=low)\n\n", k);
    
    const char *images[] = {
        "example/imgs/fruits.bmp",
        "example/imgs/monarch.bmp",
        "example/imgs/pens.bmp",
        "example/imgs/yacht.bmp",
        "example/imgs/estatua-da-liberdade.bmp",
        "example/imgs/marco-zero.bmp",
        "example/imgs/muralha-da-china.bmp",
        "example/imgs/torre-de-pisa.bmp"
    };
    int n = sizeof(images) / sizeof(images[0]);
    
    /* Process with all methods */
    process_batch(images, n, k, JPEG_DCT_LOEFFLER, "Loeffler (11 mults)");
    process_batch(images, n, k, JPEG_DCT_MATRIX, "Matrix (64 mults)");
    process_batch(images, n, k, JPEG_DCT_APPROX, "Approximate (0 mults)");
    process_batch(images, n, 1.0, JPEG_DCT_IDENTITY, "Identity (validation)");
    
    printf("\n═══════════════════════════════════════════════════════════\n");
    printf("   Processing Complete!\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("\n✓ example/output_loeffler_k%.0f/\n", k);
    printf("✓ example/output_matrix_k%.0f/\n", k);
    printf("✓ example/output_approx_k%.0f/\n", k);
    printf("✓ example/output_identity_k1/\n\n");
    
    printf("Usage: ./process_images [quality]\n");
    printf("  1.0 = High quality\n");
    printf("  2.0 = Medium (default)\n");
    printf("  4.0 = Low quality\n");
    printf("  8.0 = Very low quality\n\n");
    
    return 0;
}