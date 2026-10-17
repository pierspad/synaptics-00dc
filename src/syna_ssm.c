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

/*
 * Synaptics 06cb:00dc WBDI Async FpiSsm + FpiUsbTransfer Driver Engine
 *
 * Non-blocking sequential state machines (FpiSsm) for upstream libfprint:
 *   1. OPEN_SSM:
 *      - Reset & Claim interface 0
 *      - 0x01 ROM Version check (ROM 6.20 build 154, product 0x33)
 *      - 0x75 Sensor Module ID check (0x0190:0x00a2 -> 86C TM-P3568-001)
 *      - Optional first-boot host pairing (0x4f Ownership + 0x50 Public Material)
 *      - TLS 1.2 ClientHello -> ServerHello -> Flight 2 -> Server Finished
 *      - Encrypted 0x1a Pre-config Cleanup + 0x06 Capture Initializer
 *      - Baseline loading/calibration must be supplied by the operation orchestrator
 *   2. CAPTURE_SSM:
 *      - Encrypted 0x02 Capture Command TX/RX
 *      - Asynchronous 17024-byte raw Mode-2 bulk stream reads on EP 0x82
 *      - Encrypted 0x04 Capture Stop TX/RX
 */

#include "syna_ssm.h"
#include "syna_proto.h"
#include "syna_mode2.h"
#include "syna_matcher.h"
#include "syna_align_frames.h"
#include <math.h>
#include <string.h>

/* Pure policy helpers are kept in this source file so offline tests exercise
 * the decisions used directly by the asynchronous transfer handlers. */
static gboolean
syna_ssm_plain_response_ok (uint8_t command, const uint8_t *plain, size_t plain_len)
{
  uint16_t status = 0xffff;

  return syna_response_ok (command, plain, plain_len, &status);
}

static gboolean
syna_ssm_deadline_expired (gint64 deadline_us, gint64 now_us)
{
  return deadline_us > 0 && now_us >= deadline_us;
}

static guint
syna_ssm_deadline_timeout_ms (gint64 deadline_us, gint64 now_us, guint max_timeout_ms)
{
  if (deadline_us <= 0)
    return max_timeout_ms;
  if (now_us >= deadline_us)
    return 0;

  gint64 remaining_us = deadline_us - now_us;
  guint remaining_ms = (guint) MAX (1, (remaining_us + 999) / 1000);
  return MIN (remaining_ms, max_timeout_ms);
}

static gboolean
syna_ssm_capture_start_response_needed (gboolean response_validated)
{
  return !response_validated;
}

static gboolean
syna_ssm_should_release_open_interface (gboolean interface_claimed, gboolean open_succeeded)
{
  return interface_claimed && !open_succeeded;
}

typedef enum {
  SYNA_OPEN_RESET_AND_CLAIM = 0,
  SYNA_OPEN_GET_VERSION_TX,
  SYNA_OPEN_GET_VERSION_RX,
  SYNA_OPEN_GET_SENSOR_ID_TX,
  SYNA_OPEN_GET_SENSOR_ID_RX,
  SYNA_OPEN_LOAD_OR_PAIR_TX_4F,
  SYNA_OPEN_PAIR_RX_4F,
  SYNA_OPEN_PAIR_TX_50,
  SYNA_OPEN_PAIR_RX_50,
  SYNA_OPEN_TLS_CLIENT_HELLO_TX,
  SYNA_OPEN_TLS_SERVER_HELLO_RX,
  SYNA_OPEN_TLS_FLIGHT2_TX,
  SYNA_OPEN_TLS_FLIGHT2_RX,
  SYNA_OPEN_CLEANUP_1A_TX,
  SYNA_OPEN_CLEANUP_1A_RX,
  SYNA_OPEN_INIT_06_TX,
  SYNA_OPEN_INIT_06_RX,
  SYNA_OPEN_INIT_06_VALIDATE,
  SYNA_OPEN_RELEASE_INTERFACE,
  SYNA_OPEN_NUM_STATES
} SynaOpenState;

typedef struct
{
  FpiDeviceSyna00dc *self;
  char              *pending_key_pem;
  uint8_t            ownership_reply[512];
  size_t             ownership_reply_len;
  uint8_t            rx_buf[4096];
  size_t             rx_len;
  gboolean           interface_claimed;
  gboolean           open_succeeded;
  gboolean           reset_retried;
} SynaOpenSsmData;

static void
syna_open_ssm_data_free (gpointer data)
{
  SynaOpenSsmData *ctx = data;

  if (!ctx)
    return;
  free (ctx->pending_key_pem);
  g_free (ctx);
}

