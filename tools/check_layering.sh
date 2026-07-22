#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Enforces the two architectural invariants from docs/architecture.md that are cheap to
# check mechanically. Run in CI and before every phase hand-off.
set -uo pipefail

cd "$(dirname "$0")/.."
status=0

echo "==> 1. Brand-specific knowledge must live only in src/drivers/"
# Applies to comments too: the canonical model should not explain itself in terms of one
# driver, or the rule rots into "well, it is only a comment".
if hits=$(grep -rniE 'eversolar|zeversolar|growatt|solax|deye|sunsynk|solis|goodwe' \
        src/ --exclude-dir=drivers 2>/dev/null); then
    echo "FAIL: manufacturer names found outside src/drivers/:"
    echo "$hits"
    status=1
else
    echo "OK"
fi

echo "==> 2. The host-testable core must not depend on Arduino or ESP-IDF"
# These translation units are compiled by env:native. An Arduino include here does not just
# break the build, it means protocol logic has drifted into something untestable.
core_paths=(
    src/device
    src/protocols/pmu/pmu_protocol.h
    src/protocols/pmu/pmu_protocol.cpp
    src/drivers/eversolar_legacy/eversolar_parser.h
    src/drivers/eversolar_legacy/eversolar_parser.cpp
    src/drivers/solax_x1/solax_parser.h
    src/drivers/solax_x1/solax_parser.cpp
)
if hits=$(grep -rnE '#include[[:space:]]*[<"](Arduino|WiFi|HardwareSerial|esp_|driver/|freertos)' \
        "${core_paths[@]}" 2>/dev/null); then
    echo "FAIL: platform headers in the host-testable core:"
    echo "$hits"
    status=1
else
    echo "OK"
fi

echo "==> 3. Fixtures are in sync with their generator"
if command -v python3 >/dev/null 2>&1; then
    before=$(cat test/fixtures/eversolar_frames.h 2>/dev/null)
    python3 tools/gen_fixtures.py >/dev/null
    if [ "$before" != "$(cat test/fixtures/eversolar_frames.h)" ]; then
        echo "FAIL: test/fixtures/eversolar_frames.h is stale; commit the regenerated file"
        status=1
    else
        echo "OK"
    fi
else
    echo "SKIP: python3 not available"
fi

echo "==> 4. Every REST payload builder is reachable from a route"
# A builder with no route is dead code that unit tests happily cover. Three of them shipped
# that way in Phase 7 -- buildDevicePayload, buildMeasurementsPayload and
# buildCapabilitiesPayload were fully tested and unreachable in the firmware.
unrouted=""
for fn in $(grep -oE "^bool build[A-Za-z]+" src/outputs/rest/rest_payloads.cpp | sed 's/^bool //'); do
    if ! grep -q "$fn" src/outputs/rest/rest_api.cpp; then
        unrouted="$unrouted $fn"
    fi
done
if [ -n "$unrouted" ]; then
    echo "FAIL: payload builders with no route:$unrouted"
    status=1
else
    echo "OK"
fi

echo
if [ $status -eq 0 ]; then
    echo "RESULT: PASS"
else
    # A single unambiguous verdict on the last line. Earlier versions of this script ended
    # with the last check's "OK", which meant `check_layering.sh | tail -1` reported success
    # while an earlier check was failing. It did exactly that, and hid a real violation for
    # a whole phase.
    echo "RESULT: FAIL"
fi
exit $status
