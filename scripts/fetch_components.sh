#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMP_DIR="${ROOT_DIR}/components"
LOCK_FILE="${ROOT_DIR}/components.lock"
VERIFY=1

if [[ "${1:-}" == "--no-verify" ]]; then
  VERIFY=0
  shift
fi

if [[ ! -f "${LOCK_FILE}" ]]; then
  echo "Missing ${LOCK_FILE}" >&2
  exit 1
fi

mkdir -p "${COMP_DIR}"

fetch_repo() {
  local name="$1" url="$2" commit="$3" tree="$4" dest="$5"
  if [[ -d "${dest}/.git" ]]; then
    local current_commit
    current_commit="$(git -C "${dest}" rev-parse HEAD)"
    if [[ "${current_commit}" != "${commit}" ]]; then
      echo "Updating ${name}..."
      git -C "${dest}" fetch --all --tags --prune
      git -C "${dest}" checkout -f "${commit}"
    fi
    git -C "${dest}" reset --hard "${commit}" >/dev/null
    git -C "${dest}" clean -fd >/dev/null
    echo "${name} ready at ${commit}"
  else
    echo "Cloning ${name}..."
    git clone "${url}" "${dest}"
    git -C "${dest}" checkout -f "${commit}"
  fi
}

ensure_local_eth_phy_component() {
  local dst="${COMP_DIR}/eth_phy_jl1101"
  mkdir -p "${dst}/src" "${dst}/include"

  cp -f "${ROOT_DIR}/lib/eth_phy_jl1101/src/esp_eth_phy_jl1101.c" "${dst}/src/esp_eth_phy_jl1101.c"
  cp -f "${ROOT_DIR}/lib/eth_phy_jl1101/include/esp_eth_phy_jl1101.h" "${dst}/include/esp_eth_phy_jl1101.h"

  cat > "${dst}/CMakeLists.txt" <<'EOF'
idf_component_register(
  SRCS "src/esp_eth_phy_jl1101.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_eth esp_driver_gpio
)
EOF
}

while IFS='|' read -r name url commit tree; do
  name="${name// /}"
  url="${url// /}"
  commit="${commit// /}"
  tree="${tree// /}"
  [[ -z "${name}" ]] && continue
  [[ "${name}" =~ ^# ]] && continue

  case "${name}" in
    littlefs)
      continue
      ;;
  esac

  fetch_repo "${name}" "${url}" "${commit}" "${tree}" "${COMP_DIR}/${name}"

done < "${LOCK_FILE}"

# littlefs sub-repo inside joltwallet__littlefs
LITTLEFS_COMMIT=$(awk -F'|' '/^littlefs/ {gsub(/ /, "", $3); print $3}' "${LOCK_FILE}")
if [[ -n "${LITTLEFS_COMMIT}" ]]; then
  fetch_repo "littlefs" "https://github.com/littlefs-project/littlefs.git" "${LITTLEFS_COMMIT}" "" \
    "${COMP_DIR}/joltwallet__littlefs/src/littlefs"
fi

ensure_local_eth_phy_component

echo "Done."