static void
syna_ssm_bulk_tx (FpiSsm *ssm, FpDevice *dev, const uint8_t *buf, size_t len, guint timeout_ms)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_OUT, len);
  memcpy (t->buffer, buf, len);
  fpi_usb_transfer_submit (t, timeout_ms, fpi_device_get_cancellable (dev),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
syna_ssm_bulk_rx_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  SynaOpenSsmData *ctx = user_data;
  if (error)
    {
      if (fpi_ssm_get_cur_state (transfer->ssm) == SYNA_OPEN_GET_VERSION_RX &&
          !ctx->reset_retried &&
          !g_cancellable_is_cancelled (fpi_device_get_cancellable (device)))
        {
          g_clear_error (&error);
          ctx->reset_retried = TRUE;
          if (ctx->interface_claimed && ctx->self && ctx->self->usb_device)
            {
              g_usb_device_release_interface (ctx->self->usb_device, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, NULL);
              ctx->interface_claimed = FALSE;
            }
          if (ctx->self && ctx->self->usb_device)
            g_usb_device_reset (ctx->self->usb_device, NULL);
          fpi_ssm_jump_to_state (transfer->ssm, SYNA_OPEN_RESET_AND_CLAIM);
          return;
        }
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (transfer->actual_length < 0 || (size_t) transfer->actual_length > sizeof (ctx->rx_buf))
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Invalid USB bulk response length"));
      return;
    }
  memcpy (ctx->rx_buf, transfer->buffer, (size_t) transfer->actual_length);
  ctx->rx_len = (size_t) transfer->actual_length;
  fpi_ssm_next_state (transfer->ssm);
}

static void
syna_ssm_bulk_rx (FpiSsm *ssm, FpDevice *dev, SynaOpenSsmData *ctx, size_t max_len, guint timeout_ms)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  t->ssm = ssm;
  fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_IN, max_len);
  fpi_usb_transfer_submit (t, timeout_ms, fpi_device_get_cancellable (dev),
                           syna_ssm_bulk_rx_cb, ctx);
}

