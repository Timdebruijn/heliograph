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

if [ "$tag" = "--self-test" ]; then
    self_fail=0
    # Driven through the ENVIRONMENT and with no argument, because that is how release.yml
    # invokes this (`run: bash tools/check_version.sh`). An earlier version of this self-test
    # passed each tag as an argument instead -- and every case still passed with the bug
    # reintroduced, because the argument path was never the broken one. The bug was that a ref
    # the old guard did not recognise never reached the comparison at all.
    expect() {  # expect <wanted-exit> <ref-type> <ref-name>
        local want="$1" rt="$2" rn="$3" got=0
        GITHUB_REF_TYPE="$rt" GITHUB_REF_NAME="$rn" "$0" >/dev/null 2>&1 || got=$?
        if [ "$got" -ne "$want" ]; then
            echo "check_version self-test: ${rt} '${rn}' exited ${got}, expected ${want}" >&2
            self_fail=1
        fi
    }
    expect 0 tag "v${declared}"
    expect 0 tag "v${declared}-rc1"
    # Every one of these used to exit 0 WITHOUT CHECKING ANYTHING: the old guard recognised a
    # release tag as /^v[0-9]/, so these refs left $tag empty and the script printed the
    # declared version and returned success -- while release.yml triggers on the far wider
    # "v*" and would have built and published each of them.
    expect 1 tag "vtest"
    expect 1 tag "v-wip"
    expect 1 tag "v"
    expect 1 tag "v1.2"
    expect 1 tag "v0.0.0"
    # A branch must never be read as a version, whatever it is called. This is the false
    # positive the old name-shape guard existed to avoid, and it still must not happen.
    expect 0 branch "v-something"
    expect 0 branch "main"
    if [ "$self_fail" -eq 0 ]; then
        echo "check_version self-test: OK"
    fi
    exit "$self_fail"
fi

# A release is identified by the REF TYPE, not by the shape of the name. GITHUB_REF_NAME holds
# a branch name in ordinary CI, and the old heuristic told the two apart by reading the text --
# which let through every tag that was not v<digit>, unchecked, while release.yml triggers on
# "v*". A tag of vtest, v-wip or a bare v built and published a release with no version check
# at all, and release.yml names the release ${GITHUB_REF_NAME#v}, so vtest shipped as "test".
if [ -z "$tag" ] && [ "${GITHUB_REF_TYPE:-}" = "tag" ]; then
    tag="${GITHUB_REF_NAME:-}"
fi
if [ -z "$tag" ]; then
    echo "$declared"
    exit 0
fi

# The tag must BE a version, not merely begin with a v.
if ! printf '%s' "$tag" | grep -qE '^v[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9.]+)?$'; then
    cat >&2 <<EOF
check_version: FAIL

  tag ${tag} is not a version tag.

Expected vMAJOR.MINOR.PATCH, optionally with a pre-release suffix (v0.19.0-rc1).
release.yml triggers on "v*" and builds its release name from the tag, so a tag that
is not a version would publish a release named after whatever follows the v.
EOF
    exit 1
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
