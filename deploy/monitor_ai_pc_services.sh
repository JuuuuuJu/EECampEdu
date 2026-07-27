#!/usr/bin/env bash
set -Eeuo pipefail

GATEWAY="140.112.194.42"
SSH_USER="eecamp"
TARGETS="1-10"
LOCAL_TEAM=10
INTERVAL=60
SERVICE="eecamp-portal.service"
AUTO_RESTART=1
ONCE=0
BACKGROUND=0
SHOW_STATUS=0
TAIL_LOG=0
STOP_MONITOR=0
ORIGINAL_ARGS=("$@")

usage() {
  cat <<'USAGE'
Usage:
  bash deploy/monitor_ai_pc_services.sh [options]

Purpose:
  Check systemd user service status on the 10 AI PCs at a fixed interval.
  Default target service is eecamp-portal.service.
  If a service is not active, the monitor restarts it once and re-checks status.

Examples:
  # Check all 10 teams once.
  bash deploy/monitor_ai_pc_services.sh --once

  # Monitor all 10 teams every 60 seconds in the current terminal.
  bash deploy/monitor_ai_pc_services.sh

  # Run monitor in the background, then check progress anytime.
  bash deploy/monitor_ai_pc_services.sh --background
  bash deploy/monitor_ai_pc_services.sh --status
  bash deploy/monitor_ai_pc_services.sh --tail

  # Check selected teams or another service.
  bash deploy/monitor_ai_pc_services.sh --targets 1,2,10 --service eecamp-camera-app.service --once

Options:
  --once              Check once, then exit.
  --background        Start the monitor in the background.
  --status            Show background monitor PID and latest log tail, then exit.
  --tail              Follow the latest background monitor log, then exit.
  --stop              Stop the background monitor recorded in the PID file.
  --interval SECONDS  Poll interval. Default: 60.
  --targets LIST      Target teams. Examples: 1-10, 1,3,5, 10. Default: 1-10.
  --local-team N      Team number of this AI PC. Checked locally, not through SSH. Default: 10.
  --service NAME      systemd user service name. Default: eecamp-portal.service.
  --no-auto-restart   Only report service status; do not restart inactive/failed services.
  --gateway HOST      Gateway host. Default: 140.112.194.42.
  --user USER         SSH user. Default: eecamp.
  -h, --help          Show this help.

Notes:
  Team N uses SSH port 220+N, so Team 1 is 221 and Team 10 is 230.
  The local team is checked with local systemctl --user instead of SSH.
  Logs are written under deploy/runs/.
USAGE
}

log() { printf '[monitor] %s\n' "$*"; }
err() { printf '[monitor][ERROR] %s\n' "$*" >&2; }

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
    --once) ONCE=1 ;;
    --background) BACKGROUND=1 ;;
    --status) SHOW_STATUS=1 ;;
    --tail) TAIL_LOG=1 ;;
    --stop) STOP_MONITOR=1 ;;
    --interval) INTERVAL="${2:?missing value for --interval}"; shift ;;
    --targets) TARGETS="${2:?missing value for --targets}"; shift ;;
    --local-team) LOCAL_TEAM="${2:?missing value for --local-team}"; shift ;;
    --service) SERVICE="${2:?missing value for --service}"; shift ;;
    --no-auto-restart) AUTO_RESTART=0 ;;
    --gateway) GATEWAY="${2:?missing value for --gateway}"; shift ;;
    --user) SSH_USER="${2:?missing value for --user}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) err "Unknown option: $1"; usage; exit 2 ;;
  esac
  shift
done

if ! [[ "$INTERVAL" =~ ^[0-9]+$ ]] || (( INTERVAL < 1 )); then
  err "--interval must be a positive integer"
  exit 2
fi

ROOT="$(repo_root)"
cd "$ROOT"

RUN_DIR="$ROOT/deploy/runs"
PID_FILE="$RUN_DIR/service_monitor.pid"
LATEST_LOG="$RUN_DIR/service_monitor.latest.log"

