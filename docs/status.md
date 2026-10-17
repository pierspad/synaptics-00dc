# Status and upstream work

The production module still uses GTask worker threads for USB I/O. The separate
FpiSsm engine is experimental and is not the runtime backend. Offline corrections
to that engine are not hardware validation.

Known scope: USB 06cb:00dc, ROM 6.20 build154, module0190:00a2. Historical local
enrollment/verification observations are in REA-FS; no population FAR/FRR or spoof
resistance is established. Six-frame capture, the custom FFT matcher and cached
baseline require independent testing across touches, sessions and devices.

Before upstream submission:

1. Finish asynchronous open/calibration/close and operation orchestration; test
   cancellation/reopen, malformed responses, unplug and suspend/resume.
2. Demonstrate first pairing and baseline calibration on a fresh device without
   importing the maintainer's credentials or baseline.
3. Compare FpImageDevice/NBIS with the custom matcher on held-out local data.
   Do not select thresholds on the final evaluation set.
4. Agree persistent state and table provenance with libfprint maintainers, then
   add sanitized/synthetic USB replay coverage and compile in upstream itself.

Default offline burst/score fixtures are synthetic. Private historical fixtures
can be selected explicitly using SYNA_TEST_CAPTURE7 and SYNA_TEST_CAPTURE8;
they use fixed offsets from the original REA-FS trials, not arbitrary captures.
An explicitly missing input is an error, never a silent synthetic fallback.

## Release gates

| Milestone | Evidence required | Current state |
|---|---|---|
| Another installation | Exact hardware/software revisions and redacted report | External tester wanted |
| Fresh pairing | Device-local credentials and baseline, no maintainer exports | Not validated on a fresh external reader |
| Recovery | Cancel/reopen, unplug and suspend/resume logs | New-build hardware checks pending |
| Recognition quality | Held-out genuine/impostor attempts and fixed thresholds | Population reliability not established |
| Native upstream driver | Hardware-tested async implementation and maintainer review | Local compile prototype passing unit tests; full SSM engine wired under `SYNA_USE_SSM=1` |

Keep TOD for the current experimental distribution while validating these gates.
Removing it means integrating the driver into native libfprint, not deleting a
package dependency or changing the plugin directory. The default runtime backend
remains threaded (`GTask`) for stability; `SYNA_USE_SSM=1` allows opt-in testing
of the non-blocking state machine engine.

Specific engineering follow-ups include separating credentials/calibration state
by physical device and reviewing legacy Polkit rules separately from installation.
The generic installer message suggesting setup is for first-time configuration,
not a required update step.

## v0.1.2 — offline hardening

Version v0.1.2 adds custom setup build directories, stricter baseline
file validation and selected-directory lookup, unique private temporary files,
and refusal to re-pair automatically after existing credential/configuration errors.
Synthetic regression tests cover these cases; this is not hardware validation.

## v0.2.0 and current development — multi-finger identify, duplicate detection, and full SSM coverage

Version v0.2.0 and recent updates deliver key architectural enhancements across matching, system integration, and asynchronous readiness:

1. **Multi-Finger `identify` (1:N) & Unified Matcher:**
   - Unified probe-against-gallery matcher (`syna_match_probe_against_gallery`) shared across `verify` (1:1) and `identify` (1:N).
   - Desktop lockscreens, PAM (`pam_fprintd`), and `fprintd-verify` can authenticate any enrolled finger in a single touch.

2. **`fprintd` Duplicate-Detection Pre-Enroll Scan & Empty-Gallery Compliance:**
   - Addressed the `fprintd` stage-injection gotcha: `fprintd` (`src/device.c`) checks `FP_DEVICE_FEATURE_DUPLICATES_CHECK` when `dev_class->identify` is present. Without this flag, `fprintd` inflates `num-enroll-stages` on D-Bus by `+1` and silently executes `fp_device_identify(device, all_prints)` before calling `enroll()`.
   - Empty-gallery fix: If no fingers are enrolled (`all_prints->len == 0`), `syna_identify_thread` returns `NULL` without error (`error = NULL`), complying with upstream `libfprint` unit test `test_driver_identify_empty_gallery` and preventing `fprintd` from aborting `EnrollStart` with `enroll-unknown-error`.
   - Duplicate-check suppression: Explicitly set `dev_class->features |= FP_DEVICE_FEATURE_DUPLICATES_CHECK;` in class initialization, eliminating the redundant pre-enroll scan and keeping D-Bus `num-enroll-stages` equal to `nr_enroll_stages` (8 stages).

3. **OpenSSL 3.0 `EVP_MAC` & `EVP_PKEY` Modernization:**
   - Replaced deprecated OpenSSL 1.1 calls (`HMAC`, `EC_KEY`, `o2i_ECPublicKey`, `ECDSA_sign`) with `EVP_MAC_fetch(NULL, "HMAC", NULL)`, `EVP_Q_mac`, `EVP_PKEY_fromdata`, `EVP_PKEY_sign`, and `EVP_PKEY_Q_keygen`. Builds with zero `-Wdeprecated-declarations` warnings.

4. **Per-Device Baseline Caching:**
   - Moved `cached_baseline` array and `has_cached_baseline` flag from process-global static state into `FpiDeviceSyna00dc`, eliminating the global baseline mutex and supporting multiple device instances safely.

5. **Complete `FpiSsm` Engine Coverage (`SYNA_USE_SSM=1`):**
   - Implemented `syna_ssm_open_new` (19 states: claim, ROM probe `0x01`, module query `0x75`, pairing `0x4f`/`0x50`, TLS handshake flights, `0x1a` cleanup, `0x06` capture init, release error handler).
   - Implemented `syna_ssm_capture_new` (12 states: pipeline verification, `0x02` start, streaming bulk transfers on EP `0x82`, RMS contact/lift detection, `0x04` stop, frame alignment/filtering).
   - Implemented `syna_ssm_close_new` (5 states: cleanup `0x1a`, TLS `close_notify`, USB interface release), guaranteed uncancellable (`cancellable == NULL`).
   - Wired into `syna_open`, `syna_enroll`, `syna_verify`, `syna_identify`, and `syna_close`. Gated dynamically by `SYNA_USE_SSM=1` so users retain stable threaded execution while developers and CI can exercise the non-blocking engine directly.
