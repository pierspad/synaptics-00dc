#!/usr/bin/env bash
# User-invoked guided hardware step. Never run unattended from an agent terminal.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
usage() {
    cat <<'EOF'
Usage: bash setup.sh [--build-dir DIR]

Run the guided pairing, calibration and enrollment steps using an existing
build directory. Relative build directory paths are resolved from the current
working directory. The default is the checkout's build/ directory.
EOF
}
BUILD_DIR="$ROOT/build"
while (($#)); do
    case $1 in
        --help|-h) usage; exit 0 ;;
        --build-dir)
            (($# >= 2)) || { echo 'Missing directory after --build-dir.' >&2; exit 2; }
            BUILD_DIR=$2
            shift 2
            ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
if [[ $BUILD_DIR != /* ]]; then BUILD_DIR="$PWD/$BUILD_DIR"; fi
[[ $EUID -ne 0 ]] || { echo 'Run as your normal desktop user.' >&2; exit 1; }
[[ -r /dev/tty ]] || { echo 'A local interactive terminal is required.' >&2; exit 1; }
for tool in timeout fprintd-enroll sudo; do command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }; done
[[ -f "$BUILD_DIR/libsyna_00dc_tod.so" ]] || { echo "TOD module missing from $BUILD_DIR; build/install the driver first." >&2; exit 1; }
[[ -x "$BUILD_DIR/test_fprint_open" ]] || { echo "Diagnostic executable missing from $BUILD_DIR; build the driver first." >&2; exit 1; }
echo 'Synaptics 06cb:00dc: first pairing/calibration is experimental on new devices.'
echo 'This may pair the sensor with this Linux installation. Keep password access.'
echo 'Leave the sensor EMPTY for calibration (maximum 45 seconds).'
read -r -p 'Type PAIR to initialize locally, or press Enter to stop: ' answer </dev/tty
[[ "$answer" == PAIR ]] || exit 0
sudo -v
# Never repair a partial/corrupt credential set by silently re-pairing.
sudo bash -c '
    dir=/var/lib/fprint/syna_00dc
    count=0
    for name in host_key.pem host_cert.bin server_ecdh_pub.bin; do
        if [ -e "$dir/$name" ]; then count=$((count+1)); fi
    done
    if [ "$count" -ne 0 ] && [ "$count" -ne 3 ]; then
        echo "Partial credentials: stop and recover the backup; no automatic re-pairing." >&2
        exit 1
    fi
    if [ "$count" -eq 3 ]; then
        openssl pkey -in "$dir/host_key.pem" -check -noout >/dev/null 2>&1 || exit 1
        [ "$(wc -c < "$dir/host_cert.bin")" -eq 184 ] || exit 1
        [ "$(wc -c < "$dir/server_ecdh_pub.bin")" -eq 65 ] || exit 1
    fi
'
was_active=0
if systemctl is-active --quiet fprintd; then was_active=1; fi
restore_service() {
    if [[ $was_active -eq 1 ]]; then sudo systemctl start fprintd;
    else sudo systemctl stop fprintd; fi
}
trap restore_service EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sudo systemctl stop fprintd
sudo timeout --signal=INT --kill-after=5s 45s env \
    SYNA_ALLOW_AUTO_PAIR=1 FP_TOD_DRIVERS_DIR="$BUILD_DIR" \
    "$BUILD_DIR/test_fprint_open"
sudo systemctl start fprintd
echo 'Calibration finished. Follow fprintd prompts to touch/lift your right index finger.'
timeout --signal=INT --kill-after=5s 120s fprintd-enroll -f right-index-finger
echo 'Enrollment complete. Use your desktop fingerprint settings to verify.'
echo 'Login/PAM configuration is distribution-specific and was not changed.'