static void
syna_open_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (dev);
  SynaOpenSsmData *ctx = fpi_ssm_get_data (ssm);
  GError *error = NULL;

  switch ((SynaOpenState) fpi_ssm_get_cur_state (ssm))
    {
    case SYNA_OPEN_RESET_AND_CLAIM:
      self->usb_device = fpi_device_get_usb_device (dev);
      if (!self->usb_device)
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "No GUsbDevice attached"));
          return;
        }
      if (!g_usb_device_claim_interface (self->usb_device, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, &error))
        {
          g_clear_error (&error);
          g_usb_device_reset (self->usb_device, NULL);
          if (!g_usb_device_claim_interface (self->usb_device, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, &error))
            {
              fpi_ssm_mark_failed (ssm, error);
              return;
            }
        }
      ctx->interface_claimed = TRUE;
      fpi_ssm_next_state (ssm);
      break;

    case SYNA_OPEN_GET_VERSION_TX: {
        uint8_t cmd = SYNA_CMD_VERSION;
        syna_ssm_bulk_tx (ssm, dev, &cmd, 1, 3000);
        break;
      }

    case SYNA_OPEN_GET_VERSION_RX:
      syna_ssm_bulk_rx (ssm, dev, ctx, 64, 1500);
      break;

    case SYNA_OPEN_GET_SENSOR_ID_TX: {
        SynaVersionInfo ver_info;
        if (syna_parse_version_response (ctx->rx_buf, ctx->rx_len, &ver_info) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                                "Unsupported Synaptics ROM version"));
            return;
          }
        uint8_t cmd = SYNA_CMD_SENSOR_ID;
        syna_ssm_bulk_tx (ssm, dev, &cmd, 1, 3000);
        break;
      }

    case SYNA_OPEN_GET_SENSOR_ID_RX:
      syna_ssm_bulk_rx (ssm, dev, ctx, 32, 5000);
      break;

    case SYNA_OPEN_LOAD_OR_PAIR_TX_4F: {
        SynaSensorIdInfo id_info;
        if (syna_parse_sensor_id_response (ctx->rx_buf, ctx->rx_len, &id_info) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                                "Unsupported Synaptics sensor module ID"));
            return;
          }
        if (syna_oem_load (NULL, &self->oem) == 0)
          {
            /* Credentials already present on disk -> jump directly to TLS ClientHello */
            fpi_ssm_jump_to_state (ssm, SYNA_OPEN_TLS_CLIENT_HELLO_TX);
            return;
          }
        const char *allow_pair = getenv ("SYNA_ALLOW_AUTO_PAIR");
        if (!allow_pair || strcmp (allow_pair, "1") != 0 || !syna_oem_pairing_allowed (NULL))
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_NOT_FOUND,
                                                                "OEM credentials exist, are inaccessible, or pairing is disabled; refusing automatic re-pair"));
            return;
          }
        uint8_t own_req[453];
        size_t own_req_len = 0;
        if (syna_pairing_generate_request (&ctx->pending_key_pem, own_req, &own_req_len) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                "Failed to generate 0x4f pairing request"));
            return;
          }
        syna_ssm_bulk_tx (ssm, dev, own_req, own_req_len, 3000);
        break;
      }

    case SYNA_OPEN_PAIR_RX_4F:
      syna_ssm_bulk_rx (ssm, dev, ctx, 512, 5000);
      break;

    case SYNA_OPEN_PAIR_TX_50: {
        if (ctx->rx_len > sizeof (ctx->ownership_reply))
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "0x4f reply overflow"));
            return;
          }
        memcpy (ctx->ownership_reply, ctx->rx_buf, ctx->rx_len);
        ctx->ownership_reply_len = ctx->rx_len;
        uint8_t pub_cmd[13] = { SYNA_CMD_PUBLIC_MAT, 0 };
        syna_ssm_bulk_tx (ssm, dev, pub_cmd, sizeof (pub_cmd), 3000);
        break;
      }

    case SYNA_OPEN_PAIR_RX_50:
      syna_ssm_bulk_rx (ssm, dev, ctx, 1024, 5000);
      break;

    case SYNA_OPEN_TLS_CLIENT_HELLO_TX: {
        if (ctx->pending_key_pem)
          {
            uint8_t host_cert[184];
            size_t host_cert_len = 0;
            uint8_t server_pub[65];
            size_t server_pub_len = 0;
            if (syna_pairing_extract_credentials (ctx->ownership_reply, ctx->ownership_reply_len,
                                                  ctx->rx_buf, ctx->rx_len,
                                                  host_cert, &host_cert_len,
                                                  server_pub, &server_pub_len) != 0 ||
                syna_oem_save_credentials (NULL, ctx->pending_key_pem,
                                           host_cert, host_cert_len,
                                           server_pub, server_pub_len) != 0 ||
                syna_oem_load (NULL, &self->oem) != 0)
              {
                fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                    "Failed to extract/save paired credentials"));
                return;
              }
            free (ctx->pending_key_pem);
            ctx->pending_key_pem = NULL;
          }
        if (syna_tls_init (&self->tls, self->oem.host_key_pem,
                           self->oem.host_cert, self->oem.host_cert_len,
                           self->oem.server_ecdh_pub, self->oem.server_ecdh_pub_len) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "TLS init failed"));
            return;
          }
        uint8_t hello[128];
        size_t hello_len = 0;
        syna_tls_build_client_hello (&self->tls, hello, &hello_len);
        syna_ssm_bulk_tx (ssm, dev, hello, hello_len, 3000);
        break;
      }

    case SYNA_OPEN_TLS_SERVER_HELLO_RX:
      syna_ssm_bulk_rx (ssm, dev, ctx, 512, 5000);
      break;

    case SYNA_OPEN_TLS_FLIGHT2_TX: {
        if (syna_tls_handle_server_hello (&self->tls, ctx->rx_buf, ctx->rx_len) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "ServerHello parse failed"));
            return;
          }
        uint8_t flight2[1024];
        size_t flight2_len = 0;
        if (syna_tls_build_flight2 (&self->tls, flight2, &flight2_len) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "TLS Flight 2 build failed"));
            return;
          }
        syna_ssm_bulk_tx (ssm, dev, flight2, flight2_len, 3000);
        break;
      }

    case SYNA_OPEN_TLS_FLIGHT2_RX:
      syna_ssm_bulk_rx (ssm, dev, ctx, 512, 5000);
      break;

    case SYNA_OPEN_CLEANUP_1A_TX: {
        if (syna_tls_handle_flight2_reply (&self->tls, ctx->rx_buf, ctx->rx_len) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "TLS Finished verify failed"));
            return;
          }
        uint8_t clean_cmd = SYNA_CMD_CLEANUP;
        uint8_t enc[128];
        size_t enc_len = 0;
        if (syna_tls_encrypt_app_data (&self->tls, &clean_cmd, 1, enc, &enc_len) != 0)
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "TLS encrypt 0x1a failed"));
            return;
          }
        syna_ssm_bulk_tx (ssm, dev, enc, enc_len, 3000);
        break;
      }

    case SYNA_OPEN_CLEANUP_1A_RX:
      syna_ssm_bulk_rx (ssm, dev, ctx, 256, 5000);
      break;

    case SYNA_OPEN_INIT_06_TX: {
        uint8_t plain[64];
        size_t plain_len = 0;
        if (syna_tls_decrypt_app_data (&self->tls, ctx->rx_buf, ctx->rx_len, plain, sizeof (plain), &plain_len) != 0 ||
            !syna_ssm_plain_response_ok (SYNA_CMD_CLEANUP, plain, plain_len))
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "0x1a cleanup rejected"));
            return;
          }
        size_t req_len = 1 + self->oem.capture_init_blob_len;
        uint8_t *req = g_malloc (req_len);
        req[0] = SYNA_CMD_INIT;
        memcpy (req + 1, self->oem.capture_init_blob, self->oem.capture_init_blob_len);

        uint8_t *enc = g_malloc (req_len + 512);
        size_t enc_len = 0;
        int rc = syna_tls_encrypt_app_data (&self->tls, req, req_len, enc, &enc_len);
        g_free (req);
        if (rc != 0)
          {
            g_free (enc);
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "TLS encrypt 0x06 failed"));
            return;
          }
        syna_ssm_bulk_tx (ssm, dev, enc, enc_len, 3000);
        g_free (enc);
        break;
      }

    case SYNA_OPEN_INIT_06_RX: {
        syna_ssm_bulk_rx (ssm, dev, ctx, 256, 5000);
        break;
      }

    case SYNA_OPEN_INIT_06_VALIDATE: {
        uint8_t plain[64];
        size_t plain_len = 0;
        if (syna_tls_decrypt_app_data (&self->tls, ctx->rx_buf, ctx->rx_len,
                                       plain, sizeof (plain), &plain_len) != 0 ||
            !syna_ssm_plain_response_ok (SYNA_CMD_INIT, plain, plain_len))
          {
            fpi_ssm_mark_failed (ssm, fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                "0x06 capture initializer rejected"));
            return;
          }
        ctx->open_succeeded = TRUE;
        self->capture_pipeline_ready = TRUE;
        fpi_ssm_mark_completed (ssm);
        break;
      }

    case SYNA_OPEN_RELEASE_INTERFACE:
      /* The SSM enters this state after success and on failed opens. */
      if (syna_ssm_should_release_open_interface (ctx->interface_claimed,
                                                  ctx->open_succeeded))
        {
          g_usb_device_release_interface (self->usb_device, 0,
                                          G_USB_DEVICE_CLAIM_INTERFACE_NONE, NULL);
          ctx->interface_claimed = FALSE;
        }
      fpi_ssm_mark_completed (ssm);
      break;

    case SYNA_OPEN_NUM_STATES:
    default:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

