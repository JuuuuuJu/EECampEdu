#!/usr/bin/env bash
set -Eeuo pipefail

GATEWAY="140.112.194.42"
SSH_USER="eecamp"
REMOTE_ROOT="/home/eecamp/EECampEdu"
SOURCE_TEAM=10
TARGETS="1-9"
APPLY=0
DELETE_CODE=0
RESTART_SERVICES=1
REBUILD_FIRMWARE=1
RUN_CHECKS=1

FIRMWARE_PROJECTS=(
  "firmware/main_board"
  "firmware/model_finetune"
  "firmware/camera_usb_demo"
  "firmware/deploy_benchmark"
  "firmware/teaching_output_demo"
  "firmware/control_board"
)

usage() {
  cat <<'USAGE'
Usage:
  bash deploy/sync_to_ai_pcs.sh [options]

Purpose:
  Sync the current local working tree from this AI PC to other team AI PCs,
  then run checks, rebuild portal firmware artifacts, and restart services.
  This is rsync-based, so it includes local uncommitted changes.

Common examples:
  # Show what would be synced to Teams 1-9, without changing targets.
  bash deploy/sync_to_ai_pcs.sh

  # Actually sync Teams 1-9 from Team 10, rebuild firmware, restart services.
  bash deploy/sync_to_ai_pcs.sh --apply

  # Sync only Teams 1,2,3 and skip firmware rebuild.
  bash deploy/sync_to_ai_pcs.sh --apply --targets 1,2,3 --skip-rebuild

Options:
  --apply              Really sync and run remote commands. Default is dry-run.
  --targets LIST       Target teams. Examples: 1-9, 1,3,5, 2. Default: 1-9.
  --source-team N      Team number of this source AI PC. Skipped if in targets. Default: 10.
  --gateway HOST       Gateway host. Default: 140.112.194.42.
  --user USER          SSH user. Default: eecamp.
  --remote-root PATH   Repo path on target AI PCs. Default: /home/eecamp/EECampEdu.
  --skip-rebuild       Do not rebuild firmware projects after sync.
  --skip-checks        Do not run Python syntax checks after sync.
  --no-restart         Do not restart portal/camera services.
  --delete-code        Delete target files that disappeared locally, while still preserving runtime data.
  -h, --help           Show this help.

Safety:
  The script never syncs runtime data such as apps/training_portal/runs,
  datasets, generated models, firmware build dirs, artifacts, or secrets.
  Default mode is dry-run. Add --apply only after checking the plan.
USAGE
}

log() { printf '[sync] %s\n' "$*"; }
err() { printf '[sync][ERROR] %s\n' "$*" >&2; }

repo_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  cd "$script_dir/.." && pwd
}

ssh_port_for_team() {
  local team="$1"
  echo $((220 + team))
}

expand_targets() {
  local spec="$1"
  local result=()
  IFS=',' read -ra parts <<< "$spec"
  for part in "${parts[@]}"; do
    part="${part//[[:space:]]/}"
    [[ -z "$part" ]] && continue
    if [[ "$part" =~ ^([0-9]+)-([0-9]+)$ ]]; then
      local start="${BASH_REMATCH[1]}"
      local end="${BASH_REMATCH[2]}"
      if (( start <= end )); then
        for ((i=start; i<=end; i++)); do result+=("$i"); done
      else
        for ((i=start; i>=end; i--)); do result+=("$i"); done
      fi
    elif [[ "$part" =~ ^[0-9]+$ ]]; then
      result+=("$part")
    else
      err "Invalid target spec: $part"
      exit 2
    fi
  done
  printf '%s\n' "${result[@]}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --apply) APPLY=1 ;;
    --targets) TARGETS="${2:?missing value for --targets}"; shift ;;
    --source-team) SOURCE_TEAM="${2:?missing value for --source-team}"; shift ;;
    --gateway) GATEWAY="${2:?missing value for --gateway}"; shift ;;
    --user) SSH_USER="${2:?missing value for --user}"; shift ;;
    --remote-root) REMOTE_ROOT="${2:?missing value for --remote-root}"; shift ;;
    --skip-rebuild) REBUILD_FIRMWARE=0 ;;
    --skip-checks) RUN_CHECKS=0 ;;
    --no-restart) RESTART_SERVICES=0 ;;
    --delete-code) DELETE_CODE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) err "Unknown option: $1"; usage; exit 2 ;;
  esac
  shift
