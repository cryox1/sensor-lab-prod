# Deploying sensor-lab

This guide is written for an **AI coding agent helping a human deploy this stack** (it
works fine as a human checklist too). The repo ships with placeholder values for every
host/path/credential, so a deployment is mostly: *collect a handful of answers, write
them into two or three places, then run one script.*

## How to use this guide (agents, read this first)

1. **Read `AGENTS.md` before doing anything that touches a running stack.** Its
   data-safety rules are mandatory — this stack collects continuous data and its DB has
   been wiped once by a careless deploy.
2. Work through the **Interview** below. Ask the human each question that applies to
   their target (local vs. remote vs. flashing firmware). Don't assume — these values
   are environment-specific and unknowable from the repo.
3. Apply the answers using the **Answer → location** map.
4. Run the **Steps** for the chosen target, then the **Smoke test**.
5. If anything fails, walk the pipeline in the Smoke test order — it isolates the
   broken hop.

Nothing here requires editing source code. Connection details are read from environment
variables; everything else lives in `.env` (gitignored) or per-sketch `secrets.h`
(gitignored).

---

## Interview

### A. Target — ask first
- **Are we deploying locally (this machine) or to a remote server?** Local is just
  `docker compose up`; remote uses `deploy/deploy.sh` over SSH.
- **Will we also flash firmware to ESP devices?** If no, skip section D.

