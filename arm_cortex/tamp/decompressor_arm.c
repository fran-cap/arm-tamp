/**
 * ARM Cortex Optimized TAMP Decompressor
 *
 * Optimizations over standard decompressor:
 * - Flattened main loop with no helper function calls
 * - Bit buffer and window position cached in registers
 * - LIKELY/UNLIKELY hints for branch prediction
 * - Fused output + window copy loops
 * - Cache-aligned Huffman lookup table
 *
 * See arm_cortex/README.md for benchmark results and optimization history.
 */

#include <string.h>
#include "decompressor_arm.h"
#include "common_arm.h"

#define FLUSH 15

/**
 * Huffman decode table for match sizes 1-13 (plus FLUSH=15).
 * Indexed by 7 bits from the bit stream.
 *
 * Entry format: (huffman_bits << 4) | match_size
 *   - huffman_bits: actual number of bits in this code (saves +1 operation)
 *   - match_size: decoded value (0-13, or 15 for FLUSH)
 *
 * Aligned to 128 bytes for cache efficiency.
 */
static const uint8_t HUFFMAN_TABLE[128] __attribute__((aligned(128))) = {
    66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66, 66,       /* ms=2, bits=4 */
    101, 101, 101, 101, 138, 139, 120, 120, 102, 102, 102, 102, 109, 109, 109, 109,
    84, 84, 84, 84, 84, 84, 84, 84, 121, 121, 140, 143, 103, 103, 103, 103,
    67, 67, 67, 67, 67, 67, 67, 67, 67, 67, 67, 67, 67, 67, 67, 67,       /* ms=3, bits=4 */
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,       /* ms=1, bits=2 */
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33
};

/*============================================================================
 * Header and Initialization
 *============================================================================*/

tamp_res tamp_decompressor_read_header_arm(
    TampConf *conf,
    const unsigned char *input,
    size_t input_size,
    size_t *input_consumed_size
) {
    if (input_consumed_size) *input_consumed_size = 0;
    if (input_size == 0) return TAMP_INPUT_EXHAUSTED;

    /* Bits 0-1 must be 0 (reserved + more_header flags) */
    if (input[0] & 0x3) return TAMP_INVALID_CONF;

    conf->window = ((input[0] >> 5) & 0x7) + 8;
    conf->literal = ((input[0] >> 3) & 0x3) + 5;
    conf->use_custom_dictionary = ((input[0] >> 2) & 0x1);

    if (input_consumed_size) (*input_consumed_size)++;
    return TAMP_OK;
}

