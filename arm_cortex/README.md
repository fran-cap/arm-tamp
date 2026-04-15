# ARM-Cortex Component

The directory contains code for the arm cortex optimized tamp component

## Benchmark Environment

| Component | Details |
|-----------|---------|
| Hardware | Raspberry Pi 5 |
| CPU | ARM Cortex-A76, 4 cores @ 2.4 GHz |
| Architecture | aarch64 (ARMv8-A) |
| Cache | L1d: 256 KiB, L1i: 256 KiB, L2: 2 MiB, L3: 2 MiB |
| RAM | 8 GB |
| OS | Debian 12 (bookworm) / Raspberry Pi OS |
| Compiler | GCC 15.2.0 |

## Optimization notes:

### Current V2 speedup for the decompressor
- **1.540x** with `-O3` (197 MB/s ARM vs 128 MB/s standard)
- **1.697x** with `-Os` (178 MB/s ARM vs 105 MB/s standard)

# what hasnt worked:
- **Table-driven unified decode (512-entry)**: 1.24x (158 MB/s) - single table lookup for token/literal/flush + huffman decode. 1KB table causes cache pressure and extraction logic (3 shifts/masks) more expensive than well-predicted branches
- **Clang/LLVM compiler**: 1.30x (166 MB/s) vs GCC 1.41x (180 MB/s). GCC fully unrolls copy loop to ~16 cases while Clang uses real loop. GCC wins by 8%
- **Speculative decoding**: 1.37x (175 MB/s) - decode next token speculatively while current copy executes. Duplicated copy code hurts I-cache
- **Batch token decoding**: broke end-of-stream handling - attempted to decode multiple tokens before applying copies, but partial token at stream end caused incorrect output
- **Cleanup label restructuring**: 1.33x (170 MB/s) - separate goto labels for each exit condition (input_exhausted, output_full, oob) to enable conditional jumps. Extra labels hurt branch prediction
- **Copy loop unrolling by 4**: 1.34x (170.89 MB/s) - manual 4-iteration unroll with cleanup. GCC -O3 already optimizes better
- **using memcopy/memmove anywhere**: slowwwwwwww, 1.01x, overhead in stdlib
- **Prefetch the window data we're about to copy**: had 0 impact
- **switch based unroll for cases 2,3,4**: slower
- **no-overlap copy loop to avoid the per-iteration mask**: added too many branches, slower
- **branchless skip handling with multiplication**: slowed from 1.29x to 1.08x, multiplications too expensive
- **branchless skip handling with bit masks (-flag & value)**: slowed to 1.18x, mask ops still more expensive than well-predicted branches
- **branchless unified copy loop (overlap/no-overlap)**: complex and slower due to extra arithmetic per iteration; LIKELY/UNLIKELY hints work better
- **copy throughput with memcpy/memmove**: requires checking for linear overlap in window which adds too much branch overhead; memmove alone slowed to 1.21x
- **CLZ instruction for Huffman**: not applicable, Huffman codes are not simple unary structure, table lookup already optimal
- **9-bit Huffman lookup table**: would replace fast path for match_size=0 (well-predicted branch) with table lookup, slower
- **Bit buffer refill with bswap**: buffer rarely becomes completely empty (bbp==0), so bulk 4-byte load with bswap doesn't trigger often enough to help
- **32-bit copies for large matches**: match sizes in typical data are small (2-15 bytes), memcpy overhead outweighs benefit
- **memset shortcut for dist==1 overlap**: INCORRECT - dist==1 doesn't mean RLE. The source bytes win[woff..woff+n] can still be different values; dist only indicates circular distance between read/write positions
- **4-wide loop unrolling in copy loop**: tested on enwik8 (100MB) - baseline 1.29x vs unrolled 1.25x, manual unrolling actually SLOWER than compiler -O3 auto-optimization
- **Prefetch input bytes (PLD)**: tested on enwik8 - baseline 1.29x vs prefetch 1.24x, prefetch actually HURTS performance (cache pollution or branch overhead)
- **Switch-based refill (jump table)**: 1.33x (170 MB/s) - replacing while loop with switch on bytes-to-add. Computing space/avail/min and jump table dispatch slower than simple well-predicted while loop
- **Nested-if unrolled refill**: 1.40x (180 MB/s) - 4 nested if statements instead of while loop. Still slower than original - GCC optimizes the while loop better
- **Unroll XorShift**: Dictionary init is negligible portion of total time
- **Character table in registers**: Same as above - init cost too small to matter
- **Register pressure reduction**: un-caching min_pat/wsize - compiler -O3 already optimizes register allocation well
- **9-bit Huffman table (I-cache revisit)**: tested with -Os, 1.41x-1.47x vs baseline 1.47x-1.55x. Larger table (512 bytes) hurts D-cache more than simpler code helps I-cache
- **Simplify overlap to single loop (I-cache revisit)**: tested with -Os, 1.47x-1.51x vs baseline. No measurable improvement, but kept change for cleaner code (2 loops instead of 3)
- **Move FLUSH to cold path (I-cache revisit)**: tested with -Os using `__attribute__((cold))`, 1.39x-1.54x vs baseline. No improvement, function call overhead negates any I-cache benefit
- **Remove match_size=0 fast path (I-cache revisit)**: same as 9-bit Huffman table test - the well-predicted branch is still faster than table lookup even with -Os
- **Unpack bitfields to separate uint8_t/uint16_t**: 1.39x (176 MB/s) - separating hot fields (bbp, wpos, skip) to avoid RMW at cleanup actually slower. GCC generates efficient 64-bit load + ubfx extraction for packed bitfields; separate loads have more overhead
- **-O3 -flto**: 1.33x - LTO hurts ARM decompressor (was 180 MB/s, dropped to 172 MB/s). Standard got tiny boost.
- **-march=native -mtune=native**: 1.38x - native tuning slightly worse than plain -O3 (1.41x)
- **Prefetch (PLD)**: 1.40x - still hurts on real hardware (177 MB/s vs 180 MB/s baseline)
- **memcpy for non-overlap copies**: 1.18x - significantly slower (149 MB/s), even real ARM libc memcpy has too much overhead for small copies + extra window update loop
- **NEON intrinsics for copy loops**: 1.35x - slower (172 MB/s). Match sizes typically 2-15 bytes, so 8-byte NEON path rarely triggers; branch overhead for size check hurts
- **No-wrap path optimization**: 1.36x - slower (175 MB/s). Branch to check `wpos + match_size <= wsize` costs more than per-byte AND mask. ARM executes `& wmask` very efficiently
- **64-bit bit buffer**: 1.41x (178 MB/s) - no improvement over 32-bit. ARM64 has native 64-bit ops but larger buffer doesn't reduce refill frequency enough to matter
- **Address-independent refill (unrolled)**: 1.38x - slower (174 MB/s). Nested if statements add overhead vs simple while loop
- **Negative indexing copy loop**: 1.33x - slower (168 MB/s). End-pointer setup and negative index arithmetic costs more than simple increment
- **Lookahead amortization**: 1.40x - no improvement. Single upfront bit check vs two separate checks is noise-level difference
- **Loop count-down with do-while (PMC6263706)**: 1.37x (177 MB/s) - converting copy loops to count-down form with separate index tracking adds overhead; GCC -O3 already optimizes while(count--) well
- **Unrolled refill with nested ifs (Giesen)**: 1.36x (176 MB/s) - replacing while loop with cascaded if-statements worse than simple while loop on ARM
- **Combined shifts with inlined fast path (Dougall)**: 1.37x (176 MB/s) - duplicating copy code for fast path (match_size=0) hurts I-cache, same issue as speculative decoding
- **32-bit word loads for refill (Dougall)**: Not directly applicable - MSB-first 32-bit buffer would overflow; would need 64-bit buffer (already tried, no gain)
- **LSB-first bit buffer (Giesen)**: Would require complete rewrite; MSB-first matches TAMP's compression format
- **Rotate-based extraction (Giesen)**: Only helps when extracting from bottom after rotation; our MSB-first format extracts from top
- **GCC `__attribute__((assume(...)))` hints (GCC 13+)**: 1.47x (187-190 MB/s) - tested multiple assumptions: config bounds (cwin 8-15, clit 5-8), shift bounds (cwin_shift 17-24), match_size bounds (0-13, then 2-16 after min_pat), woff < wsize, huffman_bits 1-7, copy loop count bounds. All either matched baseline (~190 MB/s) or were slightly slower (186-187 MB/s). GCC 15's optimizer already infers these bounds from code structure; explicit assumptions just affect code layout negatively. `fallthrough` attribute not applicable (no switch statements with intentional fallthrough).
- **C23 stdbit.h `stdc_leading_zeros` (CLZ)**: 1.44x (182 MB/s) - attempted to combine token/literal detection (MSB check) and match_size flag check into single CLZ instruction. Pattern: lz==0 → literal, lz==1 → token+Huffman, lz≥2 → token+match_size=0. Slower than two well-predicted branches because CLZ overhead + code restructuring costs more than branch mispredictions (which are rare due to LIKELY hints). Other stdbit.h functions (rotate, popcount, bit_width, bit_floor) have no applicable use cases in the hot path.