done

ROOT="$(repo_root)"
cd "$ROOT"

if ! command -v rsync >/dev/null 2>&1; then
  err "rsync is required on the source AI PC. Install it or use git-based deployment."
  exit 1
fi

RSYNC_ARGS=(
  -az --human-readable --itemize-changes
  --exclude='.git/'
  --exclude='.venv/'
  --exclude='__pycache__/'
  --exclude='*.pyc'
  --exclude='*.pyo'
  --exclude='apps/training_portal/runs/'
  --exclude='apps/training_portal/runs/***'
  --exclude='deploy/eecamp-portal.env'
  --exclude='model_finetune/dataset/'
  --exclude='model_finetune/dataset/***'
  --exclude='model_finetune/models/'
  --exclude='model_finetune/models/***'
  --exclude='firmware/pc/artifacts/'
  --exclude='firmware/pc/artifacts/***'
  --exclude='**/build/'
  --exclude='**/build/***'
  --exclude='**/managed_components/'
  --exclude='**/managed_components/***'
  --exclude='sdkconfig.old'
  --exclude='*.log'
)

if [[ "$APPLY" -eq 0 ]]; then
  RSYNC_ARGS=(-n "${RSYNC_ARGS[@]}")
fi
if [[ "$DELETE_CODE" -eq 1 ]]; then
  RSYNC_ARGS+=(--delete)
fi

