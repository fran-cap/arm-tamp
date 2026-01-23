/**
 * ARM Cortex Optimized TAMP Decompressor
 *
 * ARM Cortex-specific optimizations:
 * - Completely flattened main loop (no helper function calls)
 * - Cache-aligned Huffman table (128 bytes)
 * - Combined bit buffer operations
 * - Minimal branching through arithmetic
 * - Fused copy loop optimization
 */

#include <string.h>
#include "decompressor_arm.h"
#include "common_arm.h"

#define FLUSH 15

/* Cache-aligned Huffman table */
static const uint8_t HUFFMAN_TABLE[128] __attribute__((aligned(128))) = {
    50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,  50,  85,  85,  85, 85, 122, 123, 104, 104, 86, 86,
    86, 86, 93, 93, 93, 93, 68, 68, 68, 68, 68, 68, 68, 68, 105, 105, 124, 127, 87, 87, 87,  87,  51,  51,  51, 51,
    51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 17, 17, 17,  17,  17,  17,  17, 17, 17,  17,  17,  17,  17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,  17,  17,  17,  17, 17, 17,  17,  17,  17,  17, 17,
    17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,  17,  17,  17,  17, 17, 17,  17,  17,  17
};

tamp_res tamp_decompressor_read_header_arm(TampConf *conf, const unsigned char *input, size_t input_size,
                                           size_t *input_consumed_size) {
    if (input_consumed_size) *input_consumed_size = 0;
    if (input_size == 0) return TAMP_INPUT_EXHAUSTED;
    if (input[0] & 0x3) return TAMP_INVALID_CONF;  /* Combined check for reserved and more_header */
    if (input_consumed_size) (*input_consumed_size)++;

    conf->window = ((input[0] >> 5) & 0x7) + 8;
    conf->literal = ((input[0] >> 3) & 0x3) + 5;
    conf->use_custom_dictionary = ((input[0] >> 2) & 0x1);

    return TAMP_OK;
}

static tamp_res tamp_decompressor_populate_from_conf(TampDecompressorArm *decompressor, uint8_t conf_window,
                                                     uint8_t conf_literal, uint8_t conf_use_custom_dictionary) {
    if (conf_window < 8 || conf_window > 15) return TAMP_INVALID_CONF;
    if (conf_literal < 5 || conf_literal > 8) return TAMP_INVALID_CONF;
    if (!conf_use_custom_dictionary)
        tamp_initialize_dictionary_arm(decompressor->window, (1 << conf_window));

    decompressor->conf_window = conf_window;
    decompressor->conf_literal = conf_literal;
    decompressor->min_pattern_size = tamp_compute_min_pattern_size_arm(conf_window, conf_literal);
    decompressor->configured = true;

    return TAMP_OK;
}

tamp_res tamp_decompressor_init_arm(TampDecompressorArm *decompressor, const TampConf *conf, unsigned char *window) {
    tamp_res res = TAMP_OK;
    memset(decompressor, 0, sizeof(TampDecompressorArm));
    decompressor->window = window;
    if (conf) {
        res = tamp_decompressor_populate_from_conf(decompressor, conf->window, conf->literal,
                                                   conf->use_custom_dictionary);
    }
    return res;
}

/**
 * ARM Main decompression - Completely flattened for maximum speed
 */
