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

#include "decompressor_arm.h"
#include "common_arm.h"
#include <string.h>

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
    TampConf *__restrict__ conf,
    const unsigned char *__restrict__ input,
    size_t input_size,
    size_t *__restrict__ input_consumed_size
) {
    if (input_consumed_size) *input_consumed_size = 0;
    if (input_size == 0) return TAMP_INPUT_EXHAUSTED;

    /* Bit 0 must be 0 (more_header flag) */
    if (input[0] & 0x1) return TAMP_INVALID_CONF;

    conf->window = ((input[0] >> 5) & 0x7) + 8;
    conf->literal = ((input[0] >> 3) & 0x3) + 5;
    conf->use_custom_dictionary = ((input[0] >> 2) & 0x1);
    conf->v2 = ((input[0] >> 1) & 0x1);

    if (input_consumed_size) (*input_consumed_size)++;
    return TAMP_OK;
}

static tamp_res tamp_decompressor_populate_from_conf(
    TampDecompressorArm *decompressor,
    uint8_t conf_window,
    uint8_t conf_literal,
    uint8_t conf_use_custom_dictionary,
    uint8_t conf_v2
) {
    if (conf_window < 8 || conf_window > 15) return TAMP_INVALID_CONF;
    if (conf_literal < 5 || conf_literal > 8) return TAMP_INVALID_CONF;

    if (!conf_use_custom_dictionary)
        tamp_initialize_dictionary_arm(decompressor->window, (1 << conf_window));

    decompressor->conf_window = conf_window;
    decompressor->conf_literal = conf_literal;
    decompressor->min_pattern_size = tamp_compute_min_pattern_size_arm(conf_window, conf_literal);
    decompressor->v2 = conf_v2;
    decompressor->configured = true;

    return TAMP_OK;
}

tamp_res tamp_decompressor_init_arm(
    TampDecompressorArm *__restrict__ decompressor,
    const TampConf *__restrict__ conf,
    unsigned char *__restrict__ window
) {
    for(size_t i = 0; i < sizeof(TampDecompressorArm); i++){
        *(((uint8_t*)(decompressor))+i) = 0x0;
    }
    decompressor->window = window;

    if (conf) {
        return tamp_decompressor_populate_from_conf(
            decompressor, conf->window, conf->literal, conf->use_custom_dictionary, conf->v2
        );
    }
    return TAMP_OK;
}

/*============================================================================
 * Main Decompression Loop
 *============================================================================*/
// __attribute__((hot, flatten))
// tamp_res tamp_decompressor_decompress_cb_arm(
//     TampDecompressorArm *__restrict__ decompressor,
//     unsigned char *__restrict__ output,
//     size_t output_size,
//     size_t *__restrict__ output_written_size,
//     const unsigned char *__restrict__ input,
//     size_t input_size,
//     size_t *__restrict__ input_consumed_size,
//     tamp_callback_t callback,
//     void *user_data
// ) {
//     tamp_res res;

//     /* Input/output pointers */
//     const unsigned char *__restrict__ in = input;
//     const unsigned char *const in_end = input + input_size;
//     unsigned char *__restrict__ out = output;
//     const unsigned char *const out_end = output + output_size;

//     /* Cache decompressor state in registers */
//     uint32_t bb = decompressor->bit_buffer;      /* MSB-first bit buffer */
//     uint32_t bbp = decompressor->bit_buffer_pos; /* Valid bits in buffer */
//     uint32_t wpos = decompressor->window_pos;    /* Current window write position */
//     uint32_t skip = decompressor->skip_bytes;    /* Bytes to skip (partial token resume) */
//     unsigned char *__restrict__ win = decompressor->window;

//     /* Handle header if not yet configured */
//     if (TAMP_UNLIKELY(!decompressor->configured)) {
//         size_t header_consumed_size;
//         TampConf conf;
//         res = tamp_decompressor_read_header_arm(&conf, in, in_end - in, &header_consumed_size);
//         if (res != TAMP_OK) goto cleanup;

//         res = tamp_decompressor_populate_from_conf(
//             decompressor, conf.window, conf.literal, conf.use_custom_dictionary, conf.v2
//         );
//         if (res != TAMP_OK) goto cleanup;

//         in += header_consumed_size;
//         win = decompressor->window;
//     }

