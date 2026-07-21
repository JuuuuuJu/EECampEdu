#!/usr/bin/env bash
# Convenience wrapper around the systemd user services.
# Usage: ./deploy/eecampctl.sh <start|stop|restart|status|logs> [portal|camera]
#   ./deploy/eecampctl.sh start          # start the portal
#   ./deploy/eecampctl.sh logs           # follow portal logs
#   ./deploy/eecampctl.sh status camera  # status of the camera app
set -euo pipefail
verb="${1:-status}"
svc="${2:-portal}"
case "$svc" in
  portal) unit="eecamp-portal" ;;
  camera) unit="eecamp-camera-app" ;;
  *) echo "second arg must be 'portal' or 'camera'" >&2; exit 1 ;;
esac
case "$verb" in
  start|stop|restart|status) exec systemctl --user "$verb" "$unit" ;;
  logs)                      exec journalctl --user -u "$unit" -f ;;
  *) echo "first arg must be start|stop|restart|status|logs" >&2; exit 1 ;;
esac
