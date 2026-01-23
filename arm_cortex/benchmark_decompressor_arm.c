/**
 * ARM Cortex Decompressor A/B Benchmark
 *
 * Compares performance of standard TAMP decompressor against ARM Cortex
 * optimized version using identical compressed data.
 *
 * Usage:
 *   benchmark_arm <input_file>
 *   benchmark_arm build/enwik8-100kb       # Quick test
 *   benchmark_arm datasets/enwik8          # Full benchmark
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

// Standard TAMP library
#include "tamp/compressor.h"
#include "tamp/decompressor.h"

// ARM optimized version - uses separate struct type to avoid conflicts
#include "decompressor_arm.h"

#define WINDOW_BITS 10
#define WINDOW_SIZE (1 << WINDOW_BITS)

// Helper function to read entire file into memory
static unsigned char* read_file(const char *filename, size_t *size_out) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open file: %s\n", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fprintf(stderr, "ERROR: Cannot determine file size\n");
        fclose(f);
        return NULL;
    }

    unsigned char *data = malloc(fsize);
    if (!data) {
        fprintf(stderr, "ERROR: Cannot allocate %ld bytes\n", fsize);
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(data, 1, fsize, f);
    fclose(f);

    if (read_bytes != (size_t)fsize) {
        fprintf(stderr, "ERROR: Read %zu bytes, expected %ld\n", read_bytes, fsize);
        free(data);
        return NULL;
    }

    *size_out = fsize;
    return data;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s build/enwik8-100kb\n", argv[0]);
        fprintf(stderr, "  %s datasets/enwik8\n", argv[0]);
        return 1;
    }

    const char *input_file = argv[1];
    int exit_code = 1;

    // Allocate buffers
    unsigned char *input_data = NULL;
    unsigned char *compressed_data = NULL;
    unsigned char *std_output = NULL;
    unsigned char *arm_output = NULL;
    unsigned char comp_window[WINDOW_SIZE];
    unsigned char std_window[WINDOW_SIZE];
    unsigned char arm_window[WINDOW_SIZE];

    size_t input_size = 0;
    size_t compressed_size = 0;
    size_t std_output_size = 0;
    size_t arm_output_size = 0;

    printf("=== ARM Cortex Decompressor A/B Benchmark ===\n");
    printf("Input: %s\n", input_file);
    printf("Window: %u bytes (%u bits)\n\n", WINDOW_SIZE, WINDOW_BITS);

    // Step 1: Read input file
    input_data = read_file(input_file, &input_size);
    if (!input_data) {
        goto cleanup;
    }

    // Allocate buffers
    compressed_data = malloc(input_size + 256);
    std_output = malloc(input_size);
    arm_output = malloc(input_size);
    if (!compressed_data || !std_output || !arm_output) {
        fprintf(stderr, "ERROR: Cannot allocate buffers\n");
        goto cleanup;
    }

    // Step 2: Compress input data
    printf("[1/4] Compressing input data... ");
    fflush(stdout);

    TampCompressor compressor;
    TampConf conf = {
        .window = WINDOW_BITS,
        .literal = 8,
        .use_custom_dictionary = false
    };

    tamp_compressor_init(&compressor, &conf, comp_window);

    size_t input_consumed, output_written;
    tamp_res res = tamp_compressor_compress_and_flush(&compressor, compressed_data, input_size + 256,
                                                       &output_written, input_data, input_size,
                                                       &input_consumed, true);

    if (res < TAMP_OK || input_consumed != input_size) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: Compression failed (res=%d, consumed=%zu/%zu)\n",
                res, input_consumed, input_size);
        goto cleanup;
    }

    compressed_size = output_written;
    double compression_ratio = 100.0 * compressed_size / input_size;
    printf("OK (%.1f%% ratio)\n", compression_ratio);

    // Step 3: Baseline - Standard decompressor
    printf("[2/4] Baseline: Standard decompressor... ");
    fflush(stdout);

    TampDecompressor std_decompressor;
    tamp_decompressor_init(&std_decompressor, NULL, std_window, WINDOW_BITS);

    clock_t std_start = clock();

    res = tamp_decompressor_decompress(&std_decompressor, std_output, input_size,
                                       &std_output_size, compressed_data, compressed_size,
                                       &input_consumed);

    clock_t std_end = clock();
    double std_time = (double)(std_end - std_start) / CLOCKS_PER_SEC;

    if (res < TAMP_OK) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: Standard decompression failed (res=%d)\n", res);
        goto cleanup;
    }

    double std_throughput = (std_output_size / 1024.0 / 1024.0) / std_time;
    printf("%.3fs (%.2f MB/s)\n", std_time, std_throughput);

    // Step 4: Testing - ARM decompressor
    printf("[3/4] Testing: ARM decompressor... ");
    fflush(stdout);

    TampDecompressorArm arm_decompressor;
    tamp_decompressor_init_arm(&arm_decompressor, NULL, arm_window);

    clock_t arm_start = clock();

    res = tamp_decompressor_decompress_arm(&arm_decompressor, arm_output, input_size,
                                           &arm_output_size, compressed_data, compressed_size,
                                           &input_consumed);

    clock_t arm_end = clock();
    double arm_time = (double)(arm_end - arm_start) / CLOCKS_PER_SEC;

    if (res < TAMP_OK) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: ARM decompression failed (res=%d)\n", res);
        goto cleanup;
    }

    double arm_throughput = (arm_output_size / 1024.0 / 1024.0) / arm_time;
    printf("%.3fs (%.2f MB/s)\n", arm_time, arm_throughput);

    // Step 5: Verification
    printf("[4/4] Verification... ");
    fflush(stdout);

    bool verified = true;

    // Verify standard output matches original
    if (std_output_size != input_size) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: Standard output size (%zu) doesn't match input (%zu)\n",
                std_output_size, input_size);
        verified = false;
    } else if (memcmp(std_output, input_data, input_size) != 0) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: Standard output doesn't match original\n");
        verified = false;
    }

    // Verify ARM output matches original
    if (verified && arm_output_size != input_size) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: ARM output size (%zu) doesn't match input (%zu)\n",
                arm_output_size, input_size);
        verified = false;
    } else if (verified && memcmp(arm_output, input_data, input_size) != 0) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: ARM output doesn't match original\n");
        verified = false;
    }

    // Verify both outputs match each other
    if (verified && memcmp(std_output, arm_output, input_size) != 0) {
        fprintf(stderr, "FAILED\n");
        fprintf(stderr, "ERROR: Standard and ARM outputs don't match!\n");

        // Find first difference
        for (size_t i = 0; i < input_size; i++) {
            if (std_output[i] != arm_output[i]) {
                fprintf(stderr, "       First difference at byte %zu: std=0x%02x, arm=0x%02x\n",
                        i, std_output[i], arm_output[i]);
                break;
            }
        }
        verified = false;
    }

    if (verified) {
        printf("PASS %s\n", "\xE2\x9C\x93");  // UTF-8 checkmark
    }

    // Print results
    printf("\n=== Results ===\n");
    printf("Input size:      %zu bytes\n", input_size);
    printf("Compressed size: %zu bytes (%.1f%%)\n\n", compressed_size, compression_ratio);

    double speedup = std_time / arm_time;
    printf("Standard:  %.3fs (%.2f MB/s)\n", std_time, std_throughput);
    printf("ARM:       %.3fs (%.2f MB/s)\n", arm_time, arm_throughput);
    printf("Speedup:   %.2fx\n\n", speedup);

    printf("Status:    %s\n", verified ? "PASS \xE2\x9C\x93" : "FAIL \xE2\x9C\x97");

    exit_code = verified ? 0 : 1;

cleanup:
    free(input_data);
    free(compressed_data);
    free(std_output);
    free(arm_output);

    return exit_code;
}
