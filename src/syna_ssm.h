#pragma once

#include "syna_device.h"
#include <fpi-ssm.h>
#include <fpi-usb-transfer.h>

/**
 * Create a non-blocking libfprint sequential state machine (FpiSsm) for device open,
 * hardware identity verification (0x01/0x75), optional first-boot pairing (0x4f/0x50),
 * TLS 1.2 session establishment, and capture pipeline initialization (0x1a/0x06).
 */
FpiSsm *syna_ssm_open_new (FpiDeviceSyna00dc *self);

/**
 * Callback invoked when a single 6-frame aligned & bandpass-filtered sample capture
 * completes (or fails) in the non-blocking FpiSsm capture state machine.
 */
typedef void (*SynaCaptureCompletedCallback) (FpiDeviceSyna00dc *self,
                                              float             *filtered_sample,
                                              gpointer           user_data,
                                              GError            *error);

/**
 * Create a non-blocking libfprint sequential state machine (FpiSsm) for a complete
 * biometric capture cycle:
 *   - Optional 0x1a cleanup + 0x06 capture initializer (if pipeline not ready)
 *   - 0x02 Capture Command TX/RX
 *   - Asynchronous 17,024-byte bulk stream reads on EP 0x82 with contact/lift state tracking
 *   - 0x04 Capture Stop TX/RX
 *   - 6-frame +/-2px alignment, averaging, and spatial bandpass filtering
 */
FpiSsm *syna_ssm_capture_new (FpiDeviceSyna00dc           *self,
                              gboolean                     require_finger_lift,
                              gboolean                     more_stages_needed,
                              SynaCaptureCompletedCallback cb,
                              gpointer                     user_data);

typedef enum {
  SYNA_CLOSE_STEP_CLEANUP_TX = 0,
  SYNA_CLOSE_STEP_CLEANUP_RX,
  SYNA_CLOSE_STEP_TLS_ALERT_TX,
  SYNA_CLOSE_STEP_TLS_ALERT_RX,
  SYNA_CLOSE_STEP_RELEASE_USB,
  SYNA_CLOSE_NUM_STATES,
} SynaCloseStep;

typedef void (*SynaSsmUsbRunner) (FpiSsm   *ssm,
                                  FpDevice *dev,
                                  gpointer  user_data);

FpiSsm *syna_ssm_close_new (FpDevice        *dev,
                            SynaSsmUsbRunner runner,
                            gpointer         user_data);

gboolean syna_ssm_close_cancellable_valid (GCancellable *cancellable);
