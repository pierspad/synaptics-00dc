#include "syna_device.h"
#include "syna_mode2.h"
#include "syna_matcher.h"
#include "syna_align_frames.h"
#include "syna_proto.h"
#include "syna_template.h"
#include "syna_timing.h"
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

G_DEFINE_TYPE (FpiDeviceSyna00dc, fpi_device_syna_00dc, FP_TYPE_DEVICE)

GType
fpi_tod_shared_driver_get_type (void)
{
    return FPI_TYPE_DEVICE_SYNA_00DC;
}

static const FpIdEntry syna_ids[] = {
    { { { .pid = SYNA_USB_PID, .vid = SYNA_USB_VID } }, 0 },
    { { { 0, 0 } }, 0 }
};

static void
fpi_device_syna_00dc_init (FpiDeviceSyna00dc *self)
{
    self->is_open = FALSE;
    self->main_context = g_main_context_ref_thread_default ();
    self->is_cancelling = FALSE;
    self->capture_pipeline_ready = FALSE;
    self->baseline = NULL;
    self->has_baseline = FALSE;
    self->has_cached_baseline = FALSE;
    memset (self->cached_baseline, 0, sizeof (self->cached_baseline));
    self->initial_contact_present = FALSE;
    self->enrolled_templates = g_ptr_array_new_with_free_func (g_free);
}

static void
fpi_device_syna_00dc_finalize (GObject *object)
{
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (object);

    if (self->enrolled_templates) {
        g_ptr_array_unref (self->enrolled_templates);
        self->enrolled_templates = NULL;
    }
    if (self->baseline) {
        g_free (self->baseline);
        self->baseline = NULL;
    }
    syna_tls_free (&self->tls);
    syna_oem_free (&self->oem);
    g_main_context_unref (self->main_context);

    G_OBJECT_CLASS (fpi_device_syna_00dc_parent_class)->finalize (object);
}

typedef struct {
    FpiDeviceSyna00dc *self;
    FpFingerStatusFlags added, removed;
    guint stage;
    GError *retry_error;
} SynaEvent;

static gboolean
syna_report_event (gpointer data)
{
    SynaEvent *event = data;
    if (event->retry_error) {
        fpi_device_enroll_progress (FP_DEVICE (event->self), event->stage, NULL, event->retry_error);
        g_error_free (event->retry_error);
    } else if (event->stage) {
        fpi_device_enroll_progress (FP_DEVICE (event->self), event->stage, NULL, NULL);
    } else {
        fpi_device_report_finger_status_changes (FP_DEVICE (event->self), event->added, event->removed);
    }
    g_object_unref (event->self);
    g_free (event);
    return G_SOURCE_REMOVE;
}

static void
syna_report_status (FpiDeviceSyna00dc *self, FpFingerStatusFlags added, FpFingerStatusFlags removed)
{
    SynaEvent *event = g_new0 (SynaEvent, 1);
    event->self = g_object_ref (self);
    event->added = added;
    event->removed = removed;
    g_main_context_invoke_full (self->main_context, G_PRIORITY_DEFAULT, syna_report_event, event, NULL);
}

static void
syna_report_stage (FpiDeviceSyna00dc *self, guint stage)
{
    SynaEvent *event = g_new0 (SynaEvent, 1);
    event->self = g_object_ref (self);
    event->stage = stage;
    g_main_context_invoke_full (self->main_context, G_PRIORITY_DEFAULT, syna_report_event, event, NULL);
}

static void
syna_report_enroll_retry (FpiDeviceSyna00dc *self, guint stage, FpDeviceRetry retry_type)
{
    SynaEvent *event = g_new0 (SynaEvent, 1);
    event->self = g_object_ref (self);
    event->stage = stage;
    event->retry_error = fpi_device_retry_new (retry_type);
    g_main_context_invoke_full (self->main_context, G_PRIORITY_DEFAULT, syna_report_event, event, NULL);
}

/* Forward declarations */
static gboolean
syna_read_full_frame (GUsbDevice *usb_device, uint8_t *frame_buf, gint64 deadline, GCancellable *cancellable, GError **error);

/* Helper to execute a synchronous USB bulk exchange */
static gboolean
syna_usb_cmd (GUsbDevice *usb_dev,
              const uint8_t *cmd, size_t cmd_len,
              uint8_t *resp, size_t max_resp_len, size_t *actual_resp_len,
              GCancellable *cancellable, GError **error)
{
    gsize transferred = 0;
    if (!g_usb_device_bulk_transfer (usb_dev, SYNA_EP_CMD_OUT,
                                     (guint8 *)cmd, cmd_len,
                                     &transferred, 3000,
                                     cancellable, error)) {
        return FALSE;
    }

    if (!g_usb_device_bulk_transfer (usb_dev, SYNA_EP_CMD_IN,
                                     (guint8 *)resp, max_resp_len,
                                     &transferred, 5000,
                                     cancellable, error)) {
        return FALSE;
    }

    if (actual_resp_len)
        *actual_resp_len = transferred;

    return TRUE;
}

