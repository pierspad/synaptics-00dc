# Try the driver and report a result

This is an experimental driver for **Synaptics USB 06cb:00dc**. The development
machine is Acer Aspire A514-54, ROM 6.20 build 154, module 0190:00a2. Matching USB
IDs alone do not establish support for every hardware revision.

## Before installing

1. Confirm the USB ID with `lsusb`. Stop for a different ID; do not edit the ID table
   to force binding. Keep your normal password login available.
2. Read the [installation instructions](../README.md#install),
   [current limits](status.md) and [backup/recovery notes](installation.md).
3. Note your distribution, kernel, TOD package and driver tag/commit. Distinguish
   a fresh reader from one already paired under Linux or Windows.
4. Follow the README for your distribution as a normal user. Package operations
   and installation may ask for administrative authentication. Do not run two
   fingerprint experiments concurrently.

On a new installation the explicit setup step may pair and calibrate the device,
then enroll a finger. Leave the sensor empty during calibration and follow prompts.
Setup stops after a failure; report the first error instead of repeatedly pairing.
On an existing working installation update the module only; do not run setup again.

## Hardware checks

If working with a coding agent, use the REA-FS panel and its workflow below. Do not
let an agent run unattended enrollment/capture commands in its terminal.

Start with existing enrollment where available. Verify a registered finger, then
check cancellation and reopening with a bounded panel plan (cancel within 5 seconds,
30-second cancellation-run limit, subsequent verification within 60 seconds).
Record whether the device can be reused without restarting fprintd. A cancelled
client alone does not prove sensor cleanup. Stop on an unexpected error.

Later, separately test enrollment, another registered finger, a non-enrolled finger,
and suspend/resume. A single rejection is not a false-accept-rate measurement.
Do not delete existing prints or alter login policy merely to complete a checklist.

## Share a useful report (Successes, Bugs & Fixes)

Whether the driver **worked out of the box** or failed at a specific step, please open
an Issue (using the **Installation / hardware report** template) or a Pull Request on
`https://github.com/pierspad/synaptics-00dc`:

- **Working installations matter too:** If setup, enrollment, and verification succeeded on
  your laptop, open an Issue to report your laptop model, distro/kernel, and observed sensor
  revision (`ROM X.Y build Z, module AAAA:BBBB`) so we can add your device to the verified
  hardware list.
- **Bugs or LLM-assisted fixes:** If you or your coding agent found a problem or developed a
  solution, open an Issue or PR with a detailed root-cause explanation, the focused diff, and
  offline test results.
- Include:
  - Laptop model and USB ID, plus ROM/module revision if observed (omit personal serial numbers).
  - Distribution/kernel, TOD package/version, and driver tag/commit.
  - Fresh setup or update; exact failing command and a short redacted error excerpt.
  - Separate outcomes for build, install, pairing, enrollment, verification, and cleanup.

Never upload `/var/lib/fprint`, pairing keys, templates, fingerprint images, raw USB
captures, or an entire private session/log directory. Review any excerpt manually.
A successful build and an installed module are not evidence of recognition quality.

## Working with an LLM

An LLM can help diagnose package names, build failures and a reproducible parser bug.
Ask it to explain the cause, make one focused patch and run offline regression tests.
Do not accept changes that lower match thresholds, bypass authentication/hardware
checks, ignore failed responses, broaden Polkit/PAM privileges, or erase credentials
just to turn an error into success. Protocol/pairing changes need maintainer review
and a new bounded hardware experiment, not repeated automatic attempts.

For that workflow, clone the workbench (not needed for ordinary installation):

```sh
git clone --recurse-submodules https://github.com/pierspad/REA-FS.git
cd REA-FS
```

Ask the agent to read `AGENTS.md`, `docs/getting-started.md` and the required workflow,
then resume your private checkpoint. The submodule pins an example revision; record
which driver revision you are actually testing, rather than assuming it is latest.
