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

#include <fprint.h>
#include <fpi-device.h>
#include <fpi-print.h>
#include <gusb.h>
#include "syna_tls.h"
#include "syna_oem.h"
#include "syna_proto.h"

#define FPI_TYPE_DEVICE_SYNA_00DC (fpi_device_syna_00dc_get_type ())
G_DECLARE_FINAL_TYPE (FpiDeviceSyna00dc, fpi_device_syna_00dc, FPI, DEVICE_SYNA_00DC, FpDevice)

GType fpi_tod_shared_driver_get_type (void);

typedef struct _FpiDeviceSyna00dc FpiDeviceSynaTod;

struct _FpiDeviceSyna00dc
{
  FpDevice       parent;

  GUsbDevice    *usb_device;
  GMainContext  *main_context;
  SynaOemData    oem;
  SynaTlsContext tls;

  gboolean       is_open;
  gboolean       is_cancelling;
  gboolean       capture_pipeline_ready;
  gboolean       had_capture_in_session;

  /* Calibrated empty sensor baseline (56x144 floats) */
  float   *baseline;
  gboolean has_baseline;
  gboolean initial_contact_present;

  /* Per-device cached empty sensor baseline */
  float    cached_baseline[SYNA_IMG_WIDTH * SYNA_IMG_HEIGHT];
  gboolean has_cached_baseline;

  /* Enrolled templates stored locally */
  GPtrArray *enrolled_templates;
};
