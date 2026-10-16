#!/usr/bin/env bash
# Build unprivileged; install only this driver. No USB access or PAM changes.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD="$ROOT/build"
DESTDIR=""
APPLY=0
NO_BUILD=0
CREDENTIALS=""
usage() {
    echo 'Usage: bash install.sh [--apply] [--build-dir DIR] [--credentials DIR] [--destdir DIR --no-build]'
    echo 'Default: check prerequisites and print plan. --apply builds/tests then installs with sudo.'
    echo '--destdir stages files without sudo, system services, or USB access.'
}
die() { echo "Error: $*" >&2; exit 1; }
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --apply) APPLY=1; shift ;;
        --build-dir) BUILD=${2:?missing build directory}; shift 2 ;;
        --credentials) CREDENTIALS=${2:?missing credentials directory}; shift 2 ;;
        --destdir) DESTDIR=${2:?missing staging directory}; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        --install-built) [[ $EUID -eq 0 ]] || die 'Internal installation step requires root.'; NO_BUILD=2; shift ;;
        *) usage >&2; die "Unknown argument: $1" ;;
    esac
done
[[ $NO_BUILD -ne 1 || -n "$DESTDIR" ]] || die '--no-build is only for --destdir staging.'
command -v pkg-config >/dev/null || die 'Install pkg-config and libfprint-2-tod-1 development files.'
pkg-config --exists libfprint-2-tod-1 || die 'Missing libfprint-2-tod-1 development files. See README prerequisites; ordinary libfprint is not enough.'
TOD=$(pkg-config --variable=tod_driversdir libfprint-2-tod-1)
[[ "$TOD" == /* && "$TOD" != / && "$TOD" != *..* ]] || die 'Invalid TOD plugin directory.'
[[ -z "$DESTDIR" || ( "$DESTDIR" == /* && "$DESTDIR" != / && "$DESTDIR" != *..* ) ]] || die '--destdir must be an absolute staging directory, not /.'
[[ "$BUILD" == /* ]] || BUILD="$PWD/$BUILD"
MODULE="$DESTDIR$TOD/libsyna_00dc_tod.so"
STATE="$DESTDIR/var/lib/fprint/syna_00dc"
RULE="$DESTDIR/etc/udev/rules.d/60-syna-00dc-tod.rules"
# Reject symlinks in every existing destination component, including staging roots.
for target in "$MODULE" "$STATE" "$RULE"; do
    p="$target"
    while [[ "$p" != / ]]; do
        [[ ! -L "$p" ]] || die "Symlinked destination: $p"
        p=$(dirname -- "$p")
    done
done
if [[ -n "$CREDENTIALS" ]]; then
    CREDENTIALS=$(cd -- "$CREDENTIALS" && pwd)
    for name in host_key.pem host_cert.bin server_ecdh_pub.bin; do
        [[ -s "$CREDENTIALS/$name" && ! -L "$CREDENTIALS/$name" ]] || die "Missing or symlinked credential: $name"
    done
    openssl pkey -in "$CREDENTIALS/host_key.pem" -check -noout >/dev/null 2>&1 || die 'Invalid private key.'
    [[ $(wc -c < "$CREDENTIALS/host_cert.bin") -eq 184 && $(wc -c < "$CREDENTIALS/server_ecdh_pub.bin") -eq 65 ]] || die 'Invalid credential size.'
    for name in capture_init.bin capture_packet.bin baseline_cache.bin; do
        [[ ! -L "$CREDENTIALS/$name" ]] || die "Symlinked input: $name"
    done
fi
echo "Build and offline tests: $BUILD"
echo "Module: $MODULE"
echo "Udev: $RULE; private state: $STATE"
echo 'No PAM edits, permissive Polkit policy, sensor access, or automatic pairing.'
if [[ "$APPLY" != 1 ]]; then
    echo 'DRY RUN. Add --apply to build, test and install.'
    exit 0
fi
if [[ "$NO_BUILD" == 0 ]]; then
    [[ "$EUID" != 0 ]] || die 'Run as your normal user. Only installation uses sudo.'
    for tool in meson ninja cc; do command -v "$tool" >/dev/null || die "Missing build tool: $tool"; done
    if [[ ! -f "$BUILD/build.ninja" ]]; then meson setup "$BUILD" "$ROOT"; fi
    ninja -C "$BUILD"
    meson test -C "$BUILD" --print-errorlogs
fi
[[ -f "$BUILD/libsyna_00dc_tod.so" && ! -L "$BUILD/libsyna_00dc_tod.so" ]] || die 'Build module missing or symlinked.'
if [[ -z "$DESTDIR" && "$EUID" != 0 ]]; then
    args=(--apply --install-built --build-dir "$BUILD")
    [[ -z "$CREDENTIALS" ]] || args+=(--credentials "$CREDENTIALS")
    exec sudo bash "$ROOT/install.sh" "${args[@]}"
fi
umask 077
install -d -m 0755 "$DESTDIR/var/lib/fprint"
BACKUP=$(mktemp -d "$DESTDIR/var/lib/fprint/syna_00dc-backup.XXXXXXXX")
for target in "$MODULE" "$RULE" "$STATE"; do
    if [[ -e "$target" ]]; then
        rel=${target#"$DESTDIR"}
        install -d "$BACKUP/files$(dirname -- "$rel")"
        cp -a -- "$target" "$BACKUP/files$rel"
        printf 'restore\t%s\n' "$rel" >> "$BACKUP/manifest.tsv"
    else
        printf 'created\t%s\n' "${target#"$DESTDIR"}" >> "$BACKUP/manifest.tsv"
    fi
done
install -d -m 0755 "$(dirname -- "$MODULE")" "$(dirname -- "$RULE")"
install -d -m 0700 "$STATE"
atomic_install() {
    local src=$1 dest=$2 mode=$3 tmp
    [[ ! -L "$dest" ]] || die "Symlinked destination: $dest"
    tmp=$(mktemp "$(dirname -- "$dest")/.syna-install.XXXXXXXX")
    if ! install -m "$mode" -- "$src" "$tmp" || ! mv -fT -- "$tmp" "$dest"; then
        rm -f -- "$tmp"
        die "Install failed. Backup: $BACKUP"
    fi
}
if [[ -n "$CREDENTIALS" ]]; then
    for name in host_key.pem host_cert.bin server_ecdh_pub.bin capture_init.bin capture_packet.bin baseline_cache.bin; do
        [[ ! -f "$CREDENTIALS/$name" ]] || atomic_install "$CREDENTIALS/$name" "$STATE/$name" 0600
    done
fi
atomic_install "$BUILD/libsyna_00dc_tod.so" "$MODULE" 0755
atomic_install "$ROOT/60-syna-00dc-tod.rules" "$RULE" 0644
if [[ -z "$DESTDIR" ]]; then
    udevadm control --reload-rules
    if systemctl is-active --quiet fprintd; then systemctl restart fprintd; fi
fi
echo "Installed. Backup and restore manifest: $BACKUP"
echo 'Next: bash setup.sh (guided first pairing/calibration and enrollment).'