# worked:
- **simplifying the OOB check**: woff is extracted with only cwin bits, it's already bounded by wsize
- **Accumulate bits in register**: bb/bbp cached in local vars (registers)
- **AND vs modulo**: Using wmask = (1 << cwin) - 1 for ring buffer wrapping
- **Pre-compute mask**: wmask computed once before main loop
- **Profile-guided match_size=0**: Fast path with single bit check for most common token
- **Separate hot/cold paths**: Token path is the main loop body, literal is else branch
- **128-entry Huffman table**: Already has O(1) lookup for codes up to 7 bits
- **-Os compiler flag**: ~~Smaller code = better I-cache utilization~~ REVERSED: -O3 (1.41x) beats -Os (1.32x)
- **Pointer difference for counting (PMC6263706)**: 1.42x (183 MB/s) - compute `in - input` at cleanup instead of incrementing counter per-byte in refill loop. Eliminates one add per input byte.
- **Integer for character temps (PMC6263706)**: uint32_t for literal and copy temps instead of uint8_t avoids implicit char-to-int conversions (marginal improvement, kept for cleanliness)
- **Baked-in huffman_bits in table**: Table stores actual bit count instead of (bits-1), eliminating +1 operation (GCC may already fold this, kept for clarity)
- **256-entry Huffman table (eliminates & 0x7F mask)**: Same performance - 2x cache footprint offsets mask elimination, reverted to 128-entry
- **Shift+mask vs double-shift for literal**: `(bb >> rshift) & mask` is slightly slower than `(bb << 1) >> clit_shift` - kept double-shift
- **LSB-first bit buffer with ARM RBIT**: 1.32x (169 MB/s) - significantly slower. While refill becomes simpler (`bb |= RBIT8(byte) << bbp` vs `bb |= byte << (24-bbp)`), all multi-bit values (woff, literal, Huffman index) need bit-reversal when extracted. The reversal cost (~3 extra RBIT per token/literal) outweighs the refill savings. LSB-first only helps when data is natively LSB (x86 LE), not when converting from MSB-first format.
- **`__restrict__` on function parameters**: 1.45x (186 MB/s) - adding `__restrict__` to all pointer parameters tells GCC the buffers don't overlap, enabling better load/store scheduling. Improved from 1.42x (183 MB/s).
- **Newer ARM64 GCC**: Moving to GCC 15 from GCC 12 gained 4MB/s for no code changes. ASM analysis shows GCC 15 optimizations:
  - NEON auto-vectorization for overlap tmp[] copy (1 vector load vs 16 scalar ldrb) - **19 fewer loads**
  - Better loop alignment (32-byte vs 8-byte) for I-cache
  - Smarter bit testing (`tbnz x10, 30` on original bb vs `tbnz w16, #31` on shifted tbb)
  - Smaller stack frame (256 vs 288 bytes)
  - Total: 17 fewer memory ops (228 vs 245), 4 fewer instructions (599 vs 603)
- **Indexed copy form (`out[i]=src[i]; out+=match_size`)**: +3 MB/s at `-Os` (174.6 → 177.6 MB/s). Applied to both no-overlap and overlap branches of fast-token and Huffman-token paths. Neutral at `-O3`. Same semantics as `*out++=*src++` but GCC's `-Os` pipeline produces a slightly cleaner indexed loop from this form.
- **Unconditional 2-byte prefix copy in fast-token no-overlap path**: +3 MB/s at `-O3` (194.1 → 197.1 MB/s). `match_size == min_pat >= 2` is invariant and `match_size <= remaining` is already checked, so two `__builtin_memcpy(.,.,2)` calls are always safe. GCC `-O3` inlines them as `ldrh/strh`, collapsing the old 10-instruction byte loop (5 insts × 2 iters) to ~5 instructions total. Rare `min_pat > 2` tail falls back to the original indexed loop. Neutral at `-Os` (memcpy still inlines but prefix was already cheap there).