/* Helper to send encrypted application payload and decrypt reply */
static gboolean
syna_tls_app_exchange (FpiDeviceSyna00dc *self,
                       const uint8_t *req, size_t req_len,
                       uint8_t *resp, size_t max_resp_len, size_t *resp_len,
                       GCancellable *cancellable, GError **error)
{
    size_t enc_alloc = req_len + 512;
    uint8_t *enc = g_malloc (enc_alloc);
    size_t enc_len = 0;

    if (syna_tls_encrypt_app_data (&self->tls, req, req_len, enc, &enc_len) != 0) {
        g_free (enc);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "TLS encryption failed");
        return FALSE;
    }

    uint8_t wire_resp[4096];
    size_t wire_resp_len = 0;
    gboolean ok = syna_usb_cmd (self->usb_device, enc, enc_len, wire_resp, sizeof (wire_resp), &wire_resp_len, cancellable, error);
    g_free (enc);

    if (!ok) {
        return FALSE;
    }

    if (syna_tls_decrypt_app_data (&self->tls, wire_resp, wire_resp_len, resp, max_resp_len, resp_len) != 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "TLS response decryption/HMAC failed");
        return FALSE;
    }

    uint16_t status = 0xffff;
    if (!req_len || !syna_response_ok (req[0], resp, *resp_len, &status)) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "Sensor rejected command 0x%02x: status 0x%04x (reply %zu bytes)",
                     req_len ? req[0] : 0, status, *resp_len);
        return FALSE;
    }
    return TRUE;
}

/* Helper to initialize OEM capture pipeline (0x1a pre-cleanup + 0x06 capture init blob) */
static gboolean
syna_prepare_capture_pipeline (FpiDeviceSyna00dc *self, GCancellable *cancellable, GError **error)
{
    /* 1. Pre-config cleanup (0x1a) */
    uint8_t clean_cmd = SYNA_CMD_CLEANUP;
    uint8_t clean_resp[64];
    size_t clean_resp_len = 0;
    self->capture_pipeline_ready = FALSE;
    if (!syna_tls_app_exchange (self, &clean_cmd, 1, clean_resp, sizeof (clean_resp),
                                &clean_resp_len, cancellable, error))
        return FALSE;

    /* 2. Capture initializer (0x06 + blob) */
    size_t init_req_len = 1 + self->oem.capture_init_blob_len;
    uint8_t *init_req = g_malloc (init_req_len);
    init_req[0] = SYNA_CMD_INIT;
    memcpy (init_req + 1, self->oem.capture_init_blob, self->oem.capture_init_blob_len);

    uint8_t init_resp[64];
    size_t init_resp_len = 0;
    gboolean init_ok = syna_tls_app_exchange (self, init_req, init_req_len, init_resp, sizeof (init_resp), &init_resp_len, cancellable, error);
    g_free (init_req);

    if (init_ok) {
        self->capture_pipeline_ready = TRUE;
    }
    return init_ok;
}


static char *
get_baseline_cache_path (const char *custom_dir)
{
    const char *dir = custom_dir;
    if (!dir) dir = getenv ("SYNA_TOD_DATA_DIR");
    if (!dir) dir = SYNA_DEFAULT_DATA_DIR;
    return g_build_filename (dir, "baseline_cache.bin", NULL);
}

static gboolean
syna_load_baseline_cache (const char *dir, float *out_baseline)
{
    const char *selected = dir;
    if (!selected) selected = getenv ("SYNA_TOD_DATA_DIR");
    if (!selected) selected = SYNA_DEFAULT_DATA_DIR;
    /* Do not fall back to another directory when the selected cache fails. */
    const char *names[] = { "baseline_cache.bin", "baseline.bin" };
    const size_t count = SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES;
    for (size_t i = 0; i < G_N_ELEMENTS (names); i++) {
        char *path = g_build_filename (selected, names[i], NULL);
        int fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        g_free (path);
        if (fd < 0) {
            if (errno == ENOENT) continue;
            return FALSE;
        }
        struct stat st;
        if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode) ||
            st.st_size != (off_t)(count * sizeof (float))) {
            close (fd);
            return FALSE;
        }
        FILE *f = fdopen (fd, "rb");
        if (!f) { close (fd); return FALSE; }
        float *values = g_new (float, count);
        gboolean valid = fread (values, sizeof (float), count, f) == count;
        if (fclose (f) != 0) valid = FALSE;
        for (size_t j = 0; valid && j < count; j++)
            if (!isfinite (values[j])) valid = FALSE;
        if (valid) memcpy (out_baseline, values, count * sizeof (float));
        g_free (values);
        return valid;
    }
    return FALSE;
}

static void
syna_save_baseline_cache (const char *dir, const float *baseline)
{
    const size_t count = SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES;
    for (size_t i = 0; i < count; i++)
        if (!isfinite (baseline[i])) return;
    char *path = get_baseline_cache_path (dir);
    char *tmp_path = g_strdup_printf ("%s.XXXXXX", path);
    int fd = g_mkstemp_full (tmp_path, O_RDWR | O_CLOEXEC, 0600);
    if (fd >= 0) {
        FILE *f = fdopen (fd, "wb");
        if (f) {
            gboolean valid = fwrite (baseline, sizeof (float), count, f) == count;
            if (fflush (f) != 0 || fsync (fd) != 0) valid = FALSE;
            if (fclose (f) != 0) valid = FALSE;
            if (!valid || rename (tmp_path, path) != 0) unlink (tmp_path);
        } else {
            close (fd);
            unlink (tmp_path);
        }
    }
    g_free (tmp_path);
    g_free (path);
}

static float
calc_rms (const float *frame, const float *baseline, size_t n)
{
    float sum_sq = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float diff = frame[i] - baseline[i];
        sum_sq += diff * diff;
    }
    return sqrtf (sum_sq / (float)n);
}