FpiSsm *
syna_ssm_open_new (FpiDeviceSyna00dc *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiSsm *ssm = fpi_ssm_new_full (dev, syna_open_ssm_handler,
                                  SYNA_OPEN_NUM_STATES,
                                  SYNA_OPEN_RELEASE_INTERFACE,
                                  "syna_open_ssm");
  SynaOpenSsmData *ctx = g_new0 (SynaOpenSsmData, 1);

  ctx->self = self;
  fpi_ssm_set_data (ssm, ctx, syna_open_ssm_data_free);
  return ssm;
}

/* ============================================================================
 * 2. CAPTURE_SSM: Asynchronous 0x02 Stream + 17024B Bulk Frame Loop + 0x04 Stop
 * ============================================================================ */

#define SYNA_SSM_MAX_CONTACT_FRAMES 12
#define SYNA_SSM_TARGET_FRAMES 6
#define SYNA_SSM_MIN_FRAMES 5
#define SYNA_SSM_CAPTURE_TIMEOUT_US (45 * G_TIME_SPAN_SECOND)

typedef enum {
  SYNA_CAP_CHECK_PIPELINE = 0,
  SYNA_CAP_CLEANUP_1A_TX,
  SYNA_CAP_CLEANUP_1A_RX,
  SYNA_CAP_INIT_06_TX,
  SYNA_CAP_INIT_06_RX,
  SYNA_CAP_START_02_TX,
  SYNA_CAP_START_02_RX,
  SYNA_CAP_READ_FRAME_CHUNK,
  SYNA_CAP_STOP_04_TX,
  SYNA_CAP_STOP_04_RX,
  SYNA_CAP_STOP_04_VALIDATE,
  SYNA_CAP_FINISH,
  SYNA_CAP_NUM_STATES
} SynaCaptureState;

typedef struct
{
  FpiDeviceSyna00dc           *self;
  gboolean                     require_finger_lift;
  gboolean                     more_stages_needed;
  SynaCaptureCompletedCallback cb;
  gpointer                     user_data;

  uint8_t                      rx_buf[4096];
  size_t                       rx_len;

  uint8_t                      frame_buf[SYNA_FRAME_BYTES];
  size_t                       frame_offset;

  float                        collected_frames[SYNA_SSM_MAX_CONTACT_FRAMES][SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES];
  int                          valid_contact_frames;
  int                          transient_seen;
  gboolean                     sensor_cleared;
  gboolean                     finger_detected;
  int                          empty_consecutive_frames;
  gboolean                     capture_response_validated;
  gint64                       capture_deadline_us;
  float                        min_rms;
  float                        max_rms;

  GError                      *capture_error;
} SynaCaptureSsmData;

static void
syna_capture_ssm_data_free (gpointer data)
{
  SynaCaptureSsmData *ctx = data;

  if (!ctx)
    return;
  if (ctx->capture_error)
    g_error_free (ctx->capture_error);
  g_free (ctx);
}

static float
syna_ssm_calc_rms (const float *frame, const float *baseline, size_t n)
{
  double sum_sq = 0.0;

  for (size_t i = 0; i < n; i++)
    {
      double diff = (double) frame[i] - (double) baseline[i];
      sum_sq += diff * diff;
    }
  return (float) sqrt (sum_sq / (double) n);
}


static void
syna_capture_store_error_and_jump (FpiSsm             *ssm,
                                   SynaCaptureSsmData *ctx,
                                   GError             *error,
                                   SynaCaptureState    target)
{
  if (ctx->capture_error)
    g_error_free (error);
  else
    ctx->capture_error = error;
  fpi_ssm_jump_to_state (ssm, target);
}

static void
syna_cap_tx_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  (void) device;
  SynaCaptureSsmData *ctx = user_data;
  if (error)
    {
      SynaCaptureState target = fpi_ssm_get_cur_state (transfer->ssm) == SYNA_CAP_STOP_04_TX ?
                                SYNA_CAP_FINISH : SYNA_CAP_STOP_04_TX;
      syna_capture_store_error_and_jump (transfer->ssm, ctx, error, target);
      return;
    }
  fpi_ssm_next_state (transfer->ssm);
}

static void
syna_cap_bulk_tx (FpiSsm             *ssm,
                  FpDevice           *dev,
                  SynaCaptureSsmData *ctx,
                  const uint8_t      *buf,
                  size_t              len,
                  guint               timeout_ms,
                  gboolean            cancellable)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_OUT, len);
  memcpy (t->buffer, buf, len);
  fpi_usb_transfer_submit (t, timeout_ms,
                           cancellable ? fpi_device_get_cancellable (dev) : NULL,
                           syna_cap_tx_cb, ctx);
}

