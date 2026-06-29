#!/usr/bin/env bash
# Deploy sensor-lab to your server -- SAFELY.
#
# Data-safety rules baked into this script (see AGENTS.md for the full list):
#   * Back up the production DB FIRST and abort the deploy if the backup fails.
#   * Sync ONLY source dirs, with NO --delete. The server's docker-compose.yml,
#     .env, data dirs, and any server-only files are never in the push set, so
#     they can't be overwritten or deleted.
#   * NEVER `docker compose down` the stack. Stateful services (postgres,
#     mosquitto) stay up so their volumes are never remounted.
#   * Rebuild/restart only the app services.
set -euo pipefail

# Connection details. Override via env vars (no need to edit this file) -- e.g.
#   export SENSOR_LAB_HOST=myserver SENSOR_LAB_USER=me \
#          SENSOR_LAB_SSH_KEY=~/.ssh/id_ed25519 SENSOR_LAB_REMOTE_DIR=/home/me/sensor-lab
# See DEPLOYMENT.md for the full setup interview.
HOST="${SENSOR_LAB_HOST:-myserver}"
REMOTE_USER="${SENSOR_LAB_USER:-youruser}"
KEY="${SENSOR_LAB_SSH_KEY:-${HOME}/.ssh/id_myserver}"
REMOTE="${SENSOR_LAB_REMOTE_DIR:-/home/youruser/sensor-lab}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

ssh_remote() { ssh -i "${KEY}" "${REMOTE_USER}@${HOST}" "$@"; }

echo "==> Ensuring remote dir exists"
ssh_remote "mkdir -p ${REMOTE}/mosquitto"

# --- 1. BACK UP FIRST. Stream backup.sh to the remote so it works even before
#        deploy/ has ever been synced. Abort the deploy if the backup fails. ---
echo "==> Backing up production DB before deploy (abort on failure)"
if ! ssh_remote "bash -s ${REMOTE}" < "${SCRIPT_DIR}/backup.sh"; then
  echo "!! Backup FAILED -- aborting deploy to protect production data." >&2
  echo "   (Is the postgres container up? Check: ssh ${HOST} 'cd ${REMOTE} && docker compose ps')" >&2
  exit 1
fi

# --- 2. Sync ONLY source dirs. No --delete. docker-compose.yml and .env are
#        intentionally NOT in this list, so the server's copies are untouched. ---
echo "==> Rsyncing source -> ${HOST}:${REMOTE} (whitelist, no --delete)"
SYNC_PATHS=( api ingest postgres web firmware deploy README.md )
rsync -avz \
  --exclude 'web/node_modules/' \
  --exclude 'web/.next/' \
  --exclude 'web/out/' \
  --exclude '**/__pycache__/' \
  --exclude '**/*.pyc' \
  --exclude 'firmware/*/build/' \
  --exclude 'firmware/*/secrets.h' \
  -e "ssh -i ${KEY}" \
  "${SYNC_PATHS[@]/#/${PROJECT_DIR}/}" \
  "${REMOTE_USER}@${HOST}:${REMOTE}/"

# mosquitto: push ONLY the conf file, never data/ or log/.
rsync -avz -e "ssh -i ${KEY}" \
  "${PROJECT_DIR}/mosquitto/mosquitto.conf" \
  "${REMOTE_USER}@${HOST}:${REMOTE}/mosquitto/mosquitto.conf"

# --- 3. .env: create from the example only if missing. NEVER overwrite. ---
if ! ssh_remote "test -f ${REMOTE}/.env"; then
  echo "  remote .env missing -- seeding from .env.example (edit before next deploy)"
  ssh_remote "test -f ${REMOTE}/.env.example && cp ${REMOTE}/.env.example ${REMOTE}/.env || true"
fi

# --- 4. Build + restart ONLY the app services. Stateful containers are brought
#        up if down but NEVER recreated, so their volumes are never remounted. ---
echo "==> Ensuring stateful services are up (never recreated)"
ssh_remote "cd ${REMOTE} && docker compose up -d --no-recreate postgres mosquitto"

echo "==> Building + restarting app services"
ssh_remote "cd ${REMOTE} && docker compose up -d --build ingest api web"

echo "==> Status"
ssh_remote "cd ${REMOTE} && docker compose ps"

echo "==> Recent logs"
ssh_remote "cd ${REMOTE} && docker compose logs --tail 20 ingest api web"

cat <<EOF

==> Deploy complete.

  Access via SSH tunnel:
    ssh -i ${KEY} -L 9530:localhost:9530 -L 8000:localhost:8000 ${REMOTE_USER}@${HOST}

  Then open:
    web app          http://localhost:9530
    api              http://localhost:8000

  MQTT broker is on ${HOST}:1883 (LAN) -- point ESP MQTT_HOST at the server's LAN IP.

  Malformed payloads land in the telemetry_dlq table:
    ssh ${HOST} "cd ${REMOTE} && docker compose exec postgres psql -U \${POSTGRES_USER:-sensors} -d \${POSTGRES_DB:-sensors} -c 'SELECT * FROM telemetry_dlq ORDER BY received_at DESC LIMIT 20;'"

  A pre-deploy DB backup was written to \${SENSOR_DATA_DIR}/backups on the server.
EOF
