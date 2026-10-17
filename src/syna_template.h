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
#include <glib.h>
#include "syna_proto.h"
#define SYNA_TEMPLATE_PIXELS (SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES)
#define SYNA_ENROLL_STAGES 8
#define SYNA_MAX_TEMPLATE_SAMPLES 16
/* Version 1: (uint32 version, array of arrays of doubles). Legacy: native float bytes. */
GVariant *syna_template_encode (GPtrArray *samples);
GPtrArray *syna_template_decode (GVariant *data);