/* Helper to calibrate empty sensor baseline upon device open */
static gboolean
syna_calibrate_baseline (FpiDeviceSyna00dc *self, GCancellable *cancellable, GError **error)
{
    /* Start capture stream to measure baseline */
    uint8_t cap_resp[4096];
    size_t cap_resp_len = 0;
    if (!syna_tls_app_exchange (self, self->oem.capture_packet, self->oem.capture_packet_len,
                                cap_resp, sizeof (cap_resp), &cap_resp_len, cancellable, error)) {
        return FALSE;
    }

    if (!self->baseline) {
        self->baseline = g_malloc0 (SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES * sizeof (float));
    }

    if (!self->has_cached_baseline) {
        self->has_cached_baseline = syna_load_baseline_cache (NULL, self->cached_baseline);
    }

    uint8_t frame_buf[SYNA_FRAME_BYTES];
    size_t total_pixels = SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES;
    int target_frames = self->has_cached_baseline ? 4 : 10;
    int base_count = 0;
    float *accumulated = g_malloc0 (total_pixels * sizeof (float));
    float rms_sum = 0.0f;

    gint64 deadline = g_get_monotonic_time () + 15 * G_TIME_SPAN_SECOND;
    while (base_count < target_frames && g_get_monotonic_time () < deadline) {
        if (!syna_read_full_frame (self->usb_device, frame_buf, deadline, cancellable, error)) {
            break;
        }

        if (syna_validate_frame (frame_buf, NULL) == 0) {
            float dec[SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES];
            syna_decode_frame_unclipped (frame_buf, dec);

            if (self->has_cached_baseline) {
                float r = calc_rms (dec, self->cached_baseline, total_pixels);
                rms_sum += r;
            }

            for (size_t i = 0; i < total_pixels; i++) {
                accumulated[i] += dec[i];
            }
            base_count++;
        }
    }

    /* Send stop command 0x04 */
    uint8_t stop_cmd = SYNA_CMD_STOP;
    uint8_t stop_resp[64];
    size_t stop_resp_len = 0;
    GError *stop_error = NULL;
    if (!syna_tls_app_exchange (self, &stop_cmd, 1, stop_resp, sizeof (stop_resp),
                                &stop_resp_len, NULL, &stop_error)) {
        if (error && !*error) g_propagate_error (error, stop_error);
        else g_clear_error (&stop_error);
    }

    /* Drain bulk EP 130 */
    uint8_t drain_buf[16384];
    gsize drained = 0;
    for (int d = 0; d < 10; d++) {
        if (!g_usb_device_bulk_transfer (self->usb_device, SYNA_EP_DATA_IN,
                                         drain_buf, sizeof (drain_buf),
                                         &drained, 25, NULL, NULL) || drained == 0) {
            break;
        }
    }

    self->capture_pipeline_ready = FALSE;

    if (error && *error) {
        g_free (accumulated);
        return FALSE;
    }

    if (base_count < (self->has_cached_baseline ? 2 : 5)) {
        g_free (accumulated);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to read baseline frames from sensor");
        return FALSE;
    }

    self->initial_contact_present = FALSE;

    if (self->has_cached_baseline) {
        float avg_rms = rms_sum / (float)base_count;
        if (avg_rms > 18.0f) {
            /* User finger is already touching sensor! Retain clean cached baseline. */
            g_message ("Open: finger contact present at startup (RMS=%.1f > 18.0). Retaining clean cached baseline.", avg_rms);
            memcpy (self->baseline, self->cached_baseline, total_pixels * sizeof (float));
            self->has_baseline = TRUE;
            self->initial_contact_present = TRUE;
            g_free (accumulated);
            return TRUE;
        }

        /* Clean empty sensor: update baseline */
        for (size_t i = 0; i < total_pixels; i++) {
            self->baseline[i] = accumulated[i] / (float)base_count;
        }
        memcpy (self->cached_baseline, self->baseline, total_pixels * sizeof (float));
        syna_save_baseline_cache (NULL, self->baseline);
        self->has_baseline = TRUE;
        g_free (accumulated);
        return TRUE;
    }

    /* First-time calibration without cache: verify frame values are within valid ADC range */
    float avg_val = 0.0f;
    for (size_t i = 0; i < total_pixels; i++) avg_val += accumulated[i];
    avg_val /= (float)(base_count * total_pixels);
    if (avg_val < 50.0f || avg_val > 200.0f) {
        g_free (accumulated);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Sensor calibration returned abnormal signal levels");
        return FALSE;
    }

    for (size_t i = 0; i < total_pixels; i++) {
        self->baseline[i] = accumulated[i] / (float)base_count;
    }
    memcpy (self->cached_baseline, self->baseline, total_pixels * sizeof (float));
    self->has_cached_baseline = TRUE;
    syna_save_baseline_cache (NULL, self->baseline);
    self->has_baseline = TRUE;
    g_free (accumulated);
    return TRUE;
}

