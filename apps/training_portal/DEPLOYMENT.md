# AI PC Portal — Classroom Network Deployment

How to make each team's training portal reachable at
`http://140.112.194.42:808X` from student PCs.

The public address `140.112.194.42` is **not** an AI PC — it is the classroom
**gateway**. Each AI PC only ever serves plain `:8000`; the gateway maps a
distinct public port (`8081`–`8090`) to each team's AI PC.

```
 Student PC ──http──► 140.112.194.42:808X ──(port forward)──► AI-PC_X (LAN IP):8000
 (browser)            classroom GATEWAY                        training portal (Flask)
```

The work is two parts. **Part A** is done on each AI PC (you can do it).
**Part B** is done on the gateway (needs gateway/IT admin).

---

## Part A — On each AI PC

### A1. Give the AI PC a stable LAN IP

Port forwarding breaks if the AI PC's IP changes, so it must be fixed:

- **Preferred:** add a **DHCP reservation** on the LAN router binding the AI PC's
  MAC to a fixed IP, or
- set a **static IP** on the AI PC (netplan / NetworkManager — needs `sudo`).

Find the AI PC's wired IP and the interface/route to the gateway:

```bash
hostname -I                       # e.g. 192.168.1.125
ip route get 140.112.194.42       # shows which NIC/IP reaches the gateway
```

Use the **wired** IP that appears as `src` in `ip route get` (a machine with both
Ethernet and Wi‑Fi has two IPs — forward to the wired one). Record each team's
final IP for the Part B table.

### A2. Run the portal so it survives logout / reboot

Bind `0.0.0.0` (already the default) so it is reachable from other machines, not
just localhost.

**Quick (per boot), no sudo:**

```bash
conda activate eecampedu
cd ~/EECampEdu
nohup python apps/training_portal/server.py --host 0.0.0.0 --port 8000 \
      > ~/portal.log 2>&1 &
```

**Robust (auto-start on boot) — user systemd service, no sudo:**

```bash
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/portal.service <<'EOF'
[Unit]
Description=EECampEdu training portal
After=network-online.target

[Service]
ExecStart=/home/eecamp/miniconda3/envs/eecampedu/bin/python /home/eecamp/EECampEdu/apps/training_portal/server.py --host 0.0.0.0 --port 8000
WorkingDirectory=/home/eecamp/EECampEdu
Restart=always

[Install]
WantedBy=default.target
EOF
systemctl --user daemon-reload
systemctl --user enable --now portal.service
# optional: start before login (one-time, needs sudo):
#   sudo loginctl enable-linger $USER
```

Adjust the two absolute paths if your env or checkout lives elsewhere.

### A3. Open the AI PC firewall for TCP 8000 — needs sudo

If a host firewall (`ufw`) is active, inbound `:8000` is blocked until allowed.
This needs `sudo`, so run it yourself:

```bash
sudo ufw status                                              # if 'inactive', skip this step
sudo ufw allow from 192.168.1.0/24 to any port 8000 proto tcp   # tightest: only the LAN the gateway is on
# or simply:  sudo ufw allow 8000/tcp
```

### A4. Verify

```bash
curl -s http://127.0.0.1:8000/api/health        # on the AI PC itself
curl -s http://<AIPC_LAN_IP>:8000/api/health    # from another PC on the same LAN
```

Both should return `{"status":"ok", ...}`. If the LAN one fails, the problem is
the firewall (A3) or the IP (A1) — not the gateway yet.

---

## Part B — On the classroom gateway (140.112.194.42)

Done on the gateway device, **not** the AI PC. Needs admin access to
`140.112.194.42` (its web UI or SSH). If campus/lab IT runs it, hand them this
mapping table:

