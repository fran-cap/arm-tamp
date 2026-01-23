#ifndef TAMP_COMMON_ARM_H
#define TAMP_COMMON_ARM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdlib.h>

// If standard common.h hasn't been included, provide minimal type definitions
#ifndef TAMP_OK
#include "tamp/common.h"
#endif

/**
 * @brief Pre-populate a window buffer with common characters (ARM version).
 *
 * @param[out] buffer Populated output buffer.
 * @param[in] size Size of output buffer in bytes.
 */
void tamp_initialize_dictionary_arm(unsigned char *buffer, size_t size);

/**
 * @brief Compute the minimum viable pattern size given window and literal config parameters (ARM version).
 *
 * @param[in] window Number of window bits. Valid values are [8, 15].
 * @param[in] literal Number of literal bits. Valid values are [5, 8].
 *
 * @return The minimum pattern size in bytes. Either 2 or 3.
 */
int8_t tamp_compute_min_pattern_size_arm(uint8_t window, uint8_t literal);

#ifdef __cplusplus
}
#endif

#endif  // TAMP_COMMON_ARM_H
