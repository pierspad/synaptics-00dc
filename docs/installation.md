# Installation and recovery

`install.sh` prints a plan by default. `--apply` builds/tests as the normal user,
then invokes sudo only for file installation. With `--destdir /absolute/staging`
it stages files without sudo, service changes or hardware access.

The module path comes from `pkg-config --variable=tod_driversdir libfprint-2-tod-1`.
State lives in `/var/lib/fprint/syna_00dc` (0700); imported files use 0600.
Credentials are optional at installation and are never downloaded from this repo.
Setup generates them locally only when explicitly requested. An existing partial
credential set is an error, not permission to pair again.

Each install saves a private `syna_00dc-backup.XXXXXXXX` directory under
`/var/lib/fprint`. Its `manifest.tsv` records `restore` paths with copies under
`files/`, and `created` paths that did not exist beforehand. File replacement is
atomic per file, not a transaction spanning the entire installation.

For recovery, stop fprintd, restore only listed `restore` paths, and review each
`created` path before removing it. Do not delete newly enrolled templates or
unrelated state. Reload udev rules and restart fprintd afterwards. Keep the backup
private: it may contain pairing credentials. No automatic rollback is claimed.

The new installer does not install a permissive Polkit rule. If upgrading from
REA-FS's old installer, review `/etc/polkit-1/rules.d/50-syna-00dc-fprintd.rules`:
it granted `setusername` as well as enroll/verify to local active users. Back up
and remove that old rule explicitly to restore distribution defaults. This script
does not silently delete an administrator's existing configuration.

Guided `setup.sh` uses a 45-second initialization deadline and a 120-second
enrollment deadline. It uses `build/` in the checkout by default; pass
`--build-dir DIR` to use another existing build directory (relative paths are
resolved from the directory where the command is run). The directory must contain
both `libsyna_00dc_tod.so` and the `test_fprint_open` diagnostic executable.
Failure stops the sequence and restores fprintd's prior active
or inactive state, as does normal completion. A timeout
does not prove successful device cleanup. Do not retry pairing blindly.

The installation command references the v0.1.2 tag (tags can be moved by a
repository administrator). It uses HTTPS but is not a content-pinned or
signed binary release; builds depend on your local distribution toolchain. No
universal distro compatibility or freshly paired hardware success is claimed.
