# AI PC Portal Deployment

This is the deployment guide for one AI PC serving one team.

## Public Mapping

The gateway public IP is:

```text
140.112.194.42
```

Team mapping:

| Team | Portal URL | SSH |
|---:|---|---|
| 1 | `https://140.112.194.42:4431` | `ssh -p 221 eecamp@140.112.194.42` |
| 2 | `https://140.112.194.42:4432` | `ssh -p 222 eecamp@140.112.194.42` |
| 3 | `https://140.112.194.42:4433` | `ssh -p 223 eecamp@140.112.194.42` |
| 4 | `https://140.112.194.42:4434` | `ssh -p 224 eecamp@140.112.194.42` |
| 5 | `https://140.112.194.42:4435` | `ssh -p 225 eecamp@140.112.194.42` |
| 6 | `https://140.112.194.42:4436` | `ssh -p 226 eecamp@140.112.194.42` |
| 7 | `https://140.112.194.42:4437` | `ssh -p 227 eecamp@140.112.194.42` |
| 8 | `https://140.112.194.42:4438` | `ssh -p 228 eecamp@140.112.194.42` |
| 9 | `https://140.112.194.42:4439` | `ssh -p 229 eecamp@140.112.194.42` |
| 10 | `https://140.112.194.42:4440` | `ssh -p 230 eecamp@140.112.194.42` |

Formula:

- Portal port = `4430 + team number`
- SSH port = `220 + team number`

## Why HTTPS

Browser Web Serial requires a secure context. Use HTTPS for the public portal. Chrome or Edge should be used for flashing.

## Start The Portal

Use the systemd user service:

```bash
cd ~/EECampEdu
bash deploy/install_services.sh
systemctl --user restart eecamp-portal
systemctl --user status eecamp-portal
journalctl --user -u eecamp-portal -f
```

If the portal must run before login:

```bash
sudo loginctl enable-linger $USER
```

## Gateway Requirement

The gateway must forward each public HTTPS port to that team's AI PC portal service. If localhost on the AI PC works but `https://140.112.194.42:443X` fails, check gateway forwarding and firewall rules first.

## Health Check

On the AI PC:

```bash
curl -k https://127.0.0.1:8080/api/health
```

From a student-side machine:

```bash
curl -k https://140.112.194.42:4431/api/health
```

Use the correct team port.

## Restart After Changes

Restart `eecamp-portal` after changing:

- `apps/training_portal/server.py`
- `apps/training_portal/templates/index.html`
- Portal static files
- `deploy/eecamp-portal.env`

Firmware-only source changes require rebuild/reflash, not necessarily a portal restart.
