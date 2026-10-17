/*
 * Synaptics 06cb:00dc WBDI USB Fingerprint Driver for libfprint
 * Copyright (C) 2026 Pierpaolo Spadafora
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <stddef.h>

#define SYNA_MAX_ALIGNMENT_FRAMES 32

/* Align and average count contiguous SYNA_OUTPUT_ROWS x SYNA_OUTPUT_COLS frames.
 * Returns a malloc-compatible image, or NULL for invalid input/allocation failure.
 * out_quality may be NULL; otherwise it receives the anchor frame's mean energy.
 */
float *syna_align_frames (const float *frames,
                          size_t       count,
                          float       *out_quality);