__attribute__((hot, flatten))
tamp_res tamp_decompressor_decompress_cb_arm(
    TampDecompressorArm *decompressor,
    unsigned char *output, size_t output_size, size_t *output_written_size,
    const unsigned char *input, size_t input_size, size_t *input_consumed_size,
    tamp_callback_t callback, void *user_data
) {
    size_t input_consumed_local = 0;
    size_t output_written_local = 0;
    tamp_res res;

    const unsigned char *__restrict__ in = input;
    const unsigned char *const in_end = input + input_size;
    unsigned char *__restrict__ out = output;
    const unsigned char *const out_end = output + output_size;

    /* Cache state in registers */
    uint32_t bb = decompressor->bit_buffer;
    uint32_t bbp = decompressor->bit_buffer_pos;
    uint32_t wpos = decompressor->window_pos;
    uint32_t skip = decompressor->skip_bytes;
    unsigned char *__restrict__ win = decompressor->window;

    /* Handle header */
    if (TAMP_UNLIKELY(!decompressor->configured)) {
        size_t header_consumed_size;
        TampConf conf;
        res = tamp_decompressor_read_header_arm(&conf, in, in_end - in, &header_consumed_size);
        if (res != TAMP_OK) goto cleanup;

        res = tamp_decompressor_populate_from_conf(decompressor, conf.window, conf.literal,
                                                   conf.use_custom_dictionary);
        if (res != TAMP_OK) goto cleanup;

        in += header_consumed_size;
        input_consumed_local += header_consumed_size;
        win = decompressor->window;
    }

    /* Precompute all constants */
    const uint32_t cwin = decompressor->conf_window;
    const uint32_t clit = decompressor->conf_literal;
    const uint32_t min_pat = decompressor->min_pattern_size;
    const uint32_t wmask = (1u << cwin) - 1;
    const uint32_t wsize = (1u << cwin);
    const uint32_t lit_bits = 1 + clit;
    const uint32_t cwin_shift = 32 - cwin;
    const uint32_t clit_shift = 32 - clit;

    /* Main loop */
    while (in < in_end || bbp) {
        /* Refill bit buffer at start of each iteration (matches standard) */
        while (in < in_end && bbp <= 24) {
            bb |= ((uint32_t)*in++) << (24 - bbp);
            bbp += 8;
            input_consumed_local++;
        }

        /* Exit checks */
        if (TAMP_UNLIKELY(bbp == 0)) {
            res = TAMP_INPUT_EXHAUSTED;
            goto cleanup;
        }
        if (TAMP_UNLIKELY(out >= out_end)) {
            res = TAMP_OUTPUT_FULL;
            goto cleanup;
        }

        /* 
         * ARM OPTIMIZATION: Branchless token/literal test
         * Use arithmetic to avoid branch misprediction
         */
        uint32_t is_literal = bb >> 31;
        
        if (TAMP_LIKELY(!is_literal)) {
            /* ===== TOKEN PATH ===== */
            uint32_t tbb = bb << 1;
            uint32_t tbbp = bbp - 1;

            if (TAMP_UNLIKELY(tbbp < 8)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            int32_t match_size;
            uint32_t huffman_bits;
            
            /* 
             * ARM OPTIMIZATION: Combine huffman decode cases
             * The most common case (match_size=0) still gets fast path
             */
            if (TAMP_LIKELY(!(tbb >> 31))) {
                match_size = 0;
                huffman_bits = 1;
            } else {
                uint32_t code = (tbb >> 24) & 0x7F;
                uint32_t entry = HUFFMAN_TABLE[code];
                huffman_bits = (entry >> 4) + 1;
                match_size = entry & 0xF;

                if (TAMP_UNLIKELY(match_size == FLUSH)) {
                    tbb <<= huffman_bits;
                    tbbp -= huffman_bits;
                    uint32_t discard = tbbp & 7;
                    bb = tbb << discard;
                    bbp = tbbp & ~7u;
                    
                    /* Refill after flush */
                    while (bbp <= 24 && in < in_end) {
                        bb |= ((uint32_t)*in++) << (24 - bbp);
                        bbp += 8;
                        input_consumed_local++;
                    }
                    continue;
                }
            }

            tbb <<= huffman_bits;
            tbbp -= huffman_bits;

            if (TAMP_UNLIKELY(tbbp < cwin)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            match_size += min_pat;
            uint32_t woff = tbb >> cwin_shift;

            if (TAMP_UNLIKELY(woff >= wsize || woff + (uint32_t)match_size > wsize)) {
                res = TAMP_OOB;
                goto cleanup;
            }

            int32_t ms_skip = match_size - (int32_t)skip;
            uint32_t woff_skip = woff + skip;

            size_t remaining = out_end - out;
            if (TAMP_UNLIKELY((uint32_t)ms_skip > remaining)) {
                skip += remaining;
                ms_skip = remaining;
            } else {
                skip = 0;
                bb = tbb << cwin;
                bbp = tbbp - cwin;
            }

            /* 
             * ARM OPTIMIZATION: Fused copy loop
             * Copy to output and window simultaneously when possible
             */
            const unsigned char *src = win + woff_skip;
            
            if (TAMP_LIKELY(skip == 0)) {
                /* Full decode - update window with full match */
                uint32_t safe_dist = (wpos >= woff) ? (wpos - woff) : (wpos + wsize - woff);

                /* Copy remaining bytes (ms_skip) to output from woff_skip */
                for (int32_t i = 0; i < ms_skip; i++) {
                    out[i] = src[i];
                }

                if (TAMP_LIKELY(safe_dist >= (uint32_t)match_size)) {
                    /* No overlap - direct window copy */
                    for (int32_t i = 0; i < match_size; i++) {
                        win[wpos] = win[woff + i];
                        wpos = (wpos + 1) & wmask;
                    }
                } else {
                    /* Overlap case - use temporary buffer for window update */
                    uint8_t tmp[16];
                    for (int32_t i = 0; i < match_size; i++) {
                        tmp[i] = win[woff + i];
                    }
                    for (int32_t i = 0; i < match_size; i++) {
                        win[wpos] = tmp[i];
                        wpos = (wpos + 1) & wmask;
                    }
                }
            } else {
                /* Partial decode - just copy to output */
                for (int32_t i = 0; i < ms_skip; i++) {
                    out[i] = src[i];
                }
            }
            
            out += ms_skip;
            output_written_local += ms_skip;

        } else {
            /* ===== LITERAL PATH ===== */
            if (TAMP_UNLIKELY(bbp < lit_bits)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            /* Combined shift operations */
            uint8_t literal = (bb << 1) >> clit_shift;
            bb <<= lit_bits;
            bbp -= lit_bits;

            /* Single write to both output and window */
            *out++ = literal;
            win[wpos] = literal;
            wpos = (wpos + 1) & wmask;
            output_written_local++;
        }

        /* Callback check */
        if (TAMP_UNLIKELY(callback != NULL)) {
            res = callback(user_data, output_written_local, input_size);
            if (res != 0) goto cleanup;
        }
    }

    res = TAMP_INPUT_EXHAUSTED;

cleanup:
    decompressor->bit_buffer = bb;
    decompressor->bit_buffer_pos = bbp;
    decompressor->window_pos = wpos;
    decompressor->skip_bytes = skip;

    if (output_written_size) *output_written_size = output_written_local;
    if (input_consumed_size) *input_consumed_size = input_consumed_local;

    return res;
}


// Non-inline wrapper for linking when header is not included

// Implementation of non-inline wrapper
tamp_res tamp_decompressor_decompress_arm(TampDecompressorArm *decompressor, unsigned char *output,
                                           size_t output_size, size_t *output_written_size,
                                           const unsigned char *input, size_t input_size,
                                           size_t *input_consumed_size) {
    return tamp_decompressor_decompress_cb_arm(decompressor, output, output_size, output_written_size,
                                               input, input_size, input_consumed_size, NULL, NULL);
}
