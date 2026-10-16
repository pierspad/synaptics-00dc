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