/* Background thread for OPEN */
static void
syna_open_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    (void)task_data;
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (source_object);
    GError *error = NULL;
    gboolean claimed = FALSE;

    self->usb_device = fpi_device_get_usb_device (FP_DEVICE (self));
    if (!self->usb_device) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No USB device associated");
        return;
    }

    /* Reset USB interface to ensure clean state */
    g_usb_device_reset (self->usb_device, NULL);

    if (!g_usb_device_claim_interface (self->usb_device, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, &error)) {
        g_task_return_error (task, error);
        return;
    }
    claimed = TRUE;

    /* 0. Hardware Identity Preflight (0x01 ROM Version + 0x75 Sensor Module ID) */
    {
        uint8_t ver_cmd = SYNA_CMD_VERSION;
        uint8_t ver_resp[64];
        size_t ver_resp_len = 0;
        if (!syna_usb_cmd (self->usb_device, &ver_cmd, 1, ver_resp, sizeof (ver_resp), &ver_resp_len, cancellable, &error)) {
            goto fail;
        }
        SynaVersionInfo ver_info;
        if (syna_parse_version_response (ver_resp, ver_resp_len, &ver_info) != 0) {
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                         "Unsupported Synaptics ROM version or build for 06cb:00dc");
            goto fail;
        }

        uint8_t id_cmd = SYNA_CMD_SENSOR_ID;
        uint8_t id_resp[32];
        size_t id_resp_len = 0;
        if (!syna_usb_cmd (self->usb_device, &id_cmd, 1, id_resp, sizeof (id_resp), &id_resp_len, cancellable, &error)) {
            goto fail;
        }
        SynaSensorIdInfo id_info;
        if (syna_parse_sensor_id_response (id_resp, id_resp_len, &id_info) != 0) {
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                         "Unsupported Synaptics sensor module ID for 06cb:00dc");
            goto fail;
        }
        g_message ("Hardware verified: ROM %u.%u build %u (product 0x%02x), sensor module 0x%04x:0x%04x",
                   ver_info.version_major, ver_info.version_minor, ver_info.build_number,
                   ver_info.product_id, id_info.major, id_info.minor);
    }

    /* Load OEM blobs and host credentials (or run first-boot pairing if allowed) */
    if (syna_oem_load (NULL, &self->oem) != 0) {
        const char *allow_pair = getenv ("SYNA_ALLOW_AUTO_PAIR");
        if (allow_pair && strcmp (allow_pair, "1") == 0 && !syna_oem_pairing_allowed (NULL)) {
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "OEM credentials exist or are inaccessible; refusing automatic re-pair");
            goto fail;
        }
        if (allow_pair && strcmp (allow_pair, "1") == 0) {
            g_message ("No existing credentials found; running first-boot host pairing (0x4f + 0x50)...");
            char *new_key_pem = NULL;
            uint8_t own_req[453];
            size_t own_req_len = 0;
            if (syna_pairing_generate_request (&new_key_pem, own_req, &own_req_len) != 0) {
                error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to generate 0x4f host pairing request");
                goto fail;
            }
            uint8_t own_resp[512];
            size_t own_resp_len = 0;
            if (!syna_usb_cmd (self->usb_device, own_req, own_req_len, own_resp, sizeof (own_resp), &own_resp_len, cancellable, &error)) {
                free (new_key_pem);
                goto fail;
            }
            uint8_t pub_cmd[13] = { SYNA_CMD_PUBLIC_MAT, 0 };
            uint8_t pub_resp[1024];
            size_t pub_resp_len = 0;
            if (!syna_usb_cmd (self->usb_device, pub_cmd, sizeof (pub_cmd), pub_resp, sizeof (pub_resp), &pub_resp_len, cancellable, &error)) {
                free (new_key_pem);
                goto fail;
            }
            uint8_t host_cert[184];
            size_t host_cert_len = 0;
            uint8_t server_pub[65];
            size_t server_pub_len = 0;
            if (syna_pairing_extract_credentials (own_resp, own_resp_len, pub_resp, pub_resp_len,
                                                  host_cert, &host_cert_len, server_pub, &server_pub_len) != 0 ||
                syna_oem_save_credentials (NULL, new_key_pem, host_cert, host_cert_len, server_pub, server_pub_len) != 0 ||
                syna_oem_load (NULL, &self->oem) != 0) {
                free (new_key_pem);
                error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "First-boot pairing credential extraction/save failed");
                goto fail;
            }
            free (new_key_pem);
            g_message ("First-boot pairing succeeded and saved to %s", SYNA_DEFAULT_DATA_DIR);
        } else {
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Failed to load Synaptics OEM pairing credentials");
            goto fail;
        }
    }

    /* Init TLS */
    if (syna_tls_init (&self->tls, self->oem.host_key_pem, self->oem.host_cert, self->oem.host_cert_len,
                       self->oem.server_ecdh_pub, self->oem.server_ecdh_pub_len) != 0) {
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to initialize TLS context");
        goto fail;
    }

    /* 1. ClientHello */
    uint8_t hello[128];
    size_t hello_len = 0;
    syna_tls_build_client_hello (&self->tls, hello, &hello_len);

    uint8_t s_hello[512];
    size_t s_hello_len = 0;
    if (!syna_usb_cmd (self->usb_device, hello, hello_len, s_hello, sizeof (s_hello), &s_hello_len, cancellable, &error)) {
        goto fail;
    }

    if (syna_tls_handle_server_hello (&self->tls, s_hello, s_hello_len) != 0) {
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "ServerHello parsing failed");
        goto fail;
    }

    /* 2. Flight 2 (KEX, Cert, Finished) */
    uint8_t flight2[1024];
    size_t flight2_len = 0;
    if (syna_tls_build_flight2 (&self->tls, flight2, &flight2_len) != 0) {
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to build TLS flight 2");
        goto fail;
    }

    uint8_t s_fin[512];
    size_t s_fin_len = 0;
    if (!syna_usb_cmd (self->usb_device, flight2, flight2_len, s_fin, sizeof (s_fin), &s_fin_len, cancellable, &error)) {
        goto fail;
    }

    if (syna_tls_handle_flight2_reply (&self->tls, s_fin, s_fin_len) != 0) {
        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "TLS Finished verification failed");
        goto fail;
    }

    /* 3. Prepare capture pipeline (0x1a cleanup + 0x06 init) */
    if (!syna_prepare_capture_pipeline (self, cancellable, &error)) {
        goto fail;
    }

    self->has_baseline = FALSE;
    /* 4. Load persistent baseline cache from disk, or calibrate if missing */
    if (!self->has_cached_baseline) {
        self->has_cached_baseline = syna_load_baseline_cache (NULL, self->cached_baseline);
    }
    if (self->has_cached_baseline) {
        if (!self->baseline) {
            self->baseline = g_malloc0 (SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES * sizeof (float));
        }
        memcpy (self->baseline, self->cached_baseline, SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES * sizeof (float));
        self->has_baseline = TRUE;
        g_message ("Open: loaded persistent baseline from cache (instant zero-wait startup)");
    } else {
        /* First-time calibration without cache */
        if (!syna_calibrate_baseline (self, cancellable, &error)) {
            goto fail;
        }
    }

    self->is_open = TRUE;
    g_task_return_boolean (task, TRUE);
    return;

