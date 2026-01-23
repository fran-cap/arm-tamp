# ARM-Cortex Component

The directory contains code for the arm cortex optimized tamp component

## Optimization notes:
# what hasnt worked to avoid:
adjusting compiler flags beyond o3
using memcopy/memmove anywhere -> slowwwwwwww
Prefetch the window data we're about to copy -> had 0 impact
switch based unroll for cases 2,3,4 -> slower
no-overlap copy loop to avoid the per-iteration mask -> added too many branches, slower
4-wide loop unrolling in copy loop (both with and without no-wrap fast path) -> inconsistent results, no clear improvement

# worked:
simplifying the OOB check, woff is extracted with only cwin bits, it's already bounded by wsize  

# to try:
(none currently)