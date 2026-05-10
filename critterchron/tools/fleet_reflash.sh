#!/usr/bin/env bash
# Re-flash every staging-fleet device with the same firmware + IR script.
#
# Used to satisfy FAILURE_TRIAGE.md §4's "easy reset" success criterion:
# a single command that pushes the current build to the whole fleet so
# their telemetry baselines line up. OTA path (devices already on WiFi);
# for first-flash of a fresh board use `make DEVICE=<name> flash-usb`
# directly.
#
# Usage:
#   tools/fleet_reflash.sh                      # default fleet, swarm.crit
#   tools/fleet_reflash.sh tortoise             # different IR script
#   DEVICES="timmy_tanuki" tools/fleet_reflash.sh   # override fleet list
#
# Devices default to the staging-ESP fleet. When the C3 SuperMinis
# arrive and tommy/tammy are retired, update DEVICES below.
set -euo pipefail

DEVICES="${DEVICES:-timmy_tanuki tommy_tanuki tammy_tanuki roberta_raccoon}"
SCRIPT="${1:-${SCRIPT:-swarm}}"

cd "$(dirname "$0")/../hal"

failed=()
for d in $DEVICES; do
    echo
    echo "=== $d ($SCRIPT) ==="
    if ! make DEVICE="$d" "$SCRIPT" flash; then
        failed+=("$d")
    fi
done

echo
if [ ${#failed[@]} -gt 0 ]; then
    echo "FAILED: ${failed[*]}"
    exit 1
fi
echo "All devices flashed."