static void
syna_cap_cmd_rx_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  (void) device;
  SynaCaptureSsmData *ctx = user_data;
  if (error)
    {
      SynaCaptureState target = fpi_ssm_get_cur_state (transfer->ssm) == SYNA_CAP_STOP_04_RX ?
                                SYNA_CAP_FINISH : SYNA_CAP_STOP_04_TX;
      syna_capture_store_error_and_jump (transfer->ssm, ctx, error, target);
      return;
    }
  if (transfer->actual_length < 0 || (size_t) transfer->actual_length > sizeof (ctx->rx_buf))
    {
      GError *err = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                              "Invalid USB command response length");
      SynaCaptureState target = fpi_ssm_get_cur_state (transfer->ssm) == SYNA_CAP_STOP_04_RX ?
                                SYNA_CAP_FINISH : SYNA_CAP_STOP_04_TX;
      syna_capture_store_error_and_jump (transfer->ssm, ctx, err, target);
      return;
    }
  memcpy (ctx->rx_buf, transfer->buffer, (size_t) transfer->actual_length);
  ctx->rx_len = (size_t) transfer->actual_length;
  fpi_ssm_next_state (transfer->ssm);
}

static void
syna_cap_frame_chunk_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (device);
  SynaCaptureSsmData *ctx = user_data;

  if (error)
    {
      ctx->capture_error = error;
      fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_STOP_04_TX);
      return;
    }
  if (syna_ssm_deadline_expired (ctx->capture_deadline_us,
                                 g_get_monotonic_time ()))
    {
      ctx->capture_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                "Capture deadline exceeded (45 seconds)");
      fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_STOP_04_TX);
      return;
    }
  if (transfer->actual_length <= 0)
    {
      ctx->capture_error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "Zero bytes read from EP 0x82 stream");
      fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_STOP_04_TX);
      return;
    }

  size_t copy_len = (size_t) transfer->actual_length;
  if (ctx->frame_offset + copy_len > SYNA_FRAME_BYTES)
    copy_len = SYNA_FRAME_BYTES - ctx->frame_offset;
  memcpy (ctx->frame_buf + ctx->frame_offset, transfer->buffer, copy_len);
  ctx->frame_offset += copy_len;

  if (ctx->frame_offset < SYNA_FRAME_BYTES)
    {
      /* Need more bulk chunks to complete the 17,024-byte frame */
      fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_READ_FRAME_CHUNK);
      return;
    }

  /* Full 17,024-byte frame assembled */
  ctx->frame_offset = 0;

  if (syna_validate_frame (ctx->frame_buf, NULL) == 0 && self->has_baseline && self->baseline)
    {
      float dec[SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES];
      syna_decode_frame_unclipped (ctx->frame_buf, dec);

      float rms = syna_ssm_calc_rms (dec, self->baseline, G_N_ELEMENTS (dec));
      ctx->min_rms = fminf (ctx->min_rms, rms);
      ctx->max_rms = fmaxf (ctx->max_rms, rms);

      if (!ctx->sensor_cleared)
        {
          if (!ctx->require_finger_lift && rms > 35.0f)
            {
              ctx->sensor_cleared = TRUE;
              ctx->finger_detected = TRUE;
              fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NONE);
              for (size_t i = 0; i < G_N_ELEMENTS (dec); i++)
                ctx->collected_frames[ctx->valid_contact_frames][i] = dec[i] - self->baseline[i];
              ctx->valid_contact_frames++;
            }
          else if (rms <= 28.0f)
            {
              ctx->empty_consecutive_frames++;
              if (ctx->empty_consecutive_frames >= (ctx->require_finger_lift ? 2 : 1))
                {
                  ctx->sensor_cleared = TRUE;
                  fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_PRESENT);
                }
            }
          else
            {
              ctx->empty_consecutive_frames = 0;
            }
        }
      else
        {
          if (rms > 35.0f)
            {
              if (!ctx->finger_detected)
                {
                  ctx->finger_detected = TRUE;
                  fpi_device_report_finger_status_changes (device, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NEEDED);
                }
              ctx->transient_seen++;
              if (ctx->transient_seen > 1)
                {
                  for (size_t i = 0; i < G_N_ELEMENTS (dec); i++)
                    ctx->collected_frames[ctx->valid_contact_frames][i] = dec[i] - self->baseline[i];
                  ctx->valid_contact_frames++;
                }
            }
          else if (ctx->finger_detected)
            {
              /* Finger lifted early: stop stream and evaluate */
              fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_STOP_04_TX);
              return;
            }
        }
    }

  if (ctx->valid_contact_frames >= SYNA_SSM_TARGET_FRAMES)
    fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_STOP_04_TX);
  else
    fpi_ssm_jump_to_state (transfer->ssm, SYNA_CAP_READ_FRAME_CHUNK);
}

