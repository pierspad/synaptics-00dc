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
