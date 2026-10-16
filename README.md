# Synaptics 00dc

[![Checks](https://github.com/pierspad/synaptics-00dc/actions/workflows/check.yml/badge.svg)](https://github.com/pierspad/synaptics-00dc/actions/workflows/check.yml)
[![License: LGPL-2.1](https://img.shields.io/badge/License-LGPL--2.1-blue.svg)](LICENSE)
[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-%E2%9D%A4-ea4aaa?logo=github&style=flat)](https://github.com/sponsors/pierspad)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Donate-yellow?logo=buymeacoffee)](https://buymeacoffee.com/pierspad)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support-ff5e5b?logo=ko-fi)](https://ko-fi.com/pierspad)

Native Linux fingerprint driver for **Synaptics USB `06cb:00dc`**, using libfprint TOD and fprintd. No Windows DLL, Python runtime or Rust GUI is needed by the driver.

**Experimental:** developed on Acer Aspire A514-54, ROM 6.20 build 154, module `0190:00a2`. Other variants and first pairing on a fresh device need validation. This is not included in upstream libfprint.

## Install

Requires Linux, a C compiler, Meson, Ninja, pkg-config, OpenSSL 3, GUsb, libusb, **libfprint TOD development headers**, and fprintd. Ordinary upstream libfprint alone is insufficient.

Ubuntu 24.04/26.04 prerequisites:

```sh
sudo apt install build-essential meson ninja-build pkg-config curl libssl-dev libgusb-dev libusb-1.0-0-dev libfprint-2-tod-dev fprintd
```

On Arch/CachyOS, install a compatible `libfprint-tod` package first, plus `base-devel meson ninja pkgconf openssl libgusb libusb fprintd curl`. The installer does not replace the system libfprint package or run an AUR helper automatically.

Then, as your normal user:

```sh
curl -fsSL https://raw.githubusercontent.com/pierspad/synaptics-00dc/v0.1.0/bootstrap.sh -o /tmp/synaptics-00dc-install.sh && bash /tmp/synaptics-00dc-install.sh --setup
```

Downloads the tagged source, builds and runs offline C tests, then asks sudo to install the module. Guided setup asks before pairing, calibrates with the sensor empty, and starts enrollment. **You still need to touch the sensor.** Keep password access; PAM/login settings are not changed. On a new sensor, setup may fail: stop and report the error rather than repeatedly re-pairing. Review the scripts before executing downloaded code.

Prefer a local checkout? Run `bash install.sh` to preview, `bash install.sh --apply` to install, then `bash setup.sh`. Existing credentials are preserved; optional `--credentials PRIVATE_DIR` imports your own credentials. [Install details and rollback](docs/installation.md).

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