### B. Remote server (skip if local-only) — *required for remote*
| Ask the human | Goes to (env var) | Example |
| --- | --- | --- |
| Server hostname / IP, or SSH alias | `SENSOR_LAB_HOST` | `myserver` or `192.168.1.10` |
| SSH username on that server | `SENSOR_LAB_USER` | `youruser` |
| Path to the SSH private key | `SENSOR_LAB_SSH_KEY` | `~/.ssh/id_ed25519` |
| Directory to deploy into on the server | `SENSOR_LAB_REMOTE_DIR` | `/home/youruser/sensor-lab` |
| A roomy disk path for data + backups on the server | `SENSOR_DATA_DIR` (in the **server's** `.env`) | `/mnt/data/sensor-lab` |

Confirm `ssh "$SENSOR_LAB_HOST"` actually resolves/authenticates and Docker is installed
on the remote before deploying.

### C. Database & app config (`.env`) — *required (local and remote)*
| Ask the human | `.env` key | Default / note |
| --- | --- | --- |
| Postgres password | `POSTGRES_PASSWORD` | **no default — set a strong one** |
| Postgres user / db name | `POSTGRES_USER` / `POSTGRES_DB` | both default `sensors` |
| Where stateful data is stored | `SENSOR_DATA_DIR` | `./data` locally; the big disk on a server |
| Will the dashboard be opened from another machine (LAN/Tailscale), not just localhost? | — | if yes, set the write token below |
| (If non-localhost) a shared write token | `API_WRITE_TOKEN` **and** `WEB_API_WRITE_TOKEN` (same value) | unset = rename/hide endpoints are open |
| (Optional) lock the API to specific origins | `CORS_ORIGINS` | default `*` |
| (Optional) fixed API hostname behind a proxy | `WEB_API_BASE` / `WEB_WS_URL` / `WEB_API_PORT` | unset = auto-detect from the browser host |

Auto-detection means the web app figures out the API URL from whatever host the browser
used — so localhost, a LAN IP, and a Tailscale name all work without rebuilding. Only set
`WEB_API_BASE`/`WEB_WS_URL` when the API lives behind a fixed/separate hostname.

### D. Firmware (skip if not flashing devices)
WiFi/MQTT credentials go in a per-sketch `secrets.h` (gitignored); per-device identity
goes at the top of the `.ino`.
| Ask the human | Goes to | Note |
| --- | --- | --- |
| WiFi SSID + password | `secrets.h`: `WIFI_SSID` / `WIFI_PASS` | one `secrets.h` per sketch folder |
| Server's LAN IP (running Mosquitto) | `secrets.h`: `MQTT_HOST` | same for every device |
| Per-device ID and static IP | `.ino`: `DEVICE_ID` / `STATIC_IP` | unique per device |
| LAN gateway/subnet (if not `10.0.0.0/24`) | `.ino`: `STATIC_GW` / `STATIC_MASK` / `STATIC_DNS` | or comment out `#define USE_STATIC_IP` for DHCP |

---

## Answer → location map (quick reference)

- `SENSOR_LAB_HOST`, `SENSOR_LAB_USER`, `SENSOR_LAB_SSH_KEY`, `SENSOR_LAB_REMOTE_DIR`
  → environment when running `deploy/deploy.sh` (or edit the fallbacks at the top of that
  script).
- `POSTGRES_PASSWORD`, `POSTGRES_USER`, `POSTGRES_DB`, `SENSOR_DATA_DIR`, `CORS_ORIGINS`,
  `API_WRITE_TOKEN`, `WEB_API_WRITE_TOKEN`, `WEB_API_BASE`, `WEB_WS_URL`, `WEB_API_PORT`
  → `.env` (copy from `.env.example`; gitignored). On a remote deploy, the **server's**
  `.env` is the one that matters — `deploy.sh` never overwrites it.
- `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST` → `firmware/<sketch>/secrets.h` (copy from
  `firmware/secrets.h.example`; gitignored).
- `DEVICE_ID`, `STATIC_IP`, `STATIC_GW`/`STATIC_MASK`/`STATIC_DNS` → top of each
  `firmware/<sketch>/<sketch>.ino`.

---

## Steps

### Local
```bash
cp .env.example .env          # then set POSTGRES_PASSWORD (and anything else from section C)
docker compose up -d
```
Open:
- web app — http://localhost:9530
- Redpanda Console — http://localhost:9580
- API — http://localhost:8000 (MQTT on `1883`)

### Remote server
```bash
# 1. Supply connection details (from interview section B):
export SENSOR_LAB_HOST=myserver SENSOR_LAB_USER=youruser \
       SENSOR_LAB_SSH_KEY=~/.ssh/id_ed25519 SENSOR_LAB_REMOTE_DIR=/home/youruser/sensor-lab

# 2. Ensure the server has a .env with a real POSTGRES_PASSWORD and SENSOR_DATA_DIR
#    pointing at the big disk. deploy.sh seeds one from .env.example only if missing,
#    and NEVER overwrites an existing one — so set it once on the server and edit there.

# 3. Deploy. This backs up the DB first (aborts if the backup fails), syncs only
#    source dirs (no --delete), and rebuilds just the app services.
./deploy/deploy.sh
```
`deploy.sh` prints an SSH tunnel command at the end for reaching the web/console/API
ports over the SSH connection.

### Firmware (only if flashing)
```bash
# One secrets.h per sketch folder (Arduino IDE doesn't look outside the folder):
for d in firmware/*/; do cp -n firmware/secrets.h.example "$d/secrets.h"; done
# Fill in WIFI_SSID / WIFI_PASS / MQTT_HOST in each secrets.h,
# then set DEVICE_ID / STATIC_IP at the top of each .ino and flash.
```
See the **Firmware** section of `README.md` for board packages and per-sketch library
requirements.

---

## Smoke test (verify the pipeline end to end)

Run in order — each step isolates one hop:
```bash
# 1. Devices publishing? (subscribe to everything)
docker compose exec mosquitto mosquitto_sub -t 'sensors/#' -v

# 2. Reaching Kafka? — open Redpanda Console at :9580, topic `sensors.telemetry`
#    (malformed payloads land in `sensors.telemetry.dlq`).

# 3. Writer healthy?
docker compose logs --tail=100 tsdb-writer

# 4. Rows in the DB?
docker compose exec timescaledb psql -U sensors -d sensors \
  -c 'SELECT count(*), max(ts) FROM telemetry;'
```
Finally, open the web app (`:9530`) and confirm sensors appear.

You can publish a test reading without any hardware:
```bash
docker compose exec mosquitto mosquitto_pub \
  -t 'sensors/lab/indoor01/telemetry' \
  -m '{"device_id":"indoor01","ts":1714000000,"temp_c":22.5,"humidity":45}'
```

---

## Data-safety reminder

Read **`AGENTS.md`** — these are non-negotiable:
- **Never** `docker compose down -v`, `docker volume rm`, or `docker volume prune` against
  this stack (destroys the database).
- **Never** `docker compose down` the whole stack on a server (remounts stateful volumes).
- **Never** overwrite the server's `docker-compose.yml` / `.env`, and never
  `rsync --delete` against the server tree.
- **Always** back up before any deploy or DB-touching op: `./deploy/backup.sh`. Restore
  only via `./deploy/restore.sh <file>`.
- The **only** supported deploy is `./deploy/deploy.sh` (it backs up first and aborts on
  failure).
