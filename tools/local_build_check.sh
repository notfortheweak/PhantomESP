#!/usr/bin/env bash
#
# local_build_check.sh — reproduce the GitHub CI (compile_all.yml) build locally
# so teardown changes can be compile-verified before committing.
#
# Mirrors .github/workflows/compile_all.yml:
#   - ESP-IDF v6.0.2 (~/esp-idf)
#   - per-target sdkconfig applied from configs/, plus the two CI appends
#     (FATFS_USE_DYN_BUFFERS off, ESP_GDBSTUB_ENABLED off)
#   - the 6.0.2 gdbstub source patch
#   - idf.py clean && idf.py build
#
# Python is pinned to 3.12 via ~/.idf-py-shim because the system python (3.14)
# is outside IDF 6.0.2's supported range.
#
# Usage:
#   tools/local_build_check.sh <board-name> [more-board-names...]
#   tools/local_build_check.sh --list          # list known boards
#
# Board name = the config suffix, e.g. esp32c6 -> configs/sdkconfig.default.esp32c6
# or a full config basename, e.g. ghostboard -> configs/sdkconfig.ghostboard.
# Board -> idf_target is auto-detected from the sdkconfig's CONFIG_IDF_TARGET.

set -euo pipefail

IDF_PATH_LOCAL="${IDF_PATH_LOCAL:-$HOME/esp-idf}"
PY_SHIM="${PY_SHIM:-$HOME/.idf-py-shim}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log()  { printf '\033[1;36m[build-check]\033[0m %s\n' "$*"; }
err()  { printf '\033[1;31m[build-check] ERROR:\033[0m %s\n' "$*" >&2; }

resolve_config() {
  # $1 = board arg; echoes the sdkconfig file path (relative to project)
  local b="$1"
  for cand in "configs/sdkconfig.default.$b" "configs/sdkconfig.$b" "$b"; do
    [ -f "$PROJECT_DIR/$cand" ] && { echo "$cand"; return 0; }
  done
  return 1
}

detect_target() {
  # $1 = sdkconfig path; echoes idf_target (esp32c6, etc.)
  local cfg="$PROJECT_DIR/$1"
  local t
  t="$(sed -nE 's/^CONFIG_IDF_TARGET="([^"]+)"/\1/p' "$cfg" | head -1)"
  echo "$t"
}

apply_gdbstub_patch() {
  # Reproduces the "Patch ESP-IDF 6.0.2 gdbstub compatibility" CI step.
  IDF_TARGET="$1" python3 - "$IDF_PATH_LOCAL" <<'PY'
from pathlib import Path
import os, re, sys

idf = Path(sys.argv[1])

gdbstub = idf / "components/esp_gdbstub/src/gdbstub.c"
source = gdbstub.read_text(encoding="utf-8")
helper_guard = re.compile(
    r"#if\s*\(?\s*CONFIG_ESP_SYSTEM_GDBSTUB_RUNTIME\s*\|\|\s*CONFIG_ESP_GDBSTUB_SUPPORT_TASKS\s*\)?"
)
if helper_guard.search(source):
    source = helper_guard.sub("#if 1", source)
    gdbstub.write_text(source, encoding="utf-8")

if os.environ["IDF_TARGET"] in {"esp32", "esp32s2", "esp32s3"}:
    xtensa = idf / "components/esp_gdbstub/src/port/xtensa/gdbstub_xtensa.c"
    source = xtensa.read_text(encoding="utf-8")
    source = source.replace("portNUM_PROCESSORS", "CONFIG_FREERTOS_NUMBER_OF_CORES")
    source, tcb_count = re.subn(
        r"const\s+StaticTask_t\s*\*\s*tcb\s*;", "void *tcb = NULL;", source)
    source, lookup_count = re.subn(
        r"tcb\s*=\s*esp_gdbstub_find_tcb_by_frame\s*\(frame\)\s*;", "", source)
    source, call_count = re.subn(
        r"#if\s+(?:XCHAL_HAVE_FP|0)\s+(?=gdbstub_write_fpu_regs\s*\()",
        "#if 0\n    ", source)
    if tcb_count != lookup_count or tcb_count not in {0, 1}:
        raise SystemExit(
            f"unexpected Xtensa FPU patch counts: tcb={tcb_count}, lookup={lookup_count}")
    xtensa.write_text(source, encoding="utf-8")
print("gdbstub patch applied for", os.environ["IDF_TARGET"])
PY
}

build_one() {
  local board="$1" cfg target
  cfg="$(resolve_config "$board")" || { err "no sdkconfig for board '$board'"; return 2; }
  target="$(detect_target "$cfg")"
  [ -n "$target" ] || { err "could not detect CONFIG_IDF_TARGET in $cfg"; return 2; }

  log "board=$board  config=$cfg  target=$target"
  cd "$PROJECT_DIR"

  # --- Apply Custom SDK Config (CI: "Apply Custom SDK Config") ---
  rm -f sdkconfig sdkconfig.defaults
  cp "$cfg" sdkconfig.defaults
  cp "$cfg" sdkconfig
  for f in sdkconfig.defaults sdkconfig; do
    printf '\n# CONFIG_FATFS_USE_DYN_BUFFERS is not set\n' >> "$f"
    printf '\n# CONFIG_ESP_GDBSTUB_ENABLED is not set\n'   >> "$f"
  done

  apply_gdbstub_patch "$target"

  export IDF_TARGET="$target"
  export SDKCONFIG_DEFAULTS="sdkconfig.defaults"
  export CMAKE_BUILD_PARALLEL_LEVEL="$(nproc)"

  log "idf.py set-target $target && clean && build"
  idf.py set-target "$target"
  idf.py clean
  idf.py build
}

main() {
  if [ "${1:-}" = "--list" ]; then
    ls "$PROJECT_DIR/configs" | sed 's/^sdkconfig\.//' | sort
    exit 0
  fi
  [ $# -ge 1 ] || { err "usage: $0 <board-name> [more...]  (or --list)"; exit 1; }

  [ -f "$IDF_PATH_LOCAL/export.sh" ] || { err "ESP-IDF not found at $IDF_PATH_LOCAL"; exit 1; }

  export PATH="$PY_SHIM:$PATH"
  export IDF_PATH="$IDF_PATH_LOCAL"
  log "using python: $(python3 --version)  IDF_PATH=$IDF_PATH"
  # shellcheck disable=SC1091
  . "$IDF_PATH_LOCAL/export.sh" >/dev/null

  local rc=0 board results=()
  for board in "$@"; do
    if build_one "$board"; then
      results+=("PASS  $board")
    else
      results+=("FAIL  $board")
      rc=1
    fi
  done

  echo
  log "==== summary ===="
  for r in "${results[@]}"; do printf '  %s\n' "$r"; done
  exit $rc
}

main "$@"