//     /* Precompute constants from configuration */
//     const uint32_t cwin = decompressor->conf_window;
//     const uint32_t clit = decompressor->conf_literal;
//     const uint32_t min_pat = decompressor->min_pattern_size;
//     const uint32_t wmask = (1u << cwin) - 1;   /* Window index mask */
//     const uint32_t wsize = (1u << cwin);       /* Window size */
//     const uint32_t lit_bits = 1 + clit;        /* Bits per literal (flag + value) */
//     const uint32_t cwin_shift = 32 - cwin;     /* Shift to extract window offset */
//     const uint32_t clit_shift = 32 - clit;     /* Shift to extract literal value */
//     const uint32_t is_v2 = decompressor->v2;   /* V2 format flag */

//     /*
//      * Main decode loop
//      *
//      * Bit stream format (MSB-first):
//      *   0 + 0 + <cwin bits>              = Token, match_size=0
//      *   0 + 1 + <huffman> + <cwin bits>  = Token, match_size from Huffman table
//      *   1 + <clit bits>                  = Literal
//      */
//     while (in < in_end || bbp) {
//         /* Refill bit buffer - keep at least 24 bits available */
//         while (in < in_end && bbp <= 24) {
//             bb |= ((uint32_t)*in++) << (24 - bbp);
//             bbp += 8;
//         }

//         /* Check for i/unput/output exhaustion */
//         if (TAMP_UNLIKELY(bbp == 0)) {
//             res = TAMP_INPUT_EXHAUSTED;
//             goto cleanup;
//         }
//         if (TAMP_UNLIKELY(out >= out_end)) {
//             res = TAMP_OUTPUT_FULL;
//             goto cleanup;
//         }

//         /* Decode token or literal based on MSB */
//         if (TAMP_LIKELY(!(bb >> 31))) {
//             /*
//              * TOKEN PATH
//              * Format: 0 + match_size_flag + [huffman_code] + window_offset
//              */
//             uint32_t tbb = bb << 1;   /* Consume token flag bit */
//             uint32_t tbbp = bbp - 1;

//             if (TAMP_UNLIKELY(tbbp < 8)) {
//                 res = TAMP_INPUT_EXHAUSTED;
//                 goto cleanup;
//             }

//             uint32_t match_size;
//             uint32_t huffman_bits;

//             /* Check match_size flag (now at MSB of tbb) */
//             if (TAMP_LIKELY(!(tbb >> 31))) {
//                 /* Fast path: match_size = 0 (most common) */
//                 match_size = 0;
//                 huffman_bits = 1;
//             } else {
//                 /* Decode match_size from Huffman table */
//                 uint32_t entry = HUFFMAN_TABLE[(tbb >> 24) & 0x7F];
//                 huffman_bits = entry >> 4;  /* Table stores actual bits (saves +1 op) */
//                 match_size = entry & 0xF;

//                 /* Handle FLUSH token (byte-align the bit buffer) */
//                 if (TAMP_UNLIKELY(match_size == FLUSH)) {
//                     tbb <<= huffman_bits;
//                     tbbp -= huffman_bits;
//                     uint32_t discard = tbbp & 7;
//                     bb = tbb << discard;
//                     bbp = tbbp & ~7u;
//                     continue;
//                 }
//             }

//             /* Consume Huffman bits */
//             tbb <<= huffman_bits;
//             tbbp -= huffman_bits;

//             if (TAMP_UNLIKELY(tbbp < cwin)) {
//                 res = TAMP_INPUT_EXHAUSTED;
//                 goto cleanup;
//             }

//             /* Extract window offset and compute final match size */
//             match_size += min_pat;
//             uint32_t woff;
//             {
//                 uint32_t encoded_ref = tbb >> cwin_shift;
//                 woff = is_v2 ? ((wpos - encoded_ref) & wmask) : encoded_ref;
//             }

//             /* Bounds check: check copy range doesn't exceed window */
//             if (TAMP_UNLIKELY(woff + match_size > wsize)) {
//                 res = TAMP_OOB;
//                 goto cleanup;
//             }

//             /* Handle partial token from previous call (output buffer was full) */
//             uint32_t ms_skip = match_size - skip;
//             const uint32_t woff_skip = woff + skip;