fail:
    if (claimed) {
        g_usb_device_release_interface (self->usb_device, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, NULL);
    }
    syna_tls_free (&self->tls);
    syna_oem_free (&self->oem);
    g_task_return_error (task, error);
}

static void
on_open_completed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    FpDevice *device = FP_DEVICE (source_object);
    GError *error = NULL;
    g_task_propagate_boolean (G_TASK (res), &error);
    fpi_device_open_complete (device, error);
}

static void
syna_open (FpDevice *device)
{
    GTask *task = g_task_new (device, fpi_device_get_cancellable (device),
                              on_open_completed, NULL);
    g_task_run_in_thread (task, syna_open_thread);
    g_object_unref (task);
}

/* Background thread for CLOSE */
static void
syna_close_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    (void)task_data;
    (void)cancellable;
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (source_object);

    if (self->is_open && self->usb_device) {
        /* Send TLS close_notify */
        uint8_t close_buf[128];
        size_t close_len = 0;
        if (syna_tls_build_close_notify (&self->tls, close_buf, &close_len) == 0) {
            gsize transferred = 0;
            g_usb_device_bulk_transfer (self->usb_device, SYNA_EP_CMD_OUT, close_buf, close_len, &transferred, 1000, NULL, NULL);
        }

        g_usb_device_release_interface (self->usb_device, 0, G_USB_DEVICE_CLAIM_INTERFACE_NONE, NULL);
        self->is_open = FALSE;
    }

    syna_tls_free (&self->tls);
    syna_oem_free (&self->oem);

    g_task_return_boolean (task, TRUE);
}

static void
on_close_completed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    FpDevice *device = FP_DEVICE (source_object);
    GError *error = NULL;
    g_task_propagate_boolean (G_TASK (res), &error);
    fpi_device_close_complete (device, error);
}

static void
syna_close (FpDevice *device)
{
    GTask *task = g_task_new (device, fpi_device_get_cancellable (device),
                              on_close_completed, NULL);
    g_task_run_in_thread (task, syna_close_thread);
    g_object_unref (task);
}

/* Helper to read a complete 17024-byte frame from bulk EP */
static gboolean
syna_read_full_frame (GUsbDevice *usb_device, uint8_t *frame_buf, gint64 deadline, GCancellable *cancellable, GError **error)
{
    size_t total = 0;
    while (total < SYNA_FRAME_BYTES) {
        if (g_cancellable_is_cancelled (cancellable)) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Capture cancelled");
            return FALSE;
        }
        guint timeout_ms = syna_remaining_timeout (deadline, g_get_monotonic_time ());
        if (!timeout_ms) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Frame deadline exceeded");
            return FALSE;
        }
        gsize chunk = 0;
        GError *local_err = NULL;
        if (!g_usb_device_bulk_transfer (usb_device, SYNA_EP_DATA_IN,
                                         frame_buf + total, SYNA_FRAME_BYTES - total,
                                         &chunk, timeout_ms, cancellable, &local_err)) {
            if (local_err) {
                g_propagate_error (error, local_err);
            }
            return FALSE;
        }
        if (chunk == 0) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Zero bytes read from bulk stream");
            return FALSE;
        }
        total += chunk;
    }
    return TRUE;
}

#define MAX_CONTACT_FRAMES 12
#define MIN_CONTACT_FRAMES 6