static void
syna_capture_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (dev);
  SynaCaptureSsmData *ctx = fpi_ssm_get_data (ssm);

  switch ((SynaCaptureState) fpi_ssm_get_cur_state (ssm))
    {
    case SYNA_CAP_CHECK_PIPELINE:
      if (!self->has_baseline || !self->baseline)
        {
          GError *err = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_NOT_FOUND,
                                                  "Capture baseline is unavailable");
          if (ctx->cb)
            ctx->cb (self, NULL, ctx->user_data, err);
          fpi_ssm_mark_completed (ssm);
          return;
        }
      if (self->capture_pipeline_ready)
        fpi_ssm_jump_to_state (ssm, SYNA_CAP_START_02_TX);
      else
        fpi_ssm_next_state (ssm);
      break;

    case SYNA_CAP_CLEANUP_1A_TX: {
        uint8_t cmd = SYNA_CMD_CLEANUP;
        uint8_t enc[128];
        size_t enc_len = 0;
        if (syna_tls_encrypt_app_data (&self->tls, &cmd, 1, enc, &enc_len) != 0)
          {
            syna_capture_store_error_and_jump (ssm, ctx,
                                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                         "TLS encrypt 0x1a failed"),
                                               SYNA_CAP_FINISH);
            return;
          }
        syna_cap_bulk_tx (ssm, dev, ctx, enc, enc_len, 3000, TRUE);
        break;
      }

    case SYNA_CAP_CLEANUP_1A_RX: {
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_IN, 256);
        fpi_usb_transfer_submit (t, 5000, fpi_device_get_cancellable (dev), syna_cap_cmd_rx_cb, ctx);
        break;
      }

    case SYNA_CAP_INIT_06_TX: {
        uint8_t plain[64];
        size_t plain_len = 0;
        if (syna_tls_decrypt_app_data (&self->tls, ctx->rx_buf, ctx->rx_len, plain, sizeof (plain), &plain_len) != 0 ||
            !syna_ssm_plain_response_ok (SYNA_CMD_CLEANUP, plain, plain_len))
          {
            syna_capture_store_error_and_jump (ssm, ctx,
                                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                         "0x1a cleanup rejected"),
                                               SYNA_CAP_FINISH);
            return;
          }
        size_t req_len = 1 + self->oem.capture_init_blob_len;
        uint8_t *req = g_malloc (req_len);
        req[0] = SYNA_CMD_INIT;
        memcpy (req + 1, self->oem.capture_init_blob, self->oem.capture_init_blob_len);

        uint8_t *enc = g_malloc (req_len + 512);
        size_t enc_len = 0;
        int rc = syna_tls_encrypt_app_data (&self->tls, req, req_len, enc, &enc_len);
        g_free (req);
        if (rc != 0)
          {
            g_free (enc);
            syna_capture_store_error_and_jump (ssm, ctx,
                                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                         "TLS encrypt 0x06 failed"),
                                               SYNA_CAP_FINISH);
            return;
          }
        syna_cap_bulk_tx (ssm, dev, ctx, enc, enc_len, 3000, TRUE);
        g_free (enc);
        break;
      }

    case SYNA_CAP_INIT_06_RX: {
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_IN, 256);
        fpi_usb_transfer_submit (t, 5000, fpi_device_get_cancellable (dev), syna_cap_cmd_rx_cb, ctx);
        break;
      }

    case SYNA_CAP_START_02_TX: {
        if (!self->capture_pipeline_ready)
          {
            uint8_t plain[64];
            size_t plain_len = 0;
            if (syna_tls_decrypt_app_data (&self->tls, ctx->rx_buf, ctx->rx_len, plain, sizeof (plain), &plain_len) != 0 ||
                !syna_ssm_plain_response_ok (SYNA_CMD_INIT, plain, plain_len))
              {
                syna_capture_store_error_and_jump (ssm, ctx,
                                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                             "0x06 init rejected"),
                                                   SYNA_CAP_FINISH);
                return;
              }
            self->capture_pipeline_ready = TRUE;
          }
        size_t enc_alloc = self->oem.capture_packet_len + 512;
        uint8_t *enc = g_malloc (enc_alloc);
        size_t enc_len = 0;
        if (syna_tls_encrypt_app_data (&self->tls, self->oem.capture_packet, self->oem.capture_packet_len,
                                       enc, &enc_len) != 0)
          {
            g_free (enc);
            syna_capture_store_error_and_jump (ssm, ctx,
                                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                         "TLS encrypt 0x02 failed"),
                                               SYNA_CAP_FINISH);
            return;
          }
        syna_cap_bulk_tx (ssm, dev, ctx, enc, enc_len, 3000, TRUE);
        g_free (enc);
        break;
      }

    case SYNA_CAP_START_02_RX: {
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_IN, 4096);
        fpi_usb_transfer_submit (t, 5000, fpi_device_get_cancellable (dev), syna_cap_cmd_rx_cb, ctx);
        break;
      }

    case SYNA_CAP_READ_FRAME_CHUNK: {
        if (syna_ssm_deadline_expired (ctx->capture_deadline_us,
                                       g_get_monotonic_time ()))
          {
            ctx->capture_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                      "Capture deadline exceeded (45 seconds)");
            fpi_ssm_jump_to_state (ssm, SYNA_CAP_STOP_04_TX);
            return;
          }
        if (syna_ssm_capture_start_response_needed (ctx->capture_response_validated))
          {
            uint8_t plain[4096];
            size_t plain_len = 0;
            if (syna_tls_decrypt_app_data (&self->tls, ctx->rx_buf, ctx->rx_len, plain, sizeof (plain), &plain_len) != 0 ||
                !syna_ssm_plain_response_ok (SYNA_CMD_CAPTURE, plain, plain_len))
              {
                syna_capture_store_error_and_jump (ssm, ctx,
                                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                                             "0x02 capture start rejected"),
                                                   SYNA_CAP_STOP_04_TX);
                return;
              }
            ctx->capture_response_validated = TRUE;
            ctx->capture_deadline_us = g_get_monotonic_time () + SYNA_SSM_CAPTURE_TIMEOUT_US;
          }
        guint timeout_ms = syna_ssm_deadline_timeout_ms (ctx->capture_deadline_us,
                                                         g_get_monotonic_time (),
                                                         15000);
        if (timeout_ms == 0)
          {
            ctx->capture_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                                                      "Capture deadline exceeded (45 seconds)");
            fpi_ssm_jump_to_state (ssm, SYNA_CAP_STOP_04_TX);
            return;
          }
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk (t, SYNA_EP_DATA_IN, SYNA_FRAME_BYTES - ctx->frame_offset);
        fpi_usb_transfer_submit (t, timeout_ms, fpi_device_get_cancellable (dev),
                                 syna_cap_frame_chunk_cb, ctx);
        break;
      }

    case SYNA_CAP_STOP_04_TX: {
        if (!ctx->more_stages_needed)
          {
            if (ctx->finger_detected)
              fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_PRESENT);
            else if (ctx->sensor_cleared)
              fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_NEEDED);
          }
        uint8_t stop_cmd = SYNA_CMD_STOP;
        uint8_t enc[128];
        size_t enc_len = 0;
        if (syna_tls_encrypt_app_data (&self->tls, &stop_cmd, 1, enc, &enc_len) != 0)
          {
            syna_capture_store_error_and_jump (ssm, ctx,
                                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                                         "TLS encrypt 0x04 failed"),
                                               SYNA_CAP_FINISH);
            return;
          }
        syna_cap_bulk_tx (ssm, dev, ctx, enc, enc_len, 3000, FALSE);
        break;
      }

    case SYNA_CAP_STOP_04_RX: {
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);
        t->ssm = ssm;
        fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_IN, 256);
        fpi_usb_transfer_submit (t, 3000, NULL, syna_cap_cmd_rx_cb, ctx);
        break;
      }

    case SYNA_CAP_STOP_04_VALIDATE: {
        uint8_t plain[64];
        size_t plain_len = 0;
        if (syna_tls_decrypt_app_data (&self->tls, ctx->rx_buf, ctx->rx_len,
                                       plain, sizeof (plain), &plain_len) != 0 ||
            !syna_ssm_plain_response_ok (SYNA_CMD_STOP, plain, plain_len))
          {
            if (!ctx->capture_error)
              ctx->capture_error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "0x04 capture stop rejected");
          }
        fpi_ssm_next_state (ssm);
        break;
      }

    case SYNA_CAP_FINISH: {
        self->capture_pipeline_ready = FALSE;
        self->had_capture_in_session = TRUE;
        if (ctx->capture_error)
          {
            GError *err = g_steal_pointer (&ctx->capture_error);
            if (ctx->cb)
              ctx->cb (self, NULL, ctx->user_data, err);
            fpi_ssm_mark_completed (ssm);
            return;
          }
        if (ctx->valid_contact_frames < SYNA_SSM_MIN_FRAMES)
          {
            GError *err = fpi_device_retry_new (ctx->finger_detected ? FP_DEVICE_RETRY_TOO_SHORT : FP_DEVICE_RETRY_GENERAL);
            if (ctx->cb)
              ctx->cb (self, NULL, ctx->user_data, err);
            fpi_ssm_mark_completed (ssm);
            return;
          }
        float quality = 0.0f;
        float *avg = syna_align_frames (&ctx->collected_frames[0][0], ctx->valid_contact_frames, &quality);
        if (!avg || quality < 50.0f)
          {
            g_free (avg);
            GError *err = fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER);
            if (ctx->cb)
              ctx->cb (self, NULL, ctx->user_data, err);
            fpi_ssm_mark_completed (ssm);
            return;
          }
        float *filtered = g_malloc (SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES * sizeof (float));
        if (syna_bandpass_filter (avg, filtered) != 0)
          {
            g_free (avg);
            g_free (filtered);
            GError *err = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "Bandpass filter failed");
            if (ctx->cb)
              ctx->cb (self, NULL, ctx->user_data, err);
            fpi_ssm_mark_completed (ssm);
            return;
          }
        g_free (avg);
        if (ctx->cb)
          ctx->cb (self, filtered, ctx->user_data, NULL);
        fpi_ssm_mark_completed (ssm);
        break;
      }

    case SYNA_CAP_NUM_STATES:
    default:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