remote_script() {
  cat <<'REMOTE'
set -Eeuo pipefail
cd "$REMOTE_ROOT"

status_check="SKIP"
status_build="SKIP"
status_restart="SKIP"
IDF_EXPORT_SELECTED=""

PYTHON_BIN=""
if [[ -x ".venv/bin/python" ]]; then
  PYTHON_BIN=".venv/bin/python"
elif [[ -x "$HOME/miniconda3/envs/eecampedu/bin/python" ]]; then
  PYTHON_BIN="$HOME/miniconda3/envs/eecampedu/bin/python"
elif [[ -x "$HOME/anaconda3/envs/eecampedu/bin/python" ]]; then
  PYTHON_BIN="$HOME/anaconda3/envs/eecampedu/bin/python"
elif command -v python3 >/dev/null 2>&1; then
  PYTHON_BIN="$(command -v python3)"
elif command -v python >/dev/null 2>&1; then
  PYTHON_BIN="$(command -v python)"
else
  echo "REMOTE_STATUS checks=FAIL build=$status_build restart=$status_restart"
  echo "REMOTE_ERROR no Python interpreter found (.venv, conda eecampedu, python3, python)"
  exit 1
fi

PYTHON_ALIAS_DIR="$(mktemp -d)"
trap 'rm -rf "$PYTHON_ALIAS_DIR"' EXIT
ln -sf "$PYTHON_BIN" "$PYTHON_ALIAS_DIR/python"
export PATH="$PYTHON_ALIAS_DIR:$PATH"

get_idf() {
  local export_sh=""
  local idf_dir=""
  if [[ -n "${IDF_EXPORT_SH:-}" && -f "${IDF_EXPORT_SH}" ]]; then
    export_sh="${IDF_EXPORT_SH}"
  elif [[ -f /opt/esp/idf/export.sh ]]; then
    export_sh="/opt/esp/idf/export.sh"
  elif [[ -f "$HOME/esp/esp-idf/export.sh" ]]; then
    export_sh="$HOME/esp/esp-idf/export.sh"
  else
    echo "BUILD_FAIL: ESP-IDF export.sh not found. Set IDF_EXPORT_SH in deploy/eecamp-portal.env." >&2
    return 30
  fi
  idf_dir="$(cd "$(dirname "$export_sh")" && pwd)"
  IDF_EXPORT_SELECTED="$export_sh"

  git config --global --add safe.directory "$idf_dir" >/dev/null 2>&1 || true
  if [[ -d "$idf_dir/components/openthread/openthread" ]]; then
    git config --global --add safe.directory "$idf_dir/components/openthread/openthread" >/dev/null 2>&1 || true
  fi

  set +e
  . "$export_sh" >/dev/null
  local export_rc=$?
  set -e

  if [[ "$export_rc" -ne 0 ]]; then
    if [[ ! -f "$idf_dir/tools/idf_tools.py" ]]; then
      echo "BUILD_FAIL: ESP-IDF export failed and idf_tools.py was not found under $idf_dir" >&2
      return 31
    fi
    echo "[remote] ESP-IDF export failed; rebuilding ESP-IDF Python environment with $PYTHON_BIN"
    "$PYTHON_BIN" "$idf_dir/tools/idf_tools.py" install-python-env
    . "$export_sh" >/dev/null
  fi

  export IDF_PATH="$idf_dir"
  export PATH="$idf_dir/tools:$PATH"

  if ! command -v idf.py >/dev/null 2>&1 && [[ ! -f "$idf_dir/tools/idf.py" ]]; then
    echo "BUILD_FAIL: idf.py not found after loading ESP-IDF environment or under $idf_dir/tools" >&2
    return 32
  fi
}

build_idf_project() {
  local project="$1"
  bash -lc '
    set -Eeuo pipefail
    export PATH="$1:$PATH"
    cd "$2"
    . "$3" >/dev/null
    mkdir -p "$4/build/log"
    rm -f "$4"/build/log/idf_py_stderr_output_* "$4"/build/log/idf_py_stdout_output_* 2>/dev/null || true
    unset ESPPORT ESPBAUD IDF_TARGET_PORT
    echo "[remote] idf.py -C $4 build"
    if ! idf.py -C "$4" build; then
      echo "[remote][build-failed] $4"
      latest_stderr="$(ls -t "$4"/build/log/idf_py_stderr_output_* 2>/dev/null | head -n 1 || true)"
      latest_stdout="$(ls -t "$4"/build/log/idf_py_stdout_output_* 2>/dev/null | head -n 1 || true)"
      if [[ -n "$latest_stderr" && -f "$latest_stderr" ]]; then
        echo "[remote][stderr-tail] $latest_stderr"
        tail -n 120 "$latest_stderr"
      fi
      if [[ -n "$latest_stdout" && -f "$latest_stdout" ]]; then
        echo "[remote][stdout-tail] $latest_stdout"
        tail -n 120 "$latest_stdout"
      fi
      exit 1
    fi
  ' \
    _ "$PYTHON_ALIAS_DIR" "$REMOTE_ROOT" "$IDF_EXPORT_SELECTED" "$project"
}

if [[ "$RUN_CHECKS" == "1" ]]; then
  "$PYTHON_BIN" -m py_compile apps/training_portal/server.py firmware/pc/tools/quantize_keras_model.py
  status_check="OK"
fi

if [[ "$REBUILD_FIRMWARE" == "1" ]]; then
  if [[ -f deploy/eecamp-portal.env ]]; then
    set -a
    . deploy/eecamp-portal.env
    set +a
  fi

  get_idf
  IFS=';' read -r -a firmware_projects <<< "${FIRMWARE_PROJECTS_TEXT:-}"
  for project in "${firmware_projects[@]}"; do
    if [[ -f "$project/CMakeLists.txt" ]]; then
      echo "[remote] building $project"
      build_idf_project "$project"
    else
      echo "[remote] skip missing firmware project $project"
    fi
  done
  status_build="OK"
fi

if [[ "$RESTART_SERVICES" == "1" ]]; then
  systemctl --user restart eecamp-portal
  if systemctl --user list-unit-files eecamp-camera-app.service >/dev/null 2>&1; then
    systemctl --user restart eecamp-camera-app || true
  fi
  status_restart="OK"
fi

echo "REMOTE_STATUS checks=$status_check build=$status_build restart=$status_restart"
REMOTE
}

