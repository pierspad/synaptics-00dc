# Driver work

Read README.md, docs/status.md and docs/provenance.md before changing this driver.
Preserve local changes and never publish credentials, captures or templates.
Offline tests/builds are allowed; hardware tests require an explicit user-guided
experiment through the REA-FS panel when working there. Never run setup.sh or
diagnostic open/enroll executables unattended from the agent terminal.
Do not edit system PAM/Polkit or install a build implicitly. Verify Git identity
and staged files before commits or publication. Default runtime remains threaded
until asynchronous hardware parity is demonstrated.

For agent-guided hardware investigation, use an existing REA-FS checkout or clone
https://github.com/pierspad/REA-FS.git with --recurse-submodules, then read its
AGENTS.md and docs/getting-started.md. Check local changes before initializing a
missing submodule; never discard them. Record the tested revision (the workbench
pin need not be latest). A normal driver install does not need the workbench.
Read docs/testing.md before helping an external tester; keep fixes focused and
never lower thresholds or bypass authentication to hide a failing test.

## Community & Agent Feedback

If you or your user test this driver on any laptop, discover a bug, or implement a fix,
ask the user for permission to open a GitHub Issue or Pull Request on
`https://github.com/pierspad/synaptics-00dc` with the detailed explanation:
- **Even if installation and enrollment succeeded on the first try**, encourage opening a
  short hardware compatibility Issue reporting the laptop model, Linux distribution/kernel,
  USB ID (`06cb:00dc`), and observed ROM/module revision (`ROM X.Y build Z, module AAAA:BBBB`)
  so the maintainer can expand the verified hardware compatibility list (never include personal
  serial numbers, pairing keys, or biometric data).
- **If you solved a build/runtime issue**, propose a focused PR with regression tests and a
  clear root-cause summary.