pid_is_running() {
  local pid="$1"
  [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1
}

print_monitor_status() {
  mkdir -p "$RUN_DIR"
  local pid=""
  [[ -f "$PID_FILE" ]] && pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  if pid_is_running "$pid"; then
    log "background monitor running: pid=$pid"
  elif [[ -n "$pid" ]]; then
    log "background monitor not running: last pid=$pid"
  else
    log "no background monitor pid found"
  fi
  log "latest log: $LATEST_LOG"
  if [[ -f "$LATEST_LOG" ]]; then
    printf '\n--- latest log tail ---\n'
    tail -n 80 "$LATEST_LOG"
  fi
}

if [[ "$SHOW_STATUS" -eq 1 ]]; then
  print_monitor_status
  exit 0
fi

if [[ "$TAIL_LOG" -eq 1 ]]; then
  mkdir -p "$RUN_DIR"
  log "following latest log: $LATEST_LOG"
  touch "$LATEST_LOG"
  tail -f "$LATEST_LOG"
  exit 0
fi

if [[ "$STOP_MONITOR" -eq 1 ]]; then
  mkdir -p "$RUN_DIR"
  pid=""
  [[ -f "$PID_FILE" ]] && pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  if pid_is_running "$pid"; then
    kill "$pid"
    log "stopped background monitor: pid=$pid"
  else
    log "no running background monitor found"
  fi
  exit 0
fi

if [[ "$BACKGROUND" -eq 1 ]]; then
  mkdir -p "$RUN_DIR"
  existing_pid=""
  [[ -f "$PID_FILE" ]] && existing_pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  if pid_is_running "$existing_pid"; then
    err "background monitor already running: pid=$existing_pid"
    err "Check progress with: bash deploy/monitor_ai_pc_services.sh --status"
    exit 1
  fi
  timestamp="$(date +%Y%m%d-%H%M%S)"
  log_file="$RUN_DIR/service_monitor-$timestamp.log"
  ln -sfn "$(basename "$log_file")" "$LATEST_LOG"
  filtered_args=()
  for arg in "${ORIGINAL_ARGS[@]}"; do
    [[ "$arg" == "--background" ]] && continue
    filtered_args+=("$arg")
  done
  nohup bash "$0" "${filtered_args[@]}" > "$log_file" 2>&1 &
  bg_pid=$!
  echo "$bg_pid" > "$PID_FILE"
  log "background monitor started: pid=$bg_pid"
  log "log file: $log_file"
  log "check progress: bash deploy/monitor_ai_pc_services.sh --status"
  log "follow log:     bash deploy/monitor_ai_pc_services.sh --tail"
  log "stop monitor:   bash deploy/monitor_ai_pc_services.sh --stop"
  exit 0
fi

if ! command -v ssh >/dev/null 2>&1; then
  err "ssh is required"
  exit 1
fi

mapfile -t TARGET_ARRAY < <(expand_targets "$TARGETS")

check_one_team() {
  local team="$1"
  local now
  now="$(date '+%Y-%m-%d %H:%M:%S')"

  if [[ "$team" == "$LOCAL_TEAM" ]]; then
    local local_output
    local local_rc=0
    set +e
    local_output="$(
      service="$SERVICE"
      active="$(systemctl --user is-active "$service" 2>/dev/null || true)"
      props="$(systemctl --user show "$service" -p ActiveState -p SubState -p MainPID -p NRestarts -p ExecMainStatus --no-pager 2>/dev/null || true)"
      if [[ -z "$props" ]]; then
        printf 'unknown\tunknown\t-\t-\t-\tservice-not-found-or-systemctl-failed\n'
        exit 0
      fi
      message="local"
      if [[ "$AUTO_RESTART" -eq 1 && "$active" != "active" ]]; then
        before="${active:-unknown}"
        if systemctl --user restart "$service" >/dev/null 2>&1; then
          sleep 2
          active="$(systemctl --user is-active "$service" 2>/dev/null || true)"
          props="$(systemctl --user show "$service" -p ActiveState -p SubState -p MainPID -p NRestarts -p ExecMainStatus --no-pager 2>/dev/null || true)"
          message="local,restarted-from-${before}"
        else
          message="local,restart-failed-from-${before}"
        fi
      fi
      active_state="-"
      sub_state="-"
      main_pid="-"
      restarts="-"
      exit_status="-"
      while IFS='=' read -r key value; do
        case "$key" in
          ActiveState) active_state="${value:-"-"}" ;;
          SubState) sub_state="${value:-"-"}" ;;
          MainPID) main_pid="${value:-"-"}" ;;
          NRestarts) restarts="${value:-"-"}" ;;
          ExecMainStatus) exit_status="${value:-"-"}" ;;
        esac
      done <<< "$props"
      [[ -z "$active" ]] && active="$active_state"
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$active" "$sub_state" "$main_pid" "$restarts" "$exit_status" "$message"
    )"
    local_rc=$?
    set -e

    if [[ "$local_rc" -ne 0 ]]; then
      local compact
      compact="$(printf '%s' "$local_output" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g' | cut -c 1-120)"
      printf '%-19s %-4s %-5s %-10s %-12s %-8s %-8s %-6s %s\n' \
        "$now" "$team" "local" "local-fail" "-" "-" "-" "-" "$compact"
      return 0
    fi

    local active sub_state main_pid restarts exit_status message
    IFS=$'\t' read -r active sub_state main_pid restarts exit_status message <<< "$local_output"
    printf '%-19s %-4s %-5s %-10s %-12s %-8s %-8s %-6s %s\n' \
      "$now" "$team" "local" "$active" "$sub_state" "$main_pid" "$restarts" "$exit_status" "$message"
    return 0
  fi

  local port
  port="$(ssh_port_for_team "$team")"
  local remote_output
  local rc=0

  set +e
  remote_output="$(
    ssh -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=1 \
      -p "$port" "$SSH_USER@$GATEWAY" bash -s -- "$SERVICE" "$AUTO_RESTART" 2>&1 <<'REMOTE'