FpiSsm *
syna_ssm_capture_new (FpiDeviceSyna00dc           *self,
                      gboolean                     require_finger_lift,
                      gboolean                     more_stages_needed,
                      SynaCaptureCompletedCallback cb,
                      gpointer                     user_data)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiSsm *ssm = fpi_ssm_new (dev, syna_capture_ssm_handler, SYNA_CAP_NUM_STATES);
  SynaCaptureSsmData *ctx = g_new0 (SynaCaptureSsmData, 1);

  ctx->self = self;
  ctx->require_finger_lift = require_finger_lift;
  ctx->more_stages_needed = more_stages_needed;
  ctx->cb = cb;
  ctx->user_data = user_data;
  ctx->min_rms = INFINITY;
  ctx->max_rms = 0.0f;
  fpi_ssm_set_data (ssm, ctx, syna_capture_ssm_data_free);
  return ssm;
}

/* ============================================================================
 * 3. CLOSE_SSM: Non-blocking Teardown (0x1a Cleanup + TLS Alert + Release USB)
 * ============================================================================ */

gboolean
syna_ssm_close_cancellable_valid (GCancellable *cancellable)
{
  /* Teardown operations must be uncancellable (cancellable == NULL) */
  return cancellable == NULL;
}

typedef struct
{
  FpiDeviceSyna00dc *self;
  SynaSsmUsbRunner   runner;
  gpointer           user_data;
  uint8_t            rx_buf[512];
  size_t             rx_len;
} SynaCloseSsmData;

