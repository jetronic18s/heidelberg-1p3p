#!/usr/bin/env bash
set -euo pipefail

# Generate draft release notes for a version tag from conventional commit
# subjects since the previous version tag.
#
# Usage: ./scripts/release_notes.sh <tag> [repo-url]

TAG="${1:-}"
REPO_URL="${2:-https://github.com/jetronic18s/heidelberg-1p3p}"
if [[ -z "${TAG}" ]]; then
  echo "Usage: $0 <tag> [repo-url]" >&2
  exit 1
fi

PREV_TAG="$(git describe --tags --abbrev=0 --match 'v*' "${TAG}^" 2>/dev/null || true)"
if [[ -n "${PREV_TAG}" ]]; then
  RANGE="${PREV_TAG}..${TAG}"
else
  RANGE="${TAG}"
fi

# Print commit subjects matching the given types, without the type prefix.
section() {
  local title="$1"
  local types="$2"
  local lines
  lines="$(git log --reverse --format='%s' "${RANGE}" \
    | grep -E "^(${types})(\([^)]*\))?!?: " \
    | sed -E 's/^[a-z]+(\(([^)]*)\))?!?: /- \2: /; s/^- : /- /' || true)"
  if [[ -n "${lines}" ]]; then
    printf '## %s\n%s\n\n' "${title}" "${lines}"
  fi
}

section "New" "feat"
section "Fixes" "fix"
section "Build and maintenance" "build|ci|refactor|perf|test|docs|chore"

printf '%s\n' \
  '## Update' \
  "- **OTA:** web interface → *Firmware update* → upload \`heidelberg-1p3p-${TAG}.bin\`." \
  '  Charging is stopped before the restart and evcc resumes it afterwards.' \
  '  Updating from v1.1.0 or older restarts without stopping charging, so stop charging in evcc first.' \
  '- **Serial:** use the flash bundle, see the included `README.txt`.' \
  ''

if [[ -n "${PREV_TAG}" ]]; then
  echo "**Full Changelog**: ${REPO_URL}/compare/${PREV_TAG}...${TAG}"
fi