/* Helper to capture a single finger sample from USB bulk stream */
static float *
syna_capture_finger_sample (FpiDeviceSyna00dc *self,
                            gboolean require_finger_lift,
                            gboolean more_stages_needed,
                            GCancellable *cancellable,
                            GError **error)
{
    /* 1. Ensure clean baseline is available */
    if (!self->has_baseline) {
        if (!syna_calibrate_baseline (self, cancellable, error)) {
            return NULL;
        }
    }

    /* 2. Ensure capture pipeline is initialized with OEM initializer */
    if (!self->capture_pipeline_ready) {
        if (!syna_prepare_capture_pipeline (self, cancellable, error)) {
            return NULL;
        }
    }

    /* 3. Send capture command 0x02 + packet */
    uint8_t cap_resp[4096];
    size_t cap_resp_len = 0;
    if (!syna_tls_app_exchange (self, self->oem.capture_packet, self->oem.capture_packet_len,
                                cap_resp, sizeof (cap_resp), &cap_resp_len, cancellable, error)) {
        return NULL;
    }

    uint8_t frame_buf[SYNA_FRAME_BYTES];
    float collected_frames[MAX_CONTACT_FRAMES][SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES];
    int valid_contact_frames = 0;
    int transient_seen = 0;
    gboolean sensor_cleared = FALSE;
    gboolean finger_detected = FALSE;
    gboolean waiting_for_lift = FALSE;
    int empty_consecutive_frames = 0;

    if (self->initial_contact_present && !require_finger_lift) {
        sensor_cleared = TRUE;
        finger_detected = TRUE;
        self->initial_contact_present = FALSE;
        syna_report_status (self, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NONE);
    } else {
        self->initial_contact_present = FALSE;
    }

    gint64 deadline = g_get_monotonic_time () + 60 * G_TIME_SPAN_SECOND;
    float min_rms = INFINITY, max_rms = 0.0f;

    int target_contact_frames = 6;
    int min_contact_frames = 5;

    while (valid_contact_frames < target_contact_frames) {
        if (g_get_monotonic_time () >= deadline) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Capture deadline exceeded (60 seconds)");
            break;
        }
        if (g_cancellable_is_cancelled (cancellable)) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Capture cancelled");
            break;
        }

        if (!syna_read_full_frame (self->usb_device, frame_buf, deadline, cancellable, error)) {
            break;
        }

        if (syna_validate_frame (frame_buf, NULL) == 0) {
            float dec[SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES];
            syna_decode_frame_unclipped (frame_buf, dec);

            float rms = calc_rms (dec, self->baseline, G_N_ELEMENTS (dec));
            min_rms = fminf (min_rms, rms);
            max_rms = fmaxf (max_rms, rms);

            /* Phase 1: Check initial sensor state (wait for finger lift if required between stages) */
            if (!sensor_cleared) {
                if (!require_finger_lift && rms > 35.0f && self->has_baseline) {
                    /* Finger already present on sensor with valid persistent baseline! */
                    g_message ("Capture: finger already present at startup (RMS=%.1f). Bypassing lift wait.", rms);
                    sensor_cleared = TRUE;
                    finger_detected = TRUE;
                    syna_report_status (self, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NONE);
                    for (size_t i = 0; i < G_N_ELEMENTS (dec); i++) {
                        collected_frames[valid_contact_frames][i] = dec[i] - self->baseline[i];
                    }
                    valid_contact_frames++;
                    continue;
                } else if (rms > 28.0f) {
                    empty_consecutive_frames = 0;
                    if (!waiting_for_lift) {
                        waiting_for_lift = TRUE;
                    }
                    continue;
                } else {
                    empty_consecutive_frames++;
                    if (empty_consecutive_frames >= (require_finger_lift ? 2 : 1)) {
                        sensor_cleared = TRUE;
                        syna_report_status (self, FP_FINGER_STATUS_NEEDED, FP_FINGER_STATUS_PRESENT);
                    }
                    continue;
                }
            }

            /* Phase 2: Touch detection */
            if (rms > 35.0f) {
                if (!finger_detected) {
                    finger_detected = TRUE;
                    syna_report_status (self, FP_FINGER_STATUS_PRESENT, FP_FINGER_STATUS_NEEDED);
                }

                transient_seen++;
                if (transient_seen <= 1) {
                    /* Discard 1 initial transient frame upon contact */
                    continue;
                }

                /* Store contact frame minus baseline */
                for (size_t i = 0; i < G_N_ELEMENTS (dec); i++) {
                    collected_frames[valid_contact_frames][i] = dec[i] - self->baseline[i];
                }
                valid_contact_frames++;
            } else if (finger_detected) {
                /* Finger lifted before target_contact_frames */
                break;
            }
        }
    }

    /* Update status at end of capture */
    if (!more_stages_needed) {
        if (finger_detected) {
            syna_report_status (self, FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_PRESENT);
        } else if (sensor_cleared) {
            syna_report_status (self, FP_FINGER_STATUS_NONE, FP_FINGER_STATUS_NEEDED);
        }
    }

    g_message ("Capture summary: clear=%d valid_frames=%d RMS=%.1f..%.1f",
               sensor_cleared, valid_contact_frames, min_rms, max_rms);

    /* Send stop command 0x04 */
    uint8_t stop_cmd = SYNA_CMD_STOP;
    uint8_t stop_resp[64];
    size_t stop_resp_len = 0;
    GError *stop_error = NULL;
    if (!syna_tls_app_exchange (self, &stop_cmd, 1, stop_resp, sizeof (stop_resp),
                                &stop_resp_len, NULL, &stop_error)) {
        if (error && !*error) g_propagate_error (error, stop_error);
        else g_clear_error (&stop_error);
    }

    /* Drain any leftover bulk packets on EP 130 */
    uint8_t drain_buf[16384];
    gsize drained = 0;
    for (int d = 0; d < 10; d++) {
        if (!g_usb_device_bulk_transfer (self->usb_device, SYNA_EP_DATA_IN,
                                         drain_buf, sizeof (drain_buf),
                                         &drained, 25, NULL, NULL) || drained == 0) {
            break;
        }
    }

    self->capture_pipeline_ready = FALSE;

    if ((error && *error) || valid_contact_frames < min_contact_frames) {
        if (error && !*error) {
            if (finger_detected) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT, "Finger lifted too soon");
            } else {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "No sustained finger contact detected");
            }
        }
        return NULL;
    }

    /* Motion-blur-free alignment and averaging */
    float quality = 0.0f;
    float *contact_avg = syna_align_frames ((const float *)collected_frames, (size_t)valid_contact_frames, &quality);
    if (!contact_avg) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Frame alignment failed");
        return NULL;
    }

    if (quality < 50.0f) {
        g_message ("Capture rejected: low contrast energy / flat ridge profile (%.1f)", quality);
        g_free (contact_avg);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Scan quality too low");
        return NULL;
    }

    float *filtered = g_malloc (SYNA_OUTPUT_ROWS * SYNA_SAMPLE_BYTES * sizeof (float));
    if (syna_bandpass_filter (contact_avg, filtered) != 0) {
        g_free (contact_avg);
        g_free (filtered);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Bandpass filtering failed");
        return NULL;
    }
    g_free (contact_avg);

    return filtered;
}

