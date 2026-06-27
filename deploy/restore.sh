#!/usr/bin/env bash
# Restore a gzipped pg_dump (from backup.sh) into the sensor-lab TimescaleDB.
#
# DESTRUCTIVE: drops and recreates the target database. Takes a fresh safety
# backup first, and wraps the load in timescaledb_pre_restore()/post_restore()
# (required to correctly reload hypertables).
#
# Usage: restore.sh <path/to/sensors_YYYYmmdd_HHMMSS.sql.gz>
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${PROJECT_DIR}"
set -a; [ -f .env ] && . ./.env; set +a
: "${POSTGRES_USER:=sensors}"
: "${POSTGRES_DB:=sensors}"

FILE="${1:?usage: restore.sh <backup.sql.gz>}"
[ -f "${FILE}" ] || { echo "no such file: ${FILE}" >&2; exit 1; }

echo "==> verifying gzip integrity of ${FILE}"
gunzip -t "${FILE}"

echo
echo "WARNING: this OVERWRITES database '${POSTGRES_DB}' on $(hostname)."
echo "         All current rows are replaced by the contents of the backup."
read -r -p "Type the database name ('${POSTGRES_DB}') to confirm: " confirm
[ "${confirm}" = "${POSTGRES_DB}" ] || { echo "aborted"; exit 1; }

echo "==> taking a pre-restore safety backup first"
"${PROJECT_DIR}/deploy/backup.sh" "${PROJECT_DIR}"

psqlc() { docker compose exec -T timescaledb psql -v ON_ERROR_STOP=1 -U "${POSTGRES_USER}" "$@"; }

echo "==> recreating empty database ${POSTGRES_DB}"
psqlc -d postgres -c "DROP DATABASE IF EXISTS ${POSTGRES_DB} WITH (FORCE);"
psqlc -d postgres -c "CREATE DATABASE ${POSTGRES_DB} OWNER ${POSTGRES_USER};"

echo "==> timescale pre-restore -> load -> post-restore"
psqlc -d "${POSTGRES_DB}" -c "CREATE EXTENSION IF NOT EXISTS timescaledb;"
psqlc -d "${POSTGRES_DB}" -c "SELECT timescaledb_pre_restore();"
gunzip -c "${FILE}" | psqlc -d "${POSTGRES_DB}"
psqlc -d "${POSTGRES_DB}" -c "SELECT timescaledb_post_restore();"

echo "==> restore complete"
psqlc -d "${POSTGRES_DB}" -c "SELECT count(*), max(ts) FROM telemetry;"
