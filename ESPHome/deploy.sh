#!/usr/bin/env bash
# deploy.sh — Compile Coati Clock firmware, push to rpi4, update manifest.
#
# Usage:
#   ./deploy.sh <device>           # compile + push firmware to rpi4; device self-updates within 6h
#   ./deploy.sh <device> --flash   # same, plus push OTA to device immediately (requires LAN access)
#
# <device> is the ESPHome device name / hostname (e.g. bb32, bb33).
# Each device has its own YAML, binary, and manifest on rpi4.
#
# Prerequisites on rpi4:
#   mkdir -p ~/firmware
#   cd ~/firmware
#   python3 -m http.server 8080 --bind 192.168.153.78   # run once, or as a service

set -euo pipefail

if [[ $# -lt 1 || "${1:-}" == "--help" ]]; then
    echo "Usage: ./deploy.sh <device> [--flash]"
    echo "  <device>   ESPHome device hostname (e.g. bb32, bb33)"
    echo "  --flash    Also push OTA immediately after uploading to rpi4"
    exit 1
fi

FIRMWARE_NAME="$1"
shift
FLASH=false
if [[ "${1:-}" == "--flash" ]]; then FLASH=true; fi

RPI4_HOST="rpi4.local"
RPI4_USER="austin"
RPI4_FIRMWARE_DIR="~/firmware"
MANIFEST_FILE="${FIRMWARE_NAME}_manifest.json"

# Device IP lookup (add entries here as the fleet grows)
case "$FIRMWARE_NAME" in
  bb32) DEVICE_IP="192.168.69.164" ;;
  *)    DEVICE_IP="${FIRMWARE_NAME}.local" ;;  # fallback: use mDNS
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/.esphome/build/${FIRMWARE_NAME}/.pioenvs/${FIRMWARE_NAME}"

# Generate version before compile so the same string is baked into the binary
# and written to the manifest — they must match for the update check to pass.
# Format: YYYY.MM.DD.HHMM  (lexicographically sortable, DNS-style date code)
VERSION="$(date '+%Y.%m.%d.%H%M')"

# Activate the ESPHome virtualenv
source "$SCRIPT_DIR/esphome_2026/bin/activate"

echo "==> Compiling $FIRMWARE_NAME @ $VERSION ..."
export ESPHOME_PROJECT_VERSION="$VERSION"
python3 -m esphome compile "${FIRMWARE_NAME}.yaml"

OTA_BIN="$BUILD_DIR/firmware.ota.bin"
if [ ! -f "$OTA_BIN" ]; then
    echo "ERROR: OTA binary not found at $OTA_BIN"
    exit 1
fi

# Compute MD5 of the OTA binary
MD5="$(md5 -q "$OTA_BIN" 2>/dev/null || md5sum "$OTA_BIN" | awk '{print $1}')"

echo "==> Version: $VERSION"
echo "==> MD5:     $MD5"

# Build the manifest JSON
MANIFEST=$(cat <<EOF
{
  "name": "${FIRMWARE_NAME}",
  "version": "$VERSION",
  "builds": [
    {
      "chipFamily": "ESP32-C3",
      "ota": {
        "path": "http://$RPI4_HOST:8080/$FIRMWARE_NAME.ota.bin",
        "md5": "$MD5",
        "summary": "Built $VERSION"
      }
    }
  ]
}
EOF
)

echo "==> Copying firmware to $RPI4_HOST:$RPI4_FIRMWARE_DIR ..."
scp "$OTA_BIN" "$RPI4_USER@$RPI4_HOST:$RPI4_FIRMWARE_DIR/$FIRMWARE_NAME.ota.bin"
echo "$MANIFEST" | ssh "$RPI4_USER@$RPI4_HOST" "cat > $RPI4_FIRMWARE_DIR/$MANIFEST_FILE"

echo "==> Manifest updated on $RPI4_HOST:"
echo "$MANIFEST"

# Optionally flash the device immediately via push OTA
if [[ "$FLASH" == true ]]; then
    echo "==> Flashing device immediately via push OTA..."
    python3 -m esphome run "${FIRMWARE_NAME}.yaml" --device "$DEVICE_IP" --no-logs
    echo "==> Flash complete."
else
    echo "==> Done. Device will self-update within 6h, or run:"
    echo "    ./deploy.sh $FIRMWARE_NAME --flash   to push immediately"
fi
