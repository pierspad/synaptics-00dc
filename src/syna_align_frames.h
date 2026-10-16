#pragma once

#include <stddef.h>

#define SYNA_MAX_ALIGNMENT_FRAMES 32

/* Align and average count contiguous SYNA_OUTPUT_ROWS x SYNA_OUTPUT_COLS frames.
 * Returns a malloc-compatible image, or NULL for invalid input/allocation failure.
 * out_quality may be NULL; otherwise it receives the anchor frame's mean energy.
 */
float *syna_align_frames (const float *frames, size_t count, float *out_quality);
