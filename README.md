# Synaptics 00dc (USB 06cb:00dc)

[![Checks](https://github.com/pierspad/synaptics-00dc/actions/workflows/check.yml/badge.svg)](https://github.com/pierspad/synaptics-00dc/actions/workflows/check.yml)
[![License: LGPL-2.1](https://img.shields.io/badge/License-LGPL--2.1-blue.svg)](LICENSE)
[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-%E2%9D%A4-ea4aaa?logo=github&style=flat)](https://github.com/sponsors/pierspad)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Donate-yellow?logo=buymeacoffee)](https://buymeacoffee.com/pierspad)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-ff5e5b?logo=ko-fi)](https://ko-fi.com/pierspad)

Native Linux fingerprint driver for **Synaptics USB `06cb:00dc`**, using libfprint TOD and fprintd. No Windows DLL, Python runtime or Rust GUI is needed by the driver.

**Experimental:** developed on Acer Aspire A514-54, ROM 6.20 build 154, module `0190:00a2`. Other variants and first pairing on a fresh device need validation. This is not included in upstream libfprint.

## Install

Requires Linux, a C compiler, Meson, Ninja, pkg-config, OpenSSL 3, GUsb, libusb, **libfprint TOD development headers**, and fprintd. Ordinary upstream libfprint alone is insufficient.

### <img src="https://cdn.simpleicons.org/ubuntu/E95420" alt="Ubuntu" width="22" height="22"> Ubuntu 24.04 LTS

The Ubuntu 24.04 CI builds and runs offline tests; this is not a hardware certification.
Enable **Universe** if your installation does not provide
[`libfprint-2-tod-dev`](https://packages.ubuntu.com/noble/libfprint-2-tod-dev).
Run these commands as your normal user:

```sh
sudo apt update &&
sudo apt install build-essential meson ninja-build pkg-config curl \
  libssl-dev libgusb-dev libusb-1.0-0-dev libfprint-2-tod-dev fprintd &&
curl -fsSL https://raw.githubusercontent.com/pierspad/synaptics-00dc/v0.1.1/bootstrap.sh \
  -o /tmp/synaptics-00dc-install.sh &&
bash /tmp/synaptics-00dc-install.sh --setup
```

### <img src="https://cdn.simpleicons.org/archlinux/1793D1" alt="Arch Linux" width="22" height="22"> Arch Linux / CachyOS

Install the dependencies with a full system upgrade:

```sh
sudo pacman -Syu --needed base-devel git meson ninja pkgconf curl \
  openssl libgusb libusb fprintd
```

Then install **one** TOD provider. If you already use `yay`:

```sh
yay -S --needed libfprint-tod
```

[`libfprint-tod`](https://aur.archlinux.org/packages/libfprint-tod) is an
**AUR community package** and replaces/conflicts with ordinary `libfprint`.
Review the package build and replacement prompt. `libfprint-tod-git` is an
alternative, not an additional requirement. Without an AUR helper, follow the
[manual AUR build instructions](https://wiki.archlinux.org/title/Arch_User_Repository#Installing_and_upgrading_packages).
On CachyOS, retain an existing compatible TOD installation; do not install a second provider.

Check TOD development files, then install the driver:

```sh
pkg-config --exists libfprint-2-tod-1 &&
curl -fsSL https://raw.githubusercontent.com/pierspad/synaptics-00dc/v0.1.1/bootstrap.sh \
  -o /tmp/synaptics-00dc-install.sh &&
bash /tmp/synaptics-00dc-install.sh --setup
```

If the `pkg-config` check fails, stop and fix the TOD package first. The driver
installer does not install an AUR helper or replace libfprint automatically.
Local hardware development used CachyOS; other installations still need validation.

### Other distributions

A matching TOD library **and development headers** are required. These instructions
do not establish support for Debian, Fedora, openSUSE, or other Ubuntu releases;
ordinary `libfprint` packages are not interchangeable with TOD. Check
`pkg-config --exists libfprint-2-tod-1` before attempting a local build.

### What setup does

For an existing working installation, update with `bash install.sh --apply` from
a local checkout; do not rerun first-time setup just to update the driver.

Downloads the tagged source, builds and runs offline C tests, then asks sudo to install the module. Guided setup asks before pairing, calibrates with the sensor empty, and starts enrollment. **You still need to touch the sensor.** Keep password access; PAM/login settings are not changed. On a new sensor, setup may fail: stop and report the error rather than repeatedly re-pairing. Review the scripts before executing downloaded code.

Prefer a local checkout? Run `bash install.sh` to preview, `bash install.sh --apply` to install, then `bash setup.sh`. Existing credentials are preserved; optional `--credentials PRIVATE_DIR` imports your own credentials. [Install details and rollback](docs/installation.md).

## Testing and reverse engineering

Want to help? Follow the [tester guide](docs/testing.md) and submit an installation
or hardware report, including failures. Keep biometric data and pairing keys private.
LLM-assisted fixes should include a focused diff and tests, not bypass device checks.

For investigation or agent-guided hardware experiments, use the REA-FS workflow:

```sh
git clone --recurse-submodules https://github.com/pierspad/REA-FS.git
cd REA-FS
```

Read its `AGENTS.md` and `docs/getting-started.md` first. Recursive cloning includes
the pinned example driver; an existing clone can use
`git submodule update --init --recursive`. Ordinary driver installation does not
require REA-FS, its GUI, or its submodules.

## Why TOD?

TOD (Touch OEM Drivers) is a libfprint variant with a loadable-driver interface.
This project uses it to ship a separate module; TOD does not make this driver
proprietary. The normal upstream libfprint cannot load this module as-is.
A native upstream port could remove the TOD dependency, but the current native
prototype is compile-tested only. See [remaining work](docs/status.md).

## Development

```sh
meson setup build
ninja -C build
meson test -C build --print-errorlogs
python3 -m unittest discover -s tests -p 'test_*.py'
```

Offline tests never enroll or open the sensor. Synthetic matcher tests do not establish biometric reliability. The threaded backend remains the default; the asynchronous engine is experimental. See [status and upstream work](docs/status.md).

For reverse engineering another sensor, use [REA-FS](https://github.com/pierspad/REA-FS), the experiment workbench that references this driver as a pinned submodule.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Never publish fingerprints, templates or pairing credentials.

## LLM disclosure

Developed with assistance from Large Language Models for reverse engineering, code, tests and documentation. Hardware results and limitations are recorded separately from generated code.

## License

[GNU LGPL v2.1](LICENSE). See [source provenance](docs/provenance.md) for the extraction history and remaining upstream review questions.
