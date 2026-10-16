#include <glib.h>
#include <fprint.h>
#include <fpi-ssm.h>
#include <fpi-device.h>
#include "syna_ssm.h"

#define FPI_TYPE_MOCK_DEV (mock_dev_get_type ())
G_DECLARE_FINAL_TYPE (MockDev, mock_dev, MOCK, DEV, FpDevice)
struct _MockDev {
    FpDevice parent;
};
G_DEFINE_TYPE (MockDev, mock_dev, FP_TYPE_DEVICE)
static void mock_dev_init (MockDev *self) { (void)self; }
static void mock_dev_class_init (MockDevClass *klass) {
    FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
    dev_class->id = "mock";
    dev_class->features = FP_DEVICE_FEATURE_CAPTURE;
}

static void
test_close_ssm_enum_count (void)
{
    g_assert_cmpint (SYNA_CLOSE_NUM_STATES, ==, 5);
    g_assert_cmpint (SYNA_CLOSE_STEP_CLEANUP_TX, ==, 0);
    g_assert_cmpint (SYNA_CLOSE_STEP_CLEANUP_RX, ==, 1);
    g_assert_cmpint (SYNA_CLOSE_STEP_TLS_ALERT_TX, ==, 2);
    g_assert_cmpint (SYNA_CLOSE_STEP_TLS_ALERT_RX, ==, 3);
    g_assert_cmpint (SYNA_CLOSE_STEP_RELEASE_USB, ==, 4);
}

static void
test_close_ssm_uncancellable (void)
{
    /* Close teardown must never be interrupted by cancellation */
    g_assert_true (syna_ssm_close_cancellable_valid (NULL));

    GCancellable *cancellable = g_cancellable_new ();
    g_assert_false (syna_ssm_close_cancellable_valid (cancellable));
    g_cancellable_cancel (cancellable);
    g_assert_false (syna_ssm_close_cancellable_valid (cancellable));
    g_object_unref (cancellable);
}

typedef struct {
    int visited_states[SYNA_CLOSE_NUM_STATES + 2];
    int visit_count;
    gboolean completed;
} MockRunnerData;

static void
mock_close_runner (FpiSsm *ssm, FpDevice *dev, gpointer user_data)
{
    (void)dev;
    MockRunnerData *data = user_data;
    int state = fpi_ssm_get_cur_state (ssm);
    g_assert_cmpint (data->visit_count, <, (int)G_N_ELEMENTS (data->visited_states));
    data->visited_states[data->visit_count++] = state;

    if (state == SYNA_CLOSE_STEP_RELEASE_USB) {
        data->completed = TRUE;
        fpi_ssm_mark_completed (ssm);
    } else {
        fpi_ssm_next_state (ssm);
    }
}

static void
close_ssm_completed_cb (FpiSsm *ssm, FpDevice *dev, GError *error)
{
    (void)ssm;
    (void)dev;
    g_assert_no_error (error);
}

static void
test_close_ssm_progression (void)
{
    FpDevice *dev = g_object_new (FPI_TYPE_MOCK_DEV, NULL);
    g_assert_nonnull (dev);

    MockRunnerData data = { 0 };
    FpiSsm *ssm = syna_ssm_close_new (dev, mock_close_runner, &data);
    g_assert_nonnull (ssm);

    fpi_ssm_start (ssm, close_ssm_completed_cb);

    g_assert_cmpint (data.visit_count, ==, 5);
    g_assert_true (data.completed);
    for (int i = 0; i < 5; i++) {
        g_assert_cmpint (data.visited_states[i], ==, i);
    }

    g_object_unref (dev);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/ssm/close/enum-count", test_close_ssm_enum_count);
    g_test_add_func ("/ssm/close/uncancellable", test_close_ssm_uncancellable);
    g_test_add_func ("/ssm/close/progression", test_close_ssm_progression);
    return g_test_run ();
}