static tamp_res tamp_decompressor_populate_from_conf(
    TampDecompressorArm *decompressor,
    uint8_t conf_window,
    uint8_t conf_literal,
    uint8_t conf_use_custom_dictionary
) {
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

tamp_res tamp_decompressor_init_arm(
    TampDecompressorArm *decompressor,
    const TampConf *conf,
    unsigned char *window
) {
    memset(decompressor, 0, sizeof(TampDecompressorArm));
    decompressor->window = window;

    if (conf) {
        return tamp_decompressor_populate_from_conf(
            decompressor, conf->window, conf->literal, conf->use_custom_dictionary
        );
    }
    return TAMP_OK;
}

/*============================================================================
 * Main Decompression Loop
 *============================================================================*/

__attribute__((hot, flatten))
tamp_res tamp_decompressor_decompress_cb_arm(
    TampDecompressorArm *decompressor,
    unsigned char *output,
    size_t output_size,
    size_t *output_written_size,
    const unsigned char *input,
    size_t input_size,
    size_t *input_consumed_size,
    tamp_callback_t callback,
    void *user_data
) {
    tamp_res res;

    /* Input/output pointers */
    const unsigned char *__restrict__ in = input;
    const unsigned char *const in_end = input + input_size;
    unsigned char *__restrict__ out = output;
    const unsigned char *const out_end = output + output_size;

    /* Cache decompressor state in registers */
    uint32_t bb = decompressor->bit_buffer;      /* MSB-first bit buffer */
    uint32_t bbp = decompressor->bit_buffer_pos; /* Valid bits in buffer */
    uint32_t wpos = decompressor->window_pos;    /* Current window write position */
    uint32_t skip = decompressor->skip_bytes;    /* Bytes to skip (partial token resume) */
    unsigned char *__restrict__ win = decompressor->window;

    /* Handle header if not yet configured */
    if (TAMP_UNLIKELY(!decompressor->configured)) {
        size_t header_consumed_size;
        TampConf conf;
        res = tamp_decompressor_read_header_arm(&conf, in, in_end - in, &header_consumed_size);
        if (res != TAMP_OK) goto cleanup;

        res = tamp_decompressor_populate_from_conf(
            decompressor, conf.window, conf.literal, conf.use_custom_dictionary
        );
        if (res != TAMP_OK) goto cleanup;

        in += header_consumed_size;
        win = decompressor->window;
    }

    /* Precompute constants from configuration */
    const uint32_t cwin = decompressor->conf_window;
    const uint32_t clit = decompressor->conf_literal;
    const uint32_t min_pat = decompressor->min_pattern_size;
    const uint32_t wmask = (1u << cwin) - 1;   /* Window index mask */
    const uint32_t wsize = (1u << cwin);       /* Window size */
    const uint32_t lit_bits = 1 + clit;        /* Bits per literal (flag + value) */
    const uint32_t cwin_shift = 32 - cwin;     /* Shift to extract window offset */
    const uint32_t clit_shift = 32 - clit;     /* Shift to extract literal value */

    /*
     * Main decode loop
     *
     * Bit stream format (MSB-first):
     *   0 + 0 + <cwin bits>              = Token, match_size=0
     *   0 + 1 + <huffman> + <cwin bits>  = Token, match_size from Huffman table
     *   1 + <clit bits>                  = Literal
     */
    while (in < in_end || bbp) {
        /* Refill bit buffer - keep at least 24 bits available */
        while (in < in_end && bbp <= 24) {
            bb |= ((uint32_t)*in++) << (24 - bbp);
            bbp += 8;
        }

        /* Check for input/output exhaustion */
        if (TAMP_UNLIKELY(bbp == 0)) {
            res = TAMP_INPUT_EXHAUSTED;
            goto cleanup;
        }
        if (TAMP_UNLIKELY(out >= out_end)) {
            res = TAMP_OUTPUT_FULL;
            goto cleanup;
        }

        /* Decode token or literal based on MSB */
        if (TAMP_LIKELY(!(bb >> 31))) {
            /*
             * TOKEN PATH
             * Format: 0 + match_size_flag + [huffman_code] + window_offset
             */
            uint32_t tbb = bb << 1;   /* Consume token flag bit */
            uint32_t tbbp = bbp - 1;

            if (TAMP_UNLIKELY(tbbp < 8)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            uint32_t match_size;
            uint32_t huffman_bits;

            /* Check match_size flag (now at MSB of tbb) */
            if (TAMP_LIKELY(!(tbb >> 31))) {
                /* Fast path: match_size = 0 (most common) */
                match_size = 0;
                huffman_bits = 1;
            } else {
                /* Decode match_size from Huffman table */
                uint32_t entry = HUFFMAN_TABLE[(tbb >> 24) & 0x7F];
                huffman_bits = entry >> 4;  /* Table stores actual bits (saves +1 op) */
                match_size = entry & 0xF;

                /* Handle FLUSH token (byte-align the bit buffer) */
                if (TAMP_UNLIKELY(match_size == FLUSH)) {
                    tbb <<= huffman_bits;
                    tbbp -= huffman_bits;
                    uint32_t discard = tbbp & 7;
                    bb = tbb << discard;
                    bbp = tbbp & ~7u;
                    continue;
                }
            }

            /* Consume Huffman bits */
            tbb <<= huffman_bits;
            tbbp -= huffman_bits;

            if (TAMP_UNLIKELY(tbbp < cwin)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            /* Extract window offset and compute final match size */
            match_size += min_pat;
            const uint32_t woff = tbb >> cwin_shift;

            /* Bounds check: woff < wsize by construction, check copy range */
            if (TAMP_UNLIKELY(woff + match_size > wsize)) {
                res = TAMP_OOB;
                goto cleanup;
            }

            /* Handle partial token from previous call (output buffer was full) */
            uint32_t ms_skip = match_size - skip;
            const uint32_t woff_skip = woff + skip;

            const size_t remaining = out_end - out;
            if (TAMP_UNLIKELY(ms_skip > remaining)) {
                /* Can't complete this token - save state for next call */
                skip += remaining;
                ms_skip = remaining;
            } else {
                /* Token complete - consume bits */
                skip = 0;
                bb = tbb << cwin;
                bbp = tbbp - cwin;
            }

            /* Copy from window to output, updating window as we go */
            if (TAMP_LIKELY(skip == 0)) {
                const uint32_t dist = (wpos - woff) & wmask;

                if (TAMP_LIKELY(dist >= match_size)) {
                    /* No overlap: copy directly */
                    const unsigned char *__restrict__ src = win + woff;
                    unsigned char *__restrict__ dst_out = out;
                    uint32_t count = match_size;

                    while (count--) {
                        uint32_t c = *src++;  /* Use uint32_t to avoid char-to-int conversion */
                        *dst_out++ = c;
                        win[wpos] = c;
                        wpos = (wpos + 1) & wmask;
                    }
                } else {
                    /* Overlap: snapshot source first, then copy */
                    uint32_t tmp[16];  /* Use uint32_t to avoid char arithmetic overhead */
                    const unsigned char *src = win + woff;
                    for (uint32_t i = 0; i < match_size; i++) {
                        tmp[i] = src[i];
                    }
                    for (uint32_t i = 0; i < match_size; i++) {
                        uint32_t c = tmp[i];
                        out[i] = c;
                        win[wpos] = c;
                        wpos = (wpos + 1) & wmask;
                    }
                }
            } else {
                /* Partial copy for output-limited case */
                const unsigned char *__restrict__ src = win + woff_skip;
                for (uint32_t i = 0; i < ms_skip; i++) {
                    out[i] = src[i];
                }
            }

            out += ms_skip;

        } else {
            /*
             * LITERAL PATH
             * Format: 1 + <clit bits of literal value>
             */
            if (TAMP_UNLIKELY(bbp < lit_bits)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            /* Use uint32_t to avoid char-to-int conversion overhead */
            const uint32_t literal = (bb << 1) >> clit_shift;
            bb <<= lit_bits;
            bbp -= lit_bits;

            /* Write to output and window */
            *out++ = literal;
            win[wpos] = literal;
            wpos = (wpos + 1) & wmask;
        }

        /* Progress callback (rarely used) */
        if (TAMP_UNLIKELY(callback != NULL)) {
            /* Compute written count from pointer difference */
            res = callback(user_data, (size_t)(out - output), input_size);
            if (res != 0) goto cleanup;
        }
    }

    res = TAMP_INPUT_EXHAUSTED;

cleanup:
    /* Save state back to decompressor struct */
    decompressor->bit_buffer = bb;
    decompressor->bit_buffer_pos = bbp;
    decompressor->window_pos = wpos;
    decompressor->skip_bytes = skip;

    /* Compute written count from pointer difference - eliminates per-byte increment */
    if (output_written_size) *output_written_size = (size_t)(out - output);
    /* Compute consumed bytes from pointer difference - eliminates per-byte increment */
    if (input_consumed_size) *input_consumed_size = (size_t)(in - input);

    return res;
}

/* Non-callback wrapper for simpler API */
tamp_res tamp_decompressor_decompress_arm(
    TampDecompressorArm *decompressor,
    unsigned char *output,
    size_t output_size,
    size_t *output_written_size,
    const unsigned char *input,
    size_t input_size,
    size_t *input_consumed_size
) {
    return tamp_decompressor_decompress_cb_arm(
        decompressor, output, output_size, output_written_size,
        input, input_size, input_consumed_size, NULL, NULL
    );
}
