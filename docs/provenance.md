# Source provenance

Extracted from https://github.com/pierspad/REA-FS, starting at
`5d2139f90a8f174d853e4acad00b8a84b17b3328`, directory `drivers/syna_tod`.
The extraction also preserves the maintainer's local matcher LUT optimization
present on 2026-10-16, plus the subsequent offline hardening changes described in
this repository's history. It is not a byte-identical export of that commit.

Only source, synthetic tests and configuration are included. No session data,
per-device private keys, enrolled templates or captured images are distributed.

`syna_capture_tables.h` contains the ROM6.20 configuration tables already present
in REA-FS. Protocol research used the Acer/Synaptics OEM driver as documented in
REA-FS research notes. The distinction between configuration tables and executable
firmware, exact extraction provenance and acceptable redistribution/upstream
presentation require maintainer review; embedding bytes in C does not resolve
those questions. No claim of upstream acceptance or legal clearance is made.

The TLS implementation is a device-specific protocol implementation using OpenSSL
primitives, not a general-purpose TLS stack or a completed cryptographic audit.