set -Eeuo pipefail
service="$1"
auto_restart="$2"
active="$(systemctl --user is-active "$service" 2>/dev/null || true)"
props="$(systemctl --user show "$service" -p ActiveState -p SubState -p MainPID -p NRestarts -p ExecMainStatus --no-pager 2>/dev/null || true)"
if [[ -z "$props" ]]; then
  printf 'unknown\tunknown\t-\t-\t-\tservice-not-found-or-systemctl-failed\n'
  exit 0
fi
message="ok"
if [[ "$auto_restart" -eq 1 && "$active" != "active" ]]; then
  before="${active:-unknown}"
  if systemctl --user restart "$service" >/dev/null 2>&1; then
    sleep 2
    active="$(systemctl --user is-active "$service" 2>/dev/null || true)"
    props="$(systemctl --user show "$service" -p ActiveState -p SubState -p MainPID -p NRestarts -p ExecMainStatus --no-pager 2>/dev/null || true)"
    message="restarted-from-${before}"
  else
    message="restart-failed-from-${before}"
  fi
fi
active_state="-"
sub_state="-"
main_pid="-"
restarts="-"
exit_status="-"
while IFS='=' read -r key value; do
  case "$key" in
    ActiveState) active_state="${value:-"-"}" ;;
    SubState) sub_state="${value:-"-"}" ;;
    MainPID) main_pid="${value:-"-"}" ;;
    NRestarts) restarts="${value:-"-"}" ;;
    ExecMainStatus) exit_status="${value:-"-"}" ;;
  esac
done <<< "$props"
[[ -z "$active" ]] && active="$active_state"
printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$active" "$sub_state" "$main_pid" "$restarts" "$exit_status" "$message"
REMOTE
  )"
  rc=$?
  set -e

  if [[ "$rc" -ne 0 ]]; then
    local compact
    compact="$(printf '%s' "$remote_output" | tr '\n' ' ' | sed 's/[[:space:]][[:space:]]*/ /g' | cut -c 1-120)"
    printf '%-19s %-4s %-5s %-10s %-12s %-8s %-8s %-6s %s\n' \
      "$now" "$team" "$port" "ssh-fail" "-" "-" "-" "-" "$compact"
    return 0
  fi

  local active sub_state main_pid restarts exit_status message
  IFS=$'\t' read -r active sub_state main_pid restarts exit_status message <<< "$remote_output"
  printf '%-19s %-4s %-5s %-10s %-12s %-8s %-8s %-6s %s\n' \
    "$now" "$team" "$port" "$active" "$sub_state" "$main_pid" "$restarts" "$exit_status" "$message"
}

run_round() {
  printf '\n[monitor] service=%s gateway=%s interval=%ss targets=%s auto_restart=%s\n' "$SERVICE" "$GATEWAY" "$INTERVAL" "$TARGETS" "$AUTO_RESTART"
  printf '%-19s %-4s %-5s %-10s %-12s %-8s %-8s %-6s %s\n' \
    "time" "team" "port" "active" "substate" "pid" "restarts" "exit" "message"
  printf '%s\n' '-----------------------------------------------------------------------------------------------'
  local team
  for team in "${TARGET_ARRAY[@]}"; do
    check_one_team "$team"
  done
}

while true; do
  run_round
  if [[ "$ONCE" -eq 1 ]]; then
    exit 0
  fi
  sleep "$INTERVAL"
done