/* Background thread for ENROLL */
static void
syna_enroll_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    (void)task_data;
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (source_object);
    GError *error = NULL;

    GPtrArray *samples = g_ptr_array_new_with_free_func (g_free);
    FpPrint *print = FP_PRINT (task_data);

    guint stage = 0;
    gboolean require_lift = FALSE;
    while (stage < SYNA_ENROLL_STAGES) {
        if (g_cancellable_is_cancelled (cancellable)) {
            g_ptr_array_unref (samples);
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Enrollment cancelled");
            return;
        }

        gboolean more_stages = (stage + 1 < SYNA_ENROLL_STAGES);
        float *sample = syna_capture_finger_sample (self, require_lift, more_stages, cancellable, &error);
        if (!sample) {
            if (error && (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
                          g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))) {
                g_ptr_array_unref (samples);
                g_task_return_error (task, error);
                return;
            }
            FpDeviceRetry retry_code = (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT))
                                       ? FP_DEVICE_RETRY_TOO_SHORT
                                       : FP_DEVICE_RETRY_REMOVE_FINGER;
            g_clear_error (&error);
            syna_report_enroll_retry (self, stage, retry_code);
            require_lift = TRUE;
            continue;
        }

        g_ptr_array_add (samples, sample);
        stage++;
        require_lift = TRUE;
        syna_report_stage (self, stage);
    }

    GVariant *encoded = g_variant_ref_sink (syna_template_encode (samples));
    g_ptr_array_unref (samples);
    (void)print;
    g_task_return_pointer (task, encoded, (GDestroyNotify)g_variant_unref);
}

static void
on_enroll_completed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    FpDevice *device = FP_DEVICE (source_object);
    GError *error = NULL;
    GVariant *encoded = g_task_propagate_pointer (G_TASK (res), &error);
    FpPrint *print = g_task_get_task_data (G_TASK (res));
    if (encoded) {
        fpi_print_set_type (print, FPI_PRINT_RAW);
        g_object_set (print, "fpi-data", encoded, NULL);
        g_variant_unref (encoded);
    }
    fpi_device_enroll_complete (device, encoded ? g_object_ref (print) : NULL, error);
}

static void
syna_enroll (FpDevice *device)
{
    GTask *task = g_task_new (device, fpi_device_get_cancellable (device),
                              on_enroll_completed, NULL);
    FpPrint *print = NULL;
    fpi_device_get_enroll_data (device, &print);
    g_task_set_task_data (task, g_object_ref (print), g_object_unref);
    g_task_run_in_thread (task, syna_enroll_thread);
    g_object_unref (task);
}

/* Background thread for VERIFY */
static void
syna_verify_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    (void)task_data;
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (source_object);
    GError *error = NULL;

    GPtrArray *samples = task_data;
    if (!samples) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "Invalid or unsupported enrolled template; enroll again");
        return;
    }

    float *live_sample = syna_capture_finger_sample (self, FALSE, FALSE, cancellable, &error);
    if (!live_sample) {
        g_task_return_error (task, error);
        return;
    }

    SynaMatcherContext *matcher_ctx = syna_matcher_context_new ();
    if (matcher_ctx) {
        syna_matcher_set_probe (matcher_ctx, live_sample);
    }

    SynaMatchResult match_res = { .score = -1.0f };
    for (guint i = 0; i < samples->len; i++) {
        SynaMatchResult candidate;
        int rc;
        if (matcher_ctx) {
            rc = syna_matcher_match_template (matcher_ctx, samples->pdata[i], 0.45f, &candidate);
        } else {
            rc = syna_match_images (samples->pdata[i], live_sample, 0.45f, &candidate);
        }
        if (rc != 0) {
            if (matcher_ctx) syna_matcher_context_free (matcher_ctx);
            g_free (live_sample);
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Matcher failed");
            return;
        }
        if (candidate.score > match_res.score) match_res = candidate;
        if (candidate.score >= 0.70f) {
            g_message ("Synaptics verify: early match exit on template %u (score: %.3f >= 0.70)",
                       i, candidate.score);
            break;
        }
    }
    if (matcher_ctx) syna_matcher_context_free (matcher_ctx);
    g_free (live_sample);

    g_message ("Synaptics verify score: %.3f (threshold: %.2f, rotation: %.1f deg, overlap: %.1f%%)",
               match_res.score, (float)SYNA_MATCH_THRESHOLD, match_res.angle, match_res.overlap * 100.0f);

    SynaMatchResult *result = g_new (SynaMatchResult, 1);
    *result = match_res;
    g_task_return_pointer (task, result, g_free);
}

static void
on_verify_completed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    FpDevice *device = FP_DEVICE (source_object);
    GError *error = NULL;
    SynaMatchResult *result = g_task_propagate_pointer (G_TASK (res), &error);
    if (result) {
        FpPrint *print = NULL;
        fpi_device_get_verify_data (device, &print);
        gboolean matched = result->score >= SYNA_MATCH_THRESHOLD;
        fpi_device_verify_report (device, matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                  matched ? print : NULL, NULL);
        g_free (result);
    }
    fpi_device_verify_complete (device, error);
}

