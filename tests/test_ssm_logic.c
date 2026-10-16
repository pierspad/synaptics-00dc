/* Include the implementation so these tests exercise the policy helpers that
 * the asynchronous SSM handlers call, rather than reimplementing their rules. */
#include "../src/syna_ssm.c"

static void
test_response_status_policy (void)
{
    const uint8_t ok[] = { 0x00, 0x00 };
    const uint8_t cleanup_warning[] = { 0x91, 0x04 };
    const uint8_t rejected[] = { 0x02, 0x00 };

    g_assert_true (syna_ssm_plain_response_ok (SYNA_CMD_INIT, ok, sizeof (ok)));
    g_assert_false (syna_ssm_plain_response_ok (SYNA_CMD_INIT, cleanup_warning,
                                                sizeof (cleanup_warning)));
    g_assert_true (syna_ssm_plain_response_ok (SYNA_CMD_CLEANUP, cleanup_warning,
                                               sizeof (cleanup_warning)));
    g_assert_false (syna_ssm_plain_response_ok (SYNA_CMD_STOP, rejected, sizeof (rejected)));
    g_assert_false (syna_ssm_plain_response_ok (SYNA_CMD_STOP, ok, 1));
    g_assert_false (syna_ssm_plain_response_ok (SYNA_CMD_STOP, NULL, 0));
}

static void
test_capture_deadline_boundary (void)
{
    const gint64 deadline = 45000000;

    g_assert_false (syna_ssm_deadline_expired (0, deadline));
    g_assert_false (syna_ssm_deadline_expired (deadline, deadline - 1));
    g_assert_true (syna_ssm_deadline_expired (deadline, deadline));
    g_assert_true (syna_ssm_deadline_expired (deadline, deadline + 1));
    g_assert_cmpuint (syna_ssm_deadline_timeout_ms (0, deadline, 15000), ==, 15000);
    g_assert_cmpuint (syna_ssm_deadline_timeout_ms (deadline, deadline - 15000000, 15000), ==, 15000);
    g_assert_cmpuint (syna_ssm_deadline_timeout_ms (deadline, deadline - 2500000, 15000), ==, 2500);
    g_assert_cmpuint (syna_ssm_deadline_timeout_ms (deadline, deadline, 15000), ==, 0);
}

static void
test_capture_start_response_is_one_shot (void)
{
    gboolean response_validated = FALSE;

    g_assert_true (syna_ssm_capture_start_response_needed (response_validated));
    response_validated = TRUE;
    g_assert_false (syna_ssm_capture_start_response_needed (response_validated));
}

static void
test_open_failure_cleanup_policy (void)
{
    g_assert_false (syna_ssm_should_release_open_interface (FALSE, FALSE));
    g_assert_true (syna_ssm_should_release_open_interface (TRUE, FALSE));
    g_assert_false (syna_ssm_should_release_open_interface (TRUE, TRUE));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/ssm/response-status-policy", test_response_status_policy);
    g_test_add_func ("/ssm/capture-deadline-boundary", test_capture_deadline_boundary);
    g_test_add_func ("/ssm/capture-start-response-one-shot", test_capture_start_response_is_one_shot);
    g_test_add_func ("/ssm/open-failure-cleanup-policy", test_open_failure_cleanup_policy);
    return g_test_run ();
}