| Team | Public endpoint | Forward to (AI PC wired IP : port) |
|------|-----------------|-----------------------------------|
| 1 | `140.112.194.42:8081` | `AIPC1_IP:8000` |
| 2 | `140.112.194.42:8082` | `AIPC2_IP:8000` |
| 3 | `140.112.194.42:8083` | `AIPC3_IP:8000` |
| 4 | `140.112.194.42:8084` | `AIPC4_IP:8000` |
| 5 | `140.112.194.42:8085` | `AIPC5_IP:8000` |
| 6 | `140.112.194.42:8086` | `AIPC6_IP:8000` |
| 7 | `140.112.194.42:8087` | `AIPC7_IP:8000` |
| 8 | `140.112.194.42:8088` | `AIPC8_IP:8000` |
| 9 | `140.112.194.42:8089` | `AIPC9_IP:8000` |
| 10 | `140.112.194.42:8090` | `AIPC10_IP:8000` |

Implement the mapping with whichever fits the gateway:

**Option 1 — Router UI (simplest).** Open the **Port Forwarding / Virtual
Server** page; add one rule per team: External port `808X` (TCP) → Internal IP
`AIPC_X`, internal port `8000`.

**Option 2 — Linux gateway (iptables DNAT):**

```bash
# as root on 140.112.194.42
sysctl -w net.ipv4.ip_forward=1
iptables -t nat -A PREROUTING  -p tcp --dport 8081 -j DNAT --to-destination AIPC1_IP:8000
iptables -t nat -A POSTROUTING -p tcp -d AIPC1_IP --dport 8000 -j MASQUERADE
# repeat 8082→AIPC2 … 8090→AIPC10
```

The `MASQUERADE` (SNAT) line matters: without it the AI PC needs a route back to
the student subnet; with it, replies just return via the gateway.

**Option 3 — nginx reverse proxy (cleanest for HTTP):**

```nginx
# one server block per team
server { listen 8081; location / { proxy_pass http://AIPC1_IP:8000;
         proxy_set_header Host $host; proxy_set_header X-Real-IP $remote_addr; } }
server { listen 8082; location / { proxy_pass http://AIPC2_IP:8000; } }
# …
```

Also ensure the gateway's own firewall permits inbound `8081`–`8090` from the
student network.

---

## Part C — End-to-end verification

From a student-side machine:

```bash
curl -s http://140.112.194.42:8081/api/health     # team 1
```

Then open `http://140.112.194.42:808X/` in a browser — the portal page should
load. Walk failures back:

- Works on `AIPC_IP:8000` but not `140.112.194.42:808X` → gateway rule/firewall (Part B).
- Fails even on `AIPC_IP:8000` → AI PC firewall, or server not bound to `0.0.0.0` (Part A).

---

## Important caveats

- **Keep the portal on `http`, not `https`.** The browser page also calls the
  local flash helper on `http://127.0.0.1:8765`. If the portal is served over
  HTTPS, browsers block calls to `http://127.0.0.1` (mixed content) and
  browser-based flashing breaks. Plain `http` through the gateway is supported.
- **One `:8000` per AI PC.** Every AI PC uses the same internal port; the
  *gateway* assigns each a distinct public port. Do not give each AI PC a
  different internal port.
- **Wired vs Wi‑Fi.** If an AI PC has both, forward to the **wired** IP (the one
  `ip route get 140.112.194.42` reports as `src`).

## Privilege summary

| Action | Where | Privilege |
|--------|-------|-----------|
| Static IP / DHCP reservation | AI PC or LAN router | sudo / router admin |
| Allow inbound TCP 8000 | AI PC | **sudo** (`ufw`) |
| `loginctl enable-linger` | AI PC | sudo (only for boot-before-login) |
| Port-forward `808X → AIPC:8000` | **Gateway 140.112.194.42** | **gateway / campus IT admin** |
| Open `8081–8090` inbound | Gateway | gateway admin |

The gateway port-forwarding (Part B) is the one piece outside the AI PC's
control — if you don't administer `140.112.194.42`, send the Part B table to
whoever does.
