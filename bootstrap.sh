#!/usr/bin/env bash
# Download a versioned source snapshot, then build as the invoking user.
set -euo pipefail
VERSION=v0.2.2
REPO=https://github.com/pierspad/synaptics-00dc
if [[ ${1:-} == --help ]]; then
    echo 'Usage: bootstrap.sh [--setup] (downloads, builds/tests and installs v0.2.2)'
    exit 0
fi
[[ $# -eq 0 || ( $# -eq 1 && "$1" == --setup ) ]] || { echo 'Unknown option' >&2; exit 2; }
[[ $EUID -ne 0 ]] || { echo 'Run as your normal user; installation will request sudo.' >&2; exit 1; }
for tool in curl tar pkg-config; do command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }; done
if ! pkg-config --exists libfprint-2-tod-1; then
    echo 'TOD development files are required first. See README → Prerequisites.' >&2
    exit 1
fi
base="${XDG_DATA_HOME:-$HOME/.local/share}/synaptics-00dc"
mkdir -p "$base"
work=$(mktemp -d "$base/$VERSION.XXXXXXXX")
trap 'rm -f "$work/source.tar.gz"' EXIT
curl --fail --show-error --silent --location --proto '=https' --tlsv1.2 \
    "$REPO/archive/refs/tags/$VERSION.tar.gz" -o "$work/source.tar.gz"
tar -xzf "$work/source.tar.gz" --strip-components=1 -C "$work"
bash "$work/install.sh" --apply
echo "Sources and diagnostics retained in $work"
if [[ ${1:-} == --setup ]]; then bash "$work/setup.sh"; fi
