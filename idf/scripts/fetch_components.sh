#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMP_DIR="${ROOT_DIR}/components"
LOCK_FILE="${ROOT_DIR}/components.lock"

if [[ ! -f "${LOCK_FILE}" ]]; then
  echo "Missing ${LOCK_FILE}" >&2
  exit 1
fi

mkdir -p "${COMP_DIR}"

fetch_repo() {
  local name="$1" url="$2" commit="$3" dest="$4"
  if [[ -d "${dest}/.git" ]]; then
    echo "Updating ${name}..."
    git -C "${dest}" fetch --all --tags --prune
  else
    echo "Cloning ${name}..."
    git clone "${url}" "${dest}"
  fi
  git -C "${dest}" checkout -f "${commit}"
}

while IFS='|' read -r name url commit; do
  name="${name// /}"
  url="${url// /}"
  commit="${commit// /}"
  [[ -z "${name}" ]] && continue
  [[ "${name}" =~ ^# ]] && continue

  case "${name}" in
    littlefs)
      # handled after joltwallet__littlefs
      continue
      ;;
  esac

  fetch_repo "${name}" "${url}" "${commit}" "${COMP_DIR}/${name}"

done < "${LOCK_FILE}"

# littlefs sub-repo inside joltwallet__littlefs
LITTLEFS_COMMIT=$(awk -F'|' '/^littlefs/ {gsub(/ /, "", $3); print $3}' "${LOCK_FILE}")
if [[ -n "${LITTLEFS_COMMIT}" ]]; then
  fetch_repo "littlefs" "https://github.com/littlefs-project/littlefs.git" "${LITTLEFS_COMMIT}" \
    "${COMP_DIR}/joltwallet__littlefs/src/littlefs"
fi

echo "Done."