//             const size_t remaining = out_end - out;
//             if (TAMP_UNLIKELY(ms_skip > remaining)) {
//                 /* Can't complete this token - save state for next call */
//                 skip += remaining;
//                 ms_skip = remaining;
//             } else {
//                 /* Token complete - consume bits */
//                 skip = 0;
//                 bb = tbb << cwin;
//                 bbp = tbbp - cwin;
//             }

//             /* Copy from window to output, updating window as we go */
//             if (TAMP_LIKELY(skip == 0)) {
//                 if (TAMP_LIKELY(ms_skip == match_size)) {
//                     /* Common path: fresh token, no previous partial output */
//                     const uint32_t dist = (wpos - woff) & wmask;

//                     if (TAMP_LIKELY(dist >= match_size)) {
//                         /* No overlap: copy directly */
//                         const unsigned char *__restrict__ src = win + woff;
//                         unsigned char *__restrict__ dst_out = out;
//                         uint32_t count = match_size;

//                         while (count--) {
//                             uint32_t c = *src++;  /* Use uint32_t to avoid char-to-int conversion */
//                             *dst_out++ = c;
//                             win[wpos] = c;
//                             wpos = (wpos + 1) & wmask;
//                         }
//                     } else {
//                         /* Overlap: snapshot source first, then copy */
//                         uint32_t tmp[16];  /* Use uint32_t to avoid char arithmetic overhead */
//                         const unsigned char *src = win + woff;
//                         for (uint32_t i = 0; i < match_size; i++) {
//                             tmp[i] = src[i];
//                         }
//                         for (uint32_t i = 0; i < match_size; i++) {
//                             uint32_t c = tmp[i];
//                             out[i] = c;
//                             win[wpos] = c;
//                             wpos = (wpos + 1) & wmask;
//                         }
//                     }
//                 } else {
//                     /* Resume path: completing a token started in a previous call.
//                      * Output gets ms_skip bytes from woff_skip; window gets all match_size bytes. */
//                     uint32_t tmp[16];
//                     const unsigned char *src = win + woff;
//                     for (uint32_t i = 0; i < match_size; i++) tmp[i] = src[i];

//                     const uint32_t prefix = match_size - ms_skip;
//                     for (uint32_t i = 0; i < ms_skip; i++) {
//                         out[i] = tmp[prefix + i];
//                     }
//                     for (uint32_t i = 0; i < match_size; i++) {
//                         win[wpos] = tmp[i];
//                         wpos = (wpos + 1) & wmask;
//                     }
//                 }
//             } else {
//                 /* Partial copy for output-limited case */
//                 const unsigned char *__restrict__ src = win + woff_skip;
//                 for (uint32_t i = 0; i < ms_skip; i++) {
//                     out[i] = src[i];
//                 }
//             }

//             out += ms_skip;

//         } else {
//             /*
//              * LITERAL PATH
//              * Format: 1 + <clit bits of literal value>
//              */
//             if (TAMP_UNLIKELY(bbp < lit_bits)) {
//                 res = TAMP_INPUT_EXHAUSTED;
//                 goto cleanup;
//             }

//             /* Use uint32_t to avoid char-to-int conversion overhead */
//             const uint32_t literal = (bb << 1) >> clit_shift;
//             bb <<= lit_bits;
//             bbp -= lit_bits;

//             /* Write to output and window */
//             *out++ = literal;
//             win[wpos] = literal;
//             wpos = (wpos + 1) & wmask;
//         }

//         /* Progress callback (rarely used) */
//         if (TAMP_UNLIKELY(callback != NULL)) {
//             /* Compute written count from pointer difference */
//             res = callback(user_data, (size_t)(out - output), input_size);
//             if (res != 0) goto cleanup;
//         }
//     }

//     res = TAMP_INPUT_EXHAUSTED;

// cleanup:
//     /* Save state back to decompressor struct */
//     decompressor->bit_buffer = bb;
//     decompressor->bit_buffer_pos = bbp;
//     decompressor->window_pos = wpos;
//     decompressor->skip_bytes = skip;

//     /* Compute written count from pointer difference - eliminates per-byte increment */
//     if (output_written_size) *output_written_size = (size_t)(out - output);
//     /* Compute consumed bytes from pointer difference - eliminates per-byte increment */
//     if (input_consumed_size) *input_consumed_size = (size_t)(in - input);

