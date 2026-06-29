#!/usr/bin/env bash
# Back up the sensor-lab PostgreSQL database to a gzipped pg_dump.
#
# Writes to ${SENSOR_DATA_DIR}/backups (the big disk on the server,
# ./data/backups locally), keeps the newest BACKUP_KEEP, and FAILS HARD if
# pg_dump errors or produces a suspiciously small file -- so a caller (e.g.
# deploy.sh) can abort a deploy when the backup didn't actually work.
#
# Usage: backup.sh [PROJECT_DIR]   (PROJECT_DIR defaults to this script's parent)
set -euo pipefail

PROJECT_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "${PROJECT_DIR}"

# Load DB creds + SENSOR_DATA_DIR from the project .env (if present).
set -a; [ -f .env ] && . ./.env; set +a
: "${POSTGRES_USER:=sensors}"
: "${POSTGRES_DB:=sensors}"
DATA_DIR="${SENSOR_DATA_DIR:-./data}"
BACKUP_DIR="${DATA_DIR}/backups"
KEEP="${BACKUP_KEEP:-14}"

mkdir -p "${BACKUP_DIR}"
TS="$(date +%Y%m%d_%H%M%S)"
OUT="${BACKUP_DIR}/sensors_${TS}.sql.gz"

echo "==> pg_dump ${POSTGRES_DB} -> ${OUT}"
# -T: no TTY. Stream the dump out of the container and gzip on the host.
# pipefail makes a failed pg_dump fail the whole pipeline.
# `</dev/null`: give the exec its own stdin so it can't swallow the rest of this
# script when backup.sh is itself fed to `bash -s` over ssh (deploy.sh does that).
if ! docker compose exec -T postgres \
      pg_dump -U "${POSTGRES_USER}" "${POSTGRES_DB}" </dev/null | gzip > "${OUT}.tmp"; then
  echo "!! pg_dump failed -- no backup written" >&2
  rm -f "${OUT}.tmp"
  exit 1
fi

# Reject a truncated/garbage dump (e.g. DB container down) before publishing it.
SIZE="$(stat -c%s "${OUT}.tmp" 2>/dev/null || stat -f%z "${OUT}.tmp")"
if [ "${SIZE}" -lt 200 ]; then
  echo "!! dump suspiciously small (${SIZE} bytes) -- aborting" >&2
  rm -f "${OUT}.tmp"
  exit 1
fi

mv "${OUT}.tmp" "${OUT}"          # atomic publish: only a complete dump gets the final name
echo "==> wrote $(du -h "${OUT}" | cut -f1) to ${OUT}"

# Prune: keep the newest KEEP, delete older ones.
ls -1t "${BACKUP_DIR}"/sensors_*.sql.gz 2>/dev/null | tail -n +"$((KEEP + 1))" | xargs -r rm -f
echo "==> retained newest ${KEEP} backups in ${BACKUP_DIR}"
