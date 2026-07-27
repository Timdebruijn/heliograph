#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Refuses a tag that disagrees with the version the firmware reports about itself.
#
# There are two versions in a release and nothing used to compare them. The workflow takes its
# number from the TAG NAME; the firmware reports the #defines in src/main.cpp, and that is what
# lands in /api/v1/status, the Modbus identity block and the Home Assistant device. Tag without
# bumping and every bridge reports the old number while latest.json advertises the new one --
# and the dashboard's update check compares exactly those two, so it offers the same update
# forever, to everyone, until somebody notices.
#
# That is not hypothetical: the bump was a manual step through 0.18.0, 0.18.1 and 0.18.2, and
# went right three times only because someone remembered.
#
# Usage:
#   tools/check_version.sh            # print what the firmware declares
#   tools/check_version.sh v0.19.0    # check a tag before creating it
#   GITHUB_REF_NAME=v0.19.0 tools/check_version.sh
set -euo pipefail

source_file="src/main.cpp"

read_define() {
    # The define, not the rendered string: the string is assembled by the preprocessor and
    # cannot be read out of the source without one.
    local name="$1"
    local value
    value=$(grep -oE "^#define ${name} [0-9]+" "$source_file" | grep -oE '[0-9]+$' || true)
    if [ -z "$value" ]; then
        echo "check_version: cannot read ${name} from ${source_file}" >&2
        exit 2
    fi
    printf '%s' "$value"
}

declared="$(read_define HELIOGRAPH_VERSION_MAJOR).$(read_define HELIOGRAPH_VERSION_MINOR).$(read_define HELIOGRAPH_VERSION_PATCH)"

# An explicit argument is always checked. GITHUB_REF_NAME is only checked when it LOOKS like a
# release tag -- in ordinary CI that variable holds a branch name, and a version check that
# tried to read "main" as a version would fail every pull request for a reason that has nothing
# to do with the change. Being wired into the wrong workflow should do nothing, not break it.
tag="${1:-}"
if [ -z "$tag" ]; then
    case "${GITHUB_REF_NAME:-}" in
        v[0-9]*) tag="$GITHUB_REF_NAME" ;;
    esac
fi
if [ -z "$tag" ]; then
    echo "$declared"
    exit 0
fi

# A pre-release keeps the version of the release it leads to: v0.19.0-rc1 is 0.19.0 with a
# label, and requiring the label in the source would mean a commit per release candidate.
wanted="${tag#v}"
wanted="${wanted%%-*}"

if [ "$wanted" != "$declared" ]; then
    cat >&2 <<EOF
check_version: FAIL

  tag says           ${tag}  (version ${wanted})
  ${source_file} declares  ${declared}

The firmware would report ${declared} while the release advertises ${wanted}. Every bridge
would then be offered this update forever, because the dashboard compares those two numbers.

Fix: set the defines in ${source_file} to ${wanted}, merge that, and tag the merge commit.
Never move the tag onto a build that reports a different number.
EOF
    exit 1
fi

echo "check_version: OK (${tag} matches ${source_file})"