static void
syna_close_ssm_data_free (gpointer data)
{
  SynaCloseSsmData *ctx = data;

  g_free (ctx);
}

static void
syna_ssm_close_bulk_rx_cb (FpiUsbTransfer *transfer, FpDevice *device, gpointer user_data, GError *error)
{
  (void) device;
  (void) user_data;
  if (error)
    g_clear_error (&error);
  fpi_ssm_next_state (transfer->ssm);
}

static void
syna_ssm_close_bulk_rx (FpiSsm *ssm, FpDevice *dev, size_t max_len, guint timeout_ms)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  t->ssm = ssm;
  fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_IN, max_len);
  fpi_usb_transfer_submit (t, timeout_ms, NULL, syna_ssm_close_bulk_rx_cb, NULL);
}

static void
syna_ssm_close_bulk_tx (FpiSsm *ssm, FpDevice *dev, const uint8_t *buf, size_t len, guint timeout_ms)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  t->ssm = ssm;
  t->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk (t, SYNA_EP_CMD_OUT, len);
  memcpy (t->buffer, buf, len);
  fpi_usb_transfer_submit (t, timeout_ms, NULL, fpi_ssm_usb_transfer_cb, NULL);
}

static void
syna_close_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  SynaCloseSsmData *ctx = fpi_ssm_get_data (ssm);
  FpiDeviceSyna00dc *self = ctx ? ctx->self : (FpiDeviceSyna00dc *) dev;
  int step = fpi_ssm_get_cur_state (ssm);

  if (ctx && ctx->runner)
    {
      ctx->runner (ssm, dev, ctx->user_data);
      return;
    }

  switch ((SynaCloseStep) step)
    {
    case SYNA_CLOSE_STEP_CLEANUP_TX: {
        if (!self || !self->tls.finished_verified)
          {
            fpi_ssm_jump_to_state (ssm, SYNA_CLOSE_STEP_RELEASE_USB);
            return;
          }
        uint8_t cmd = SYNA_CMD_CLEANUP;
        uint8_t enc[256];
        size_t enc_len = 0;
        if (syna_tls_encrypt_app_data (&self->tls, &cmd, 1, enc, &enc_len) != 0)
          {
            fpi_ssm_jump_to_state (ssm, SYNA_CLOSE_STEP_TLS_ALERT_TX);
            return;
          }
        syna_ssm_close_bulk_tx (ssm, dev, enc, enc_len, 1000);
        break;
      }

    case SYNA_CLOSE_STEP_CLEANUP_RX:
      syna_ssm_close_bulk_rx (ssm, dev, sizeof (ctx->rx_buf), 1000);
      break;

    case SYNA_CLOSE_STEP_TLS_ALERT_TX: {
        if (!self || !self->tls.finished_verified)
          {
            fpi_ssm_jump_to_state (ssm, SYNA_CLOSE_STEP_RELEASE_USB);
            return;
          }
        uint8_t alert[128];
        size_t alert_len = 0;
        if (syna_tls_build_close_notify (&self->tls, alert, &alert_len) != 0)
          {
            fpi_ssm_jump_to_state (ssm, SYNA_CLOSE_STEP_RELEASE_USB);
            return;
          }
        syna_ssm_close_bulk_tx (ssm, dev, alert, alert_len, 1000);
        break;
      }

    case SYNA_CLOSE_STEP_TLS_ALERT_RX:
      syna_ssm_close_bulk_rx (ssm, dev, sizeof (ctx->rx_buf), 1000);
      break;

    case SYNA_CLOSE_STEP_RELEASE_USB:
      if (self)
        {
          syna_tls_free (&self->tls);
          syna_oem_free (&self->oem);
          if (self->usb_device)
            g_usb_device_release_interface (self->usb_device, 0, 0, NULL);
          self->is_open = FALSE;
          self->capture_pipeline_ready = FALSE;
          self->had_capture_in_session = FALSE;
        }
      fpi_ssm_mark_completed (ssm);
      break;

    case SYNA_CLOSE_NUM_STATES:
    default:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

FpiSsm *
syna_ssm_close_new (FpDevice *dev, SynaSsmUsbRunner runner, gpointer user_data)
{
  FpiSsm *ssm = fpi_ssm_new_full (dev, syna_close_ssm_handler,
                                  SYNA_CLOSE_NUM_STATES,
                                  SYNA_CLOSE_STEP_RELEASE_USB,
                                  "syna_close_ssm");
  SynaCloseSsmData *ctx = g_new0 (SynaCloseSsmData, 1);

  ctx->self = (FpiDeviceSyna00dc *) dev;
  ctx->runner = runner;
  ctx->user_data = user_data;
  fpi_ssm_set_data (ssm, ctx, syna_close_ssm_data_free);
  return ssm;
}