//     return res;
// }

/*============================================================================
 * Optimized No-Callback Decompression Loop
 *
 * Optimizations over _cb_arm:
 *   A) Callback code fully removed: frees registers, removes branch per iteration
 *   B) UBFX-based combined dispatch: single branch for most common path
 *============================================================================*/

__attribute__((hot, flatten))
tamp_res tamp_decompressor_decompress_nocb_arm(
    TampDecompressorArm *decompressor,
    unsigned char *output,
    size_t output_size,
    size_t *output_written_size,
    const unsigned char *input,
    size_t input_size,
    size_t *input_consumed_size
) {
    tamp_res res;

    /* Input/output pointers */
    const unsigned char *__restrict__ in = input;
    const unsigned char *const in_end = input + input_size;
    unsigned char *__restrict__ out = output;
    const unsigned char *const out_end = output + output_size;

    /* Cache decompressor state in registers */
    uint32_t bb = decompressor->bit_buffer;
    uint32_t bbp = decompressor->bit_buffer_pos;
    uint32_t wpos = decompressor->window_pos;
    uint32_t skip = decompressor->skip_bytes;
    unsigned char *__restrict__ win = decompressor->window;

    /* Handle header if not yet configured */
    if (TAMP_UNLIKELY(!decompressor->configured)) {
        return TAMP_INVALID_CONF;
    }

    /* Precompute constants from configuration */
    const uint32_t cwin = decompressor->conf_window;
    const uint32_t clit = decompressor->conf_literal;
    const uint32_t min_pat = decompressor->min_pattern_size;
    const uint32_t wmask = (1u << cwin) - 1;
    const uint32_t wsize = (1u << cwin);
    const uint32_t lit_bits = 1 + clit;
    const uint32_t cwin_shift = 32 - cwin;
    const uint32_t clit_shift = 32 - clit;
    const uint32_t is_v2 = decompressor->v2;

    while (in < in_end || bbp) {
        /* Refill bit buffer */
        while (in < in_end && bbp <= 24) {
            bb |= ((uint32_t)*in++) << (24 - bbp);
            bbp += 8;
        }

        if (TAMP_UNLIKELY(bbp == 0)) {
            res = TAMP_INPUT_EXHAUSTED;
            goto cleanup;
        }
        if (TAMP_UNLIKELY(out >= out_end)) {
            res = TAMP_OUTPUT_FULL;
            goto cleanup;
        }

        /*
         * [Optimization B] UBFX combined dispatch
         *
         * Extract bits [31:30] of bb in one instruction:
         *   top2 == 0: token, match_size=0 (most common, ~50%)
         *   top2 == 1: token, Huffman needed (~30%)
         *   top2 >= 2: literal (~20%)
         *
         * Reduces 2 sequential branches to 1 for the fast path.
         */
        uint32_t top2;
#if defined(__aarch64__)
        __asm__ volatile("ubfx %w0, %w1, #30, #2" : "=r"(top2) : "r"(bb));
#else
        top2 = (bb >> 30) & 3;
#endif

        /* Decode: token paths set tbb, tbbp, woff, match_size then
         * fall through to the shared copy code. Literal uses continue. */
        uint32_t match_size;
        uint32_t tbb, tbbp, woff;

        if (TAMP_LIKELY(top2 == 0)) {
            /*
             * FAST TOKEN PATH: match_size = min_pat
             * Consume 2 bits at once (token flag '0' + match_size flag '0')
             */
            tbb = bb << 2;
            tbbp = bbp - 2;

            if (TAMP_UNLIKELY(tbbp < cwin)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            {
                uint32_t encoded_ref = tbb >> cwin_shift;
                woff = is_v2 ? ((wpos - encoded_ref) & wmask) : encoded_ref;
            }
            match_size = min_pat;

            if (TAMP_UNLIKELY(woff + match_size > wsize)) {
                res = TAMP_OOB;
                goto cleanup;
            }

            /* Fall through to shared copy */

        } else if (TAMP_UNLIKELY(top2 >= 2)) {
            /*
             * LITERAL PATH
             */
            if (TAMP_UNLIKELY(bbp < lit_bits)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            const uint32_t literal = (bb << 1) >> clit_shift;
            bb <<= lit_bits;
            bbp -= lit_bits;

            *out++ = literal;
            win[wpos] = literal;
            wpos = (wpos + 1) & wmask;
            continue;

        } else {
            /*
             * HUFFMAN TOKEN PATH (top2 == 1)
             * Consume 2 bits (token flag '0' + match_size flag '1')
             */
            tbb = bb << 2;
            tbbp = bbp - 2;

            if (TAMP_UNLIKELY(tbbp < 7)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            uint32_t entry = HUFFMAN_TABLE[(tbb >> 25) & 0x7F];
            uint32_t huffman_bits = entry >> 4;
            match_size = entry & 0xF;

            if (TAMP_UNLIKELY(match_size == FLUSH)) {
                uint32_t hb = huffman_bits - 1;
                tbb <<= hb;
                tbbp -= hb;
                uint32_t discard = tbbp & 7;
                bb = tbb << discard;
                bbp = tbbp & ~7u;
                continue;
            }

            match_size += min_pat;

            /* huffman_bits includes the match_size flag bit we already
             * consumed in bb<<2, so subtract 1 */
            uint32_t hb = huffman_bits - 1;
            tbb <<= hb;
            tbbp -= hb;

            if (TAMP_UNLIKELY(tbbp < cwin)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            {
                uint32_t encoded_ref = tbb >> cwin_shift;
                woff = is_v2 ? ((wpos - encoded_ref) & wmask) : encoded_ref;
            }

            if (TAMP_UNLIKELY(woff + match_size > wsize)) {
                res = TAMP_OOB;
                goto cleanup;
            }

            /* Fall through to shared copy */
        }

        /* Shared token copy - reached by fast path and Huffman path */
        {
            uint32_t ms_skip = match_size - skip;
            const uint32_t woff_skip = woff + skip;

            const size_t remaining = out_end - out;
            if (TAMP_UNLIKELY(ms_skip > remaining)) {
                skip += remaining;
                ms_skip = remaining;
            } else {
                skip = 0;
                bb = tbb << cwin;
                bbp = tbbp - cwin;
            }

            if (TAMP_LIKELY(skip == 0)) {
                if (TAMP_LIKELY(ms_skip == match_size)) {
                    /* Common path: fresh token, no previous partial output */
                    const uint32_t dist = (wpos - woff) & wmask;

                    if (TAMP_LIKELY(dist >= match_size)) {
                        const unsigned char *__restrict__ src = win + woff;
                        unsigned char *__restrict__ dst_out = out;
                        uint32_t count = match_size;

                        while (count--) {
                            uint32_t c = *src++;
                            *dst_out++ = c;
                            win[wpos] = c;
                            wpos = (wpos + 1) & wmask;
                        }
                    } else {
                        uint32_t tmp[16];
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
                    /* Resume path: completing a token started in a previous call.
                     * Output gets ms_skip bytes from woff_skip; window gets all match_size bytes. */
                    uint32_t tmp[16];
                    const unsigned char *src = win + woff;
                    for (uint32_t i = 0; i < match_size; i++) tmp[i] = src[i];

                    const uint32_t prefix = match_size - ms_skip;
                    for (uint32_t i = 0; i < ms_skip; i++) {
                        out[i] = tmp[prefix + i];
                    }
                    for (uint32_t i = 0; i < match_size; i++) {
                        win[wpos] = tmp[i];
                        wpos = (wpos + 1) & wmask;
                    }
                }
            } else {
                const unsigned char *__restrict__ src = win + woff_skip;
                for (uint32_t i = 0; i < ms_skip; i++) {
                    out[i] = src[i];
                }
            }

            out += ms_skip;
        }
    }

    res = TAMP_INPUT_EXHAUSTED;

cleanup:
    decompressor->bit_buffer = bb;
    decompressor->bit_buffer_pos = bbp;
    decompressor->window_pos = wpos;
    decompressor->skip_bytes = skip;

    if (output_written_size) *output_written_size = (size_t)(out - output);
    if (input_consumed_size) *input_consumed_size = (size_t)(in - input);

    return res;
}

/*============================================================================
 * V2 Fast-Path Decompressor (Output-as-Window)
 *
 * For V2 streams with large output buffers, uses the output buffer itself
 * as the sliding window, eliminating the per-byte dual-write bottleneck.
 *
 * Two-phase approach:
 *   1. Bootstrap: first wsize bytes via _nocb_arm (uses ring buffer window)
 *   2. Fast path: remaining bytes with no window writes, no wpos tracking
 *
 * At cleanup, syncs the decompressor's window from the output tail so
 * multi-call decompression works correctly.
 *============================================================================*/

__attribute__((hot, flatten))
static tamp_res _v2_fast_arm_internal(
    TampDecompressorArm *decompressor,
    unsigned char *__restrict__ output,
    size_t output_size,
    size_t *__restrict__ output_written_size,
    const unsigned char *__restrict__ input,
    size_t input_size,
    size_t *__restrict__ input_consumed_size
) {
    const uint32_t cwin = decompressor->conf_window;
    const uint32_t wsize = 1u << cwin;

    /* Phase 1: Bootstrap - delegate first wsize bytes to _nocb_arm.
     * Give an extra 16 bytes (max match size) so tokens that straddle
     * the wsize boundary can complete without leaving skip_bytes > 0. */
    size_t bootstrap_written = 0;
    size_t bootstrap_consumed = 0;
    size_t bs_limit = wsize + 16;
    if (bs_limit > output_size) bs_limit = output_size;
    tamp_res res = tamp_decompressor_decompress_nocb_arm(
        decompressor, output, bs_limit, &bootstrap_written,
        input, input_size, &bootstrap_consumed
    );

    /* If bootstrap didn't fill the window, return as-is */
    if (bootstrap_written < wsize) {
        if (output_written_size) *output_written_size = bootstrap_written;
        if (input_consumed_size) *input_consumed_size = bootstrap_consumed;
        return res;
    }

    /* Finish any partial token at bootstrap boundary */
    while (decompressor->skip_bytes > 0 && bootstrap_written < output_size) {
        size_t extra_written = 0, extra_consumed = 0;
        size_t extra_limit = output_size - bootstrap_written;
        if (extra_limit > 16) extra_limit = 16;
        res = tamp_decompressor_decompress_nocb_arm(
            decompressor, output + bootstrap_written, extra_limit,
            &extra_written, input + bootstrap_consumed,
            input_size - bootstrap_consumed, &extra_consumed
        );
        bootstrap_written += extra_written;
        bootstrap_consumed += extra_consumed;
        if (extra_written == 0) break;
    }

    /* If still incomplete (input exhausted or output full), return */
    if (bootstrap_written < wsize || decompressor->skip_bytes > 0) {
        if (output_written_size) *output_written_size = bootstrap_written;
        if (input_consumed_size) *input_consumed_size = bootstrap_consumed;
        return res;
    }

    /* Phase 2: Fast path - no window writes, no wpos tracking */
    const unsigned char *__restrict__ in = input + bootstrap_consumed;
    const unsigned char *const in_end = input + input_size;
    unsigned char *out = output + bootstrap_written;  /* No __restrict__: out IS the window, self-aliasing expected */
    const unsigned char *const out_end = output + output_size;

    /* Reload bit buffer state (bootstrap saved it to struct) */
    uint32_t bb = decompressor->bit_buffer;
    uint32_t bbp = decompressor->bit_buffer_pos;

    /* Precompute constants */
    const uint32_t clit = decompressor->conf_literal;
    const uint32_t min_pat = decompressor->min_pattern_size;
    const uint32_t lit_bits = 1 + clit;
    const uint32_t cwin_shift = 32 - cwin;
    const uint32_t clit_shift = 32 - clit;

    while (in < in_end || bbp) {
        /* Refill bit buffer */
        while (in < in_end && bbp <= 24) {
            bb |= ((uint32_t)*in++) << (24 - bbp);
            bbp += 8;
        }

        if (TAMP_UNLIKELY(bbp == 0)) {
            res = TAMP_INPUT_EXHAUSTED;
            goto cleanup;
        }
        if (TAMP_UNLIKELY(out >= out_end)) {
            res = TAMP_OUTPUT_FULL;
            goto cleanup;
        }

        /* UBFX combined dispatch (same as _nocb_arm) */
        uint32_t top2;
#if defined(__aarch64__)
        __asm__ volatile("ubfx %w0, %w1, #30, #2" : "=r"(top2) : "r"(bb));
#else
        top2 = (bb >> 30) & 3;
#endif

        if (TAMP_LIKELY(top2 == 0)) {
            /*
             * FAST TOKEN PATH: match_size = min_pat
             */
            uint32_t tbb = bb << 2;
            uint32_t tbbp = bbp - 2;

            if (TAMP_UNLIKELY(tbbp < cwin)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            uint32_t encoded_ref = tbb >> cwin_shift;
            uint32_t back_dist = encoded_ref ? encoded_ref : wsize;
            uint32_t match_size = min_pat;

            /* Check output space for partial token */
            const size_t remaining = out_end - out;
            if (TAMP_UNLIKELY(match_size > remaining)) {
                /* Partial token: copy what fits, don't consume cwin bits. */
                uint32_t partial = remaining;
                if (TAMP_LIKELY(back_dist >= match_size)) {
                    const unsigned char *src = out - back_dist;
                    for (uint32_t i = 0; i < partial; i++)
                        out[i] = src[i];
                } else {
                    const unsigned char *src_recent = out - back_dist;
                    const unsigned char *src_old = out - wsize;
                    for (uint32_t i = 0; i < partial; i++)
                        out[i] = (i < back_dist) ? src_recent[i] : src_old[i - back_dist];
                }
                out += partial;
                decompressor->skip_bytes = partial;
                res = TAMP_OUTPUT_FULL;
                goto cleanup;
            }

            /* Consume all bits and copy from output buffer */
            bb = tbb << cwin;
            bbp = tbbp - cwin;

            if (TAMP_LIKELY(back_dist >= match_size)) {
                const unsigned char *src = out - back_dist;
                /* match_size == min_pat >= 2, and match_size <= remaining
                 * (checked above), so a 2-byte copy is always safe. */
                uint16_t v;
                __builtin_memcpy(&v, src, 2);
                __builtin_memcpy(out, &v, 2);
                for (uint32_t i = 2; i < match_size; i++)
                    out[i] = src[i];
                out += match_size;
            } else {
                /* Overlap: TAMP uses snapshot copy, not LZ77 RLE.
                 * Recent bytes at out[-back_dist], old bytes at out[-wsize]. */
                const unsigned char *src_recent = out - back_dist;
                const unsigned char *src_old = out - wsize;
                for (uint32_t i = 0; i < back_dist; i++)
                    out[i] = src_recent[i];
                for (uint32_t i = 0; i < match_size - back_dist; i++)
                    out[back_dist + i] = src_old[i];
                out += match_size;
            }

        } else if (TAMP_UNLIKELY(top2 >= 2)) {
            /*
             * LITERAL PATH: single write, no window
             */
            if (TAMP_UNLIKELY(bbp < lit_bits)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            const uint32_t literal = (bb << 1) >> clit_shift;
            bb <<= lit_bits;
            bbp -= lit_bits;

            *out++ = literal;

        } else {
            /*
             * HUFFMAN TOKEN PATH (top2 == 1)
             */
            uint32_t tbb = bb << 2;
            uint32_t tbbp = bbp - 2;

            if (TAMP_UNLIKELY(tbbp < 7)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            uint32_t entry = HUFFMAN_TABLE[(tbb >> 25) & 0x7F];
            uint32_t huffman_bits = entry >> 4;
            uint32_t match_size = entry & 0xF;

            if (TAMP_UNLIKELY(match_size == FLUSH)) {
                uint32_t hb = huffman_bits - 1;
                tbb <<= hb;
                tbbp -= hb;
                uint32_t discard = tbbp & 7;
                bb = tbb << discard;
                bbp = tbbp & ~7u;
                continue;
            }

            match_size += min_pat;

            uint32_t hb = huffman_bits - 1;
            tbb <<= hb;
            tbbp -= hb;

            if (TAMP_UNLIKELY(tbbp < cwin)) {
                res = TAMP_INPUT_EXHAUSTED;
                goto cleanup;
            }

            uint32_t encoded_ref = tbb >> cwin_shift;
            uint32_t back_dist = encoded_ref ? encoded_ref : wsize;

            /* Check output space for partial token */
            const size_t remaining = out_end - out;
            if (TAMP_UNLIKELY(match_size > remaining)) {
                uint32_t partial = remaining;
                if (TAMP_LIKELY(back_dist >= match_size)) {
                    const unsigned char *src = out - back_dist;
                    for (uint32_t i = 0; i < partial; i++)
                        out[i] = src[i];
                } else {
                    const unsigned char *src_recent = out - back_dist;
                    const unsigned char *src_old = out - wsize;
                    for (uint32_t i = 0; i < partial; i++)
                        out[i] = (i < back_dist) ? src_recent[i] : src_old[i - back_dist];
                }
                out += partial;
                decompressor->skip_bytes = partial;
                res = TAMP_OUTPUT_FULL;
                goto cleanup;
            }

            /* Consume all bits and copy from output buffer */
            bb = tbb << cwin;
            bbp = tbbp - cwin;

            if (TAMP_LIKELY(back_dist >= match_size)) {
                const unsigned char *src = out - back_dist;
                for (uint32_t i = 0; i < match_size; i++)
                    out[i] = src[i];
                out += match_size;
            } else {
                /* Overlap: TAMP uses snapshot copy, not LZ77 RLE.
                 * Recent bytes at out[-back_dist], old bytes at out[-wsize]. */
                const unsigned char *src_recent = out - back_dist;
                const unsigned char *src_old = out - wsize;
                for (uint32_t i = 0; i < back_dist; i++)
                    out[i] = src_recent[i];
                for (uint32_t i = 0; i < match_size - back_dist; i++)
                    out[back_dist + i] = src_old[i];
                out += match_size;
            }
        }
    }

    res = TAMP_INPUT_EXHAUSTED;

cleanup:
    /* Sync window from output tail for multi-call resume.
     * window[0] = oldest byte, wpos = 0: subsequent _nocb_arm calls
     * can read from the synced window correctly. */
    {
        size_t total_written = out - output;
        if (total_written >= wsize) {
            memcpy(decompressor->window, out - wsize, wsize);
            decompressor->window_pos = 0;
        }
    }

    decompressor->bit_buffer = bb;
    decompressor->bit_buffer_pos = bbp;

    if (output_written_size) *output_written_size = (size_t)(out - output);
    if (input_consumed_size) *input_consumed_size = (size_t)(in - input);

    return res;
}

/*============================================================================
 * Public API Wrapper
 *
 * Routes to V2 fast path when conditions are met, otherwise falls back
 * to the standard _nocb_arm path (handles V1, V2-slow, and resume).
 *============================================================================*/

tamp_res tamp_decompressor_decompress_arm(
    TampDecompressorArm *__restrict__ decompressor,
    unsigned char *__restrict__ output,
    size_t output_size,
    size_t *__restrict__ output_written_size,
    const unsigned char *__restrict__ input,
    size_t input_size,
    size_t *__restrict__ input_consumed_size
) {
    size_t header_consumed = 0;

    /* Read header in wrapper so we know v2/wsize before routing */
    if (TAMP_UNLIKELY(!decompressor->configured)) {
        TampConf conf;
        tamp_res res = tamp_decompressor_read_header_arm(
            &conf, input, input_size, &header_consumed
        );
        if (res != TAMP_OK) {
            if (output_written_size) *output_written_size = 0;
            if (input_consumed_size) *input_consumed_size = 0;
            return res;
        }
        res = tamp_decompressor_populate_from_conf(
            decompressor, conf.window, conf.literal,
            conf.use_custom_dictionary, conf.v2
        );
        if (res != TAMP_OK) {
            if (output_written_size) *output_written_size = 0;
            if (input_consumed_size) *input_consumed_size = 0;
            return res;
        }
        input += header_consumed;
        input_size -= header_consumed;
    }

    tamp_res res;
    size_t sub_consumed = 0;

    if (decompressor->v2
        && output_size >= (1u << decompressor->conf_window)
        && decompressor->skip_bytes == 0) {
        res = _v2_fast_arm_internal(
            decompressor, output, output_size, output_written_size,
            input, input_size, &sub_consumed
        );
    } else {
        /* Return an error if we use the v1 format */
        res = tamp_decompressor_decompress_nocb_arm(
            decompressor, output, output_size, output_written_size,
            input, input_size, &sub_consumed
        );
    }

    if (input_consumed_size) *input_consumed_size = header_consumed + sub_consumed;
    return res;
}
