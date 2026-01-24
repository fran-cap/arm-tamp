# ARM-Cortex Component

The directory contains code for the arm cortex optimized tamp component

## Optimization notes:
# most recent speedup ratio vs standard
1.49x (with -Os compiler flag)
1.45x (with -03 -flto compiler flag)

# what hasnt worked:
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
- **Unroll XorShift**: Dictionary init is negligible portion of total time
- **Character table in registers**: Same as above - init cost too small to matter
- **Register pressure reduction**: un-caching min_pat/wsize - compiler -O3 already optimizes register allocation well
- **9-bit Huffman table (I-cache revisit)**: tested with -Os, 1.41x-1.47x vs baseline 1.47x-1.55x. Larger table (512 bytes) hurts D-cache more than simpler code helps I-cache
- **Simplify overlap to single loop (I-cache revisit)**: tested with -Os, 1.47x-1.51x vs baseline. No measurable improvement, but kept change for cleaner code (2 loops instead of 3)
- **Move FLUSH to cold path (I-cache revisit)**: tested with -Os using `__attribute__((cold))`, 1.39x-1.54x vs baseline. No improvement, function call overhead negates any I-cache benefit
- **Remove match_size=0 fast path (I-cache revisit)**: same as 9-bit Huffman table test - the well-predicted branch is still faster than table lookup even with -Os

# tested but inconclusive (measurement noise):
(none remaining)

# worked:
- **simplifying the OOB check**: woff is extracted with only cwin bits, it's already bounded by wsize
- **Accumulate bits in register**: bb/bbp cached in local vars (registers)
- **AND vs modulo**: Using wmask = (1 << cwin) - 1 for ring buffer wrapping
- **Pre-compute mask**: wmask computed once before main loop
- **Profile-guided match_size=0**: Fast path with single bit check for most common token
- **Separate hot/cold paths**: Token path is the main loop body, literal is else branch
- **128-entry Huffman table**: Already has O(1) lookup for codes up to 7 bits
- **-Os compiler flag**: Smaller code = better I-cache utilization. Tested on enwik8: -O3 gave 1.26x, -Os gave 1.49x. Note: -Os + -flto is worse (1.20x), don't combine them, only use -o3 + -flto

# to try:
(none remaining - all I-cache-aware revisits tested, see "hasn't worked" section)