static void
syna_verify (FpDevice *device)
{
    GTask *task = g_task_new (device, fpi_device_get_cancellable (device),
                              on_verify_completed, NULL);
    FpPrint *print = NULL;
    GVariant *data = NULL;
    fpi_device_get_verify_data (device, &print);
    g_object_get (print, "fpi-data", &data, NULL);
    GPtrArray *samples = syna_template_decode (data);
    if (data) g_variant_unref (data);
    g_task_set_task_data (task, samples, samples ? (GDestroyNotify)g_ptr_array_unref : NULL);
    g_task_run_in_thread (task, syna_verify_thread);
    g_object_unref (task);
}

/* Multi-finger IDENTIFY (1-to-N across all enrolled fingers) */
typedef struct {
    FpPrint *print;
    GPtrArray *samples;
} SynaIdentifyEntry;

static void
syna_identify_entry_free (gpointer data)
{
    SynaIdentifyEntry *entry = data;
    if (!entry) return;
    if (entry->print) g_object_unref (entry->print);
    if (entry->samples) g_ptr_array_unref (entry->samples);
    g_free (entry);
}

static void
syna_identify_thread (GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (source_object);
    GPtrArray *entries = task_data;
    GError *error = NULL;

    if (!entries || entries->len == 0) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "No valid enrolled templates available for identification");
        return;
    }

    float *live_sample = syna_capture_finger_sample (self, FALSE, FALSE, cancellable, &error);
    if (!live_sample) {
        g_task_return_error (task, error);
        return;
    }

    SynaMatcherContext *matcher_ctx = syna_matcher_context_new ();
    if (matcher_ctx) {
        syna_matcher_set_probe (matcher_ctx, live_sample);
    }

    SynaMatchResult best_res = { .score = -1.0f };
    FpPrint *best_print = NULL;

    for (guint f = 0; f < entries->len; f++) {
        SynaIdentifyEntry *entry = entries->pdata[f];
        for (guint i = 0; i < entry->samples->len; i++) {
            SynaMatchResult candidate;
            int rc;
            if (matcher_ctx) {
                rc = syna_matcher_match_template (matcher_ctx, entry->samples->pdata[i], 0.45f, &candidate);
            } else {
                rc = syna_match_images (entry->samples->pdata[i], live_sample, 0.45f, &candidate);
            }
            if (rc != 0) continue;
            if (candidate.score > best_res.score) {
                best_res = candidate;
                best_print = entry->print;
            }
            if (candidate.score >= 0.72f) {
                g_message ("Synaptics identify: early match exit on finger %u template %u (score: %.3f)",
                           f, i, candidate.score);
                goto identify_done;
            }
        }
    }

identify_done:
    if (matcher_ctx) syna_matcher_context_free (matcher_ctx);
    g_free (live_sample);

    g_message ("Synaptics identify best score: %.3f (threshold: %.2f, rotation: %.1f deg, overlap: %.1f%%)",
               best_res.score, (float)SYNA_MATCH_THRESHOLD, best_res.angle, best_res.overlap * 100.0f);

    if (best_print && best_res.score >= SYNA_MATCH_THRESHOLD) {
        g_task_return_pointer (task, g_object_ref (best_print), g_object_unref);
    } else {
        g_task_return_pointer (task, NULL, NULL);
    }
}

static void
on_identify_completed (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    (void)user_data;
    FpDevice *device = FP_DEVICE (source_object);
    GError *error = NULL;
    FpPrint *matched_print = g_task_propagate_pointer (G_TASK (res), &error);
    if (!error) {
        fpi_device_identify_report (device, matched_print, matched_print, NULL);
    }
    if (matched_print) {
        g_object_unref (matched_print);
    }
    fpi_device_identify_complete (device, error);
}

static void
syna_identify (FpDevice *device)
{
    GTask *task = g_task_new (device, fpi_device_get_cancellable (device),
                              on_identify_completed, NULL);
    GPtrArray *prints = NULL;
    fpi_device_get_identify_data (device, &prints);

    GPtrArray *entries = g_ptr_array_new_with_free_func (syna_identify_entry_free);
    if (prints) {
        for (guint i = 0; i < prints->len; i++) {
            FpPrint *print = prints->pdata[i];
            GVariant *data = NULL;
            g_object_get (print, "fpi-data", &data, NULL);
            GPtrArray *samples = syna_template_decode (data);
            if (data) g_variant_unref (data);
            if (samples) {
                SynaIdentifyEntry *entry = g_new0 (SynaIdentifyEntry, 1);
                entry->print = g_object_ref (print);
                entry->samples = samples;
                g_ptr_array_add (entries, entry);
            }
        }
    }

    g_task_set_task_data (task, entries, (GDestroyNotify)g_ptr_array_unref);
    g_task_run_in_thread (task, syna_identify_thread);
    g_object_unref (task);
}

static void
syna_cancel (FpDevice *device)
{
    FpiDeviceSyna00dc *self = FPI_DEVICE_SYNA_00DC (device);
    self->is_cancelling = TRUE;
}

static void
fpi_device_syna_00dc_class_init (FpiDeviceSyna00dcClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

    object_class->finalize = fpi_device_syna_00dc_finalize;

    dev_class->id = "syna_00dc_tod";
    dev_class->full_name = "Synaptics 06cb:00dc (Acer Aspire A514-54)";
    dev_class->type = FP_DEVICE_TYPE_USB;
    dev_class->scan_type = FP_SCAN_TYPE_PRESS;
    dev_class->nr_enroll_stages = SYNA_ENROLL_STAGES;
    dev_class->id_table = syna_ids;

    dev_class->open = syna_open;
    dev_class->close = syna_close;
    dev_class->enroll = syna_enroll;
    dev_class->verify = syna_verify;
    dev_class->identify = syna_identify;
    dev_class->cancel = syna_cancel;

    fpi_device_class_auto_initialize_features (dev_class);
}
