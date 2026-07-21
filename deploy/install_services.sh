#!/usr/bin/env bash
# Install the EECampEdu portal (and optionally the student-PC camera app) as
# systemd *user* services. User services need no sudo for start/stop/status/logs.
#
#   ./deploy/install_services.sh              # portal only
#   ./deploy/install_services.sh --with-camera  # portal + camera app
#
# Override the interpreter if your conda env lives elsewhere:
#   EECAMP_PYTHON=/path/to/python ./deploy/install_services.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${EECAMP_PYTHON:-$HOME/miniconda3/envs/eecampedu/bin/python}"
UNIT_DIR="$HOME/.config/systemd/user"

if [ ! -x "$PYTHON" ]; then
  echo "ERROR: Python interpreter not found/executable: $PYTHON" >&2
  echo "Set EECAMP_PYTHON=/path/to/eecampedu/python and re-run." >&2
  exit 1
fi

mkdir -p "$UNIT_DIR"

render() {  # render <template.in> <dest> substituting @PYTHON@ and @REPO@
  sed -e "s#@PYTHON@#${PYTHON//#/\\#}#g" -e "s#@REPO@#${REPO//#/\\#}#g" "$1" > "$2"
  echo "  wrote $2"
}

echo "Installing user services (python=$PYTHON, repo=$REPO):"
render "$REPO/deploy/systemd/eecamp-portal.service.in" "$UNIT_DIR/eecamp-portal.service"

UNITS="eecamp-portal.service"
if [ "${1:-}" = "--with-camera" ]; then
  render "$REPO/deploy/systemd/eecamp-camera-app.service.in" "$UNIT_DIR/eecamp-camera-app.service"
  UNITS="$UNITS eecamp-camera-app.service"
fi

# Create the secrets env file from the example on first install (never overwrite).
if [ ! -f "$REPO/deploy/eecamp-portal.env" ]; then
  cp "$REPO/deploy/eecamp-portal.env.example" "$REPO/deploy/eecamp-portal.env"
  chmod 600 "$REPO/deploy/eecamp-portal.env"
  echo "  created deploy/eecamp-portal.env (edit it to set EECAMP_PORTAL_SECRET / team passwords)"
fi

systemctl --user daemon-reload
# shellcheck disable=SC2086
systemctl --user enable $UNITS

cat <<EOF

Done. Manage with (no sudo needed):
  systemctl --user start   eecamp-portal
  systemctl --user stop    eecamp-portal
  systemctl --user restart eecamp-portal
  systemctl --user status  eecamp-portal
  journalctl  --user -u eecamp-portal -f      # live logs

To keep services running without an active login (survive logout / reboot),
run once (this is the only step that needs sudo):
  sudo loginctl enable-linger $USER

NOTE: if a portal is already running via 'nohup', stop it before starting the
service so they do not both bind port 8080:
  pkill -f "apps/training_portal/server.py"
EOF
