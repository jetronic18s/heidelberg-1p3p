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
    # Ensure deterministic component content before local compatibility patches.
    git -C "${dest}" reset --hard "${commit}" >/dev/null
    git -C "${dest}" clean -fd >/dev/null
    echo "${name} ready at ${commit}"
  else
    echo "Cloning ${name}..."
    git clone "${url}" "${dest}"
    git -C "${dest}" checkout -f "${commit}"
  fi
  if [[ "${VERIFY}" -eq 1 && -n "${tree}" ]]; then
    local actual_tree
    actual_tree="$(git -C "${dest}" rev-parse HEAD^{tree})"
    if [[ "${actual_tree}" != "${tree}" ]]; then
      echo "Tree hash mismatch for ${name}: expected ${tree}, got ${actual_tree}" >&2
      exit 1
    fi
  fi
}

patch_arduino_cmake_requires() {
  local arduino_cmake="${COMP_DIR}/arduino/CMakeLists.txt"
  if [[ ! -f "${arduino_cmake}" ]]; then
    return 0
  fi

  python3 - "$arduino_cmake" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

lines = text.splitlines()
changed = False
new_lines = []
for line in lines:
    if line.startswith("set(requires ") and line.endswith(")"):
        prefix = "set(requires "
        suffix = ")"
        body = line[len(prefix):-len(suffix)].strip()
        parts = body.split()
        required = ["esp_timer", "esp_netif", "esp_wifi"]
        for item in required:
            if item not in parts:
                parts.append(item)
                changed = True
        line = prefix + " ".join(parts) + suffix
    new_lines.append(line)

if changed:
    path.write_text("\n".join(new_lines) + "\n")
PY
}

patch_arduino_network_event_group() {
  local netif_cpp="${COMP_DIR}/arduino/libraries/Network/src/NetworkInterface.cpp"

  python3 - "$netif_cpp" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
if not path.exists():
    raise SystemExit(0)

text = path.read_text()
old = """  if (_interface_event_group != NULL) {
    vEventGroupDelete(_interface_event_group);
    _interface_event_group = NULL;
    _initial_bits = 0;
  }
"""
new = """  if (_interface_event_group != NULL) {
    // Keep the event group allocated to avoid use-after-free races with event callbacks.
    // FreeRTOS reserves the top 8 bits for internal use.
    xEventGroupClearBits(_interface_event_group, 0x00FFFFFF);
    _initial_bits = 0;
  }
"""
if old in text:
    text = text.replace(old, new)
    path.write_text(text)
PY
}

patch_arduino_component_manifest() {
  local arduino_manifest="${COMP_DIR}/arduino/idf_component.yml"
  if [[ ! -f "${arduino_manifest}" ]]; then
    return 0
  fi

  python3 - "$arduino_manifest" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()

marker = "\ndependencies:\n"
idx = text.find(marker)
if idx < 0:
    raise SystemExit(0)

examples_marker = "\nexamples:\n"
eidx = text.find(examples_marker, idx + 1)
if eidx < 0:
    raise SystemExit(0)

minimal = """\ndependencies:
  idf: ">=5.3,<5.6"
  joltwallet/littlefs:
    version: "^1.10.2"
"""

patched = text[:idx] + minimal + text[eidx:]
if patched != text:
    path.write_text(patched)
PY
}

patch_emodbus_format_specifiers() {
  local mb_tcp_async_cpp="${COMP_DIR}/eModbus/src/ModbusClientTCPasync.cpp"

  python3 - "$mb_tcp_async_cpp" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
if not path.exists():
    raise SystemExit(0)

text = path.read_text()
old = '      LOG_D("request timeouts (now:%lu-sent:%u)\\n", millis(), request->sentTime);'
new = '      LOG_D("request timeouts (now:%lu-sent:%lu)\\n", millis(), (unsigned long)request->sentTime);'
if old in text:
    text = text.replace(old, new)
    path.write_text(text)
PY
}

ensure_component_cmakelists() {
  local emodbus_cmake="${COMP_DIR}/eModbus/CMakeLists.txt"
  local uptime_cmake="${COMP_DIR}/Uptime/CMakeLists.txt"

  cat > "${emodbus_cmake}" <<'EOF'
set(EMODBUS_SRCS
  "src/CoilData.cpp"
  "src/Logging.cpp"
  "src/ModbusClient.cpp"
  "src/ModbusClientRTU.cpp"
  "src/ModbusMessage.cpp"
  "src/ModbusTypeDefs.cpp"
  "src/RTUutils.cpp"
)

idf_component_register(
  SRCS ${EMODBUS_SRCS}
  INCLUDE_DIRS "src"
  PRIV_REQUIRES arduino
)
EOF

  if [[ ! -f "${uptime_cmake}" ]]; then
    cat > "${uptime_cmake}" <<'EOF'
file(GLOB UPTIME_SRCS "src/*.cpp")

idf_component_register(
  SRCS ${UPTIME_SRCS}
  INCLUDE_DIRS "src"
  PRIV_REQUIRES arduino
)
EOF
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

remove_legacy_components() {
  # Legacy components can still be discovered by ESP-IDF and trigger
  # unintended side effects or stale dependencies.
  # WiFiManager is no longer used; a leftover local component can still be
  # discovered by ESP-IDF and trigger unwanted AP/STA side effects.
  rm -rf "${COMP_DIR}/WiFiManager"
  rm -rf "${COMP_DIR}/ESPAsyncWebServer"
  rm -rf "${COMP_DIR}/ESPTelnet"
  rm -rf "${COMP_DIR}/AsyncTCP"
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
      # handled after joltwallet__littlefs
      continue
      ;;
  esac

  fetch_repo "${name}" "${url}" "${commit}" "${tree}" "${COMP_DIR}/${name}"

done < "${LOCK_FILE}"

# littlefs sub-repo inside joltwallet__littlefs
LITTLEFS_COMMIT=$(awk -F'|' '/^littlefs/ {gsub(/ /, "", $3); print $3}' "${LOCK_FILE}")
LITTLEFS_TREE=$(awk -F'|' '/^littlefs/ {gsub(/ /, "", $4); print $4}' "${LOCK_FILE}")
if [[ -n "${LITTLEFS_COMMIT}" ]]; then
  fetch_repo "littlefs" "https://github.com/littlefs-project/littlefs.git" "${LITTLEFS_COMMIT}" "${LITTLEFS_TREE}" \
    "${COMP_DIR}/joltwallet__littlefs/src/littlefs"
fi

patch_arduino_cmake_requires
patch_arduino_network_event_group
patch_arduino_component_manifest
patch_emodbus_format_specifiers
ensure_component_cmakelists
ensure_local_eth_phy_component
remove_legacy_components

echo "Done."
