# Synaptics 00dc (USB 06cb:00dc)

[![Checks](https://github.com/pierspad/synaptics-00dc/actions/workflows/check.yml/badge.svg)](https://github.com/pierspad/synaptics-00dc/actions/workflows/check.yml)
[![License: LGPL-2.1](https://img.shields.io/badge/License-LGPL--2.1-blue.svg)](LICENSE)
[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-%E2%9D%A4-ea4aaa?logo=github&style=flat)](https://github.com/sponsors/pierspad)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Donate-yellow?logo=buymeacoffee)](https://buymeacoffee.com/pierspad)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-ff5e5b?logo=ko-fi)](https://ko-fi.com/pierspad)

Native Linux fingerprint driver for **Synaptics USB `06cb:00dc`**, built for `libfprint-tod` and `fprintd`. No Windows DLL.

**Experimental:** developed on Acer Aspire A514-54 (ROM `6.20` build `154`, module `0190:00a2`). Other hardware revisions and first-time pairing on a fresh device still need validation. Not yet part of upstream `libfprint`.

## Install

Requires Linux, a C compiler, Meson, Ninja, `pkg-config`, OpenSSL 3, GUsb, libusb, **`libfprint-tod` development headers**, and `fprintd`.

### <img src="https://cdn.simpleicons.org/ubuntu/E95420" alt="Ubuntu" width="22" height="22"> Ubuntu 24.04 LTS

Enable **Universe** if [`libfprint-2-tod-dev`](https://packages.ubuntu.com/noble/libfprint-2-tod-dev) is not available, then run as your normal user:

```sh
sudo apt update &&
sudo apt install build-essential meson ninja-build pkg-config curl \
  libssl-dev libgusb-dev libusb-1.0-0-dev libfprint-2-tod-dev fprintd &&
curl -fsSL https://raw.githubusercontent.com/pierspad/synaptics-00dc/v0.2.0/bootstrap.sh \
  -o /tmp/synaptics-00dc-install.sh &&
bash /tmp/synaptics-00dc-install.sh --setup
```

### <img src="https://cdn.simpleicons.org/archlinux/1793D1" alt="Arch Linux" width="22" height="22"> Arch Linux

Install the system dependencies:

```sh
sudo pacman -Syu --needed base-devel git meson ninja pkgconf curl \
  openssl libgusb libusb fprintd
```

Install a TOD provider from the AUR ([`libfprint-tod`](https://aur.archlinux.org/packages/libfprint-tod) replaces standard `libfprint`; skip if already installed):

```sh
yay -S --needed libfprint-tod
```

Then build and install the driver:

```sh
pkg-config --exists libfprint-2-tod-1 &&
curl -fsSL https://raw.githubusercontent.com/pierspad/synaptics-00dc/v0.2.0/bootstrap.sh \
  -o /tmp/synaptics-00dc-install.sh &&
bash /tmp/synaptics-00dc-install.sh --setup
```

Setup builds the driver, installs it with a backup, and guides pairing, calibration, and enrollment. Keep the reader clear until prompted to touch it. PAM/login settings are not modified. See [installation and recovery](docs/installation.md).

## Update an existing installation

From a local checkout:

```sh
bash install.sh --apply
```

Existing credentials are preserved; do not re-run pairing to update the driver.

## Development

```sh
meson setup build
ninja -C build
meson test -C build --print-errorlogs
python3 -m unittest discover -s tests -p 'test_*.py'
```

[Testing and reporting bugs](docs/testing.md) ·
[Known limitations and upstream work](docs/status.md)

To reverse-engineer another sensor, clone the [REA-FS workbench](https://github.com/pierspad/REA-FS):

```sh
git clone --recurse-submodules https://github.com/pierspad/REA-FS.git
```

Start with its `AGENTS.md` and `docs/getting-started.md`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Never publish fingerprints, templates, or pairing credentials.

## LLM disclosure

Developed with assistance from Large Language Models for reverse engineering, code, tests, and documentation. Hardware results and limitations are recorded separately from generated code.

## License

[GNU LGPL v2.1](LICENSE). See [source provenance](docs/provenance.md) for extraction history and upstream review notes.
