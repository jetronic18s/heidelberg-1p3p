#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if ! command -v rg >/dev/null 2>&1; then
  echo "ripgrep (rg) is required" >&2
  exit 2
fi

# Point 4 guard: keep project code free from classic Arduino types/helpers.
STRICT_PATTERN='(\bString\b|\bIPAddress\b|#include\s*<Arduino\.h>|#include\s*"Arduino\.h")'
TIME_HELPER_PATTERN='(\bmillis\s*\(|\bdelay\s*\()'

if rg -n -e "${STRICT_PATTERN}" src include lib; then
  echo
  echo "Found forbidden Arduino-style types/APIs in project sources." >&2
  exit 1
fi

# Ignore lines with string literals to avoid false positives from UI text such as "delay (ms)".
if rg -n -e "${TIME_HELPER_PATTERN}" src include lib | rg -v '"'; then
  echo
  echo "Found forbidden Arduino-style types/APIs in project sources." >&2
  exit 1
fi

echo "OK: no forbidden Arduino-style types/APIs found in src/include/lib."