mapfile -t target_teams < <(expand_targets "$TARGETS")
if [[ "${#target_teams[@]}" -eq 0 ]]; then
  err "No targets selected."
  exit 2
fi

log "source repo : $ROOT"
log "gateway     : $GATEWAY"
log "targets     : ${target_teams[*]}"
log "mode        : $([[ $APPLY -eq 1 ]] && echo apply || echo dry-run)"
log "rebuild     : $([[ $REBUILD_FIRMWARE -eq 1 ]] && echo yes || echo no)"
log "restart     : $([[ $RESTART_SERVICES -eq 1 ]] && echo yes || echo no)"

printf '\n%-6s %-8s %-8s %-8s %-8s %s\n' "Team" "Sync" "Checks" "Build" "Restart" "Message"
printf '%s\n' "----------------------------------------------------------------------"

failures=0
for team in "${target_teams[@]}"; do
  if [[ "$team" == "$SOURCE_TEAM" ]]; then
    printf '%-6s %-8s %-8s %-8s %-8s %s\n' "$team" "SKIP" "SKIP" "SKIP" "SKIP" "source team"
    continue
  fi
  port="$(ssh_port_for_team "$team")"
  ssh_dest="${SSH_USER}@${GATEWAY}"
  rsync_target="${ssh_dest}:${REMOTE_ROOT}/"
  msg=""
  sync_status="FAIL"
  check_status="SKIP"
  build_status="SKIP"
  restart_status="SKIP"

  if rsync "${RSYNC_ARGS[@]}" -e "ssh -p ${port}" ./ "$rsync_target" >/tmp/eecamp_sync_${team}.log 2>&1; then
    sync_status="OK"
  else
    msg="rsync failed: $(tail -n 1 /tmp/eecamp_sync_${team}.log 2>/dev/null || true)"
    printf '%-6s %-8s %-8s %-8s %-8s %s\n' "$team" "$sync_status" "$check_status" "$build_status" "$restart_status" "$msg"
    failures=$((failures + 1))
    continue
  fi

  if [[ "$APPLY" -eq 0 ]]; then
    printf '%-6s %-8s %-8s %-8s %-8s %s\n' "$team" "$sync_status" "$check_status" "$build_status" "$restart_status" "dry-run only"
    continue
  fi

  firmware_projects_text="$(IFS=';'; echo "${FIRMWARE_PROJECTS[*]}")"
  remote_env=(
    "REMOTE_ROOT=$(printf '%q' "$REMOTE_ROOT")"
    "RUN_CHECKS=$RUN_CHECKS"
    "REBUILD_FIRMWARE=$REBUILD_FIRMWARE"
    "RESTART_SERVICES=$RESTART_SERVICES"
    "FIRMWARE_PROJECTS_TEXT=$(printf '%q' "$firmware_projects_text")"
  )
  if output=$(ssh -p "$port" "$ssh_dest" "${remote_env[*]} bash -s" <<< "$(remote_script)" 2>&1); then
    [[ "$RUN_CHECKS" == "1" ]] && check_status="OK"
    [[ "$REBUILD_FIRMWARE" == "1" ]] && build_status="OK"
    [[ "$RESTART_SERVICES" == "1" ]] && restart_status="OK"
    msg="done"
  else
    msg="remote failed: $(printf '%s' "$output" | tail -n 1)"
    failures=$((failures + 1))
  fi
  printf '%-6s %-8s %-8s %-8s %-8s %s\n' "$team" "$sync_status" "$check_status" "$build_status" "$restart_status" "$msg"
  if [[ -n "${output:-}" && "$msg" == remote\ failed:* ]]; then
    printf '[sync][team %s] remote output tail:\n' "$team"
    printf '%s\n' "$output" | tail -n 80
    printf '\n'
  fi
done

printf '%s\n' "----------------------------------------------------------------------"
if [[ "$APPLY" -eq 0 ]]; then
  log "dry-run finished. Re-run with --apply to update target AI PCs."
fi
if [[ "$failures" -gt 0 ]]; then
  err "$failures target(s) failed."
  exit 1
fi
log "all selected targets completed."
