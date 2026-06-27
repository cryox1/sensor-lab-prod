# Agent rules — sensor-lab

This stack collects real sensor data continuously. **The production TimescaleDB
has already been wiped once** by a deploy that overwrote the server's compose and
`rsync --delete`'d the tree. The rules below exist so that never happens again.
They are mandatory for any automated agent or human operator.

> **Setting this up for the first time?** See `DEPLOYMENT.md` — it walks an agent
> through the questions to ask and the values to fill in for a working deployment.
> The placeholders below (`$SENSOR_LAB_HOST`, `SENSOR_DATA_DIR`, …) are explained there.

## Where the data lives (know this before touching deploys)

- **Your deploy server:** referred to here as `$SENSOR_LAB_HOST`, with the project
  checked out at `$SENSOR_LAB_REMOTE_DIR` (e.g. `/home/youruser/sensor-lab`) and compose
  project name `sensor-lab`.
- **The live data is whatever `SENSOR_DATA_DIR` points at.** This repo's compose binds
  the stateful data under `${SENSOR_DATA_DIR}` (default `./data` locally; a roomy disk
  such as `/mnt/data/sensor-lab` on a server): `${SENSOR_DATA_DIR}/tsdb` (TimescaleDB,
  container `sensor-tsdb`), `${SENSOR_DATA_DIR}/redpanda`, and
  `${SENSOR_DATA_DIR}/mosquitto/{data,log}`. Verify the *actual* layout before assuming
  anything: `ssh "$SENSOR_LAB_HOST" 'cd "$SENSOR_LAB_REMOTE_DIR" && docker compose config'`
  and `ssh "$SENSOR_LAB_HOST" 'docker inspect sensor-tsdb --format "{{range .Mounts}}{{.Source}} -> {{.Destination}}{{println}}{{end}}"'`.
- **Treat any other on-disk data location as off-limits.** If a server has leftover data
  from an earlier layout (a different `SENSOR_DATA_DIR`, an old named volume, a previous
  bind-mount path), it is **NOT** the live data — do not point the running stack at it
  without a deliberate, backed-up migration. Remounting the DB onto a different/empty
  location is exactly what causes a wipe.
- `SENSOR_DATA_DIR` also drives backups: `backup.sh` writes to `${SENSOR_DATA_DIR}/backups`
  — keep that on the big disk, never the root fs.

## Data-safety rules (do not violate)

1. **NEVER** run `docker compose down -v`, `docker volume rm`, or `docker volume prune`
   against this stack. `-v` / volume removal destroys the database.
2. **NEVER** `docker compose down` the whole stack on the server. The stateful
   services (`timescaledb`, `redpanda`, `mosquitto`) must stay up across deploys so
   their volumes are never remounted.
3. **NEVER overwrite the server's `docker-compose.yml` or `.env`.** `deploy.sh` does
   not sync them — keep it that way. Renaming a volume, or switching a stateful
   service from its named volume to a bind mount (or vice-versa), remounts the DB
   onto a different/empty location → **wipe**.
4. **NEVER `rsync --delete` against the server tree.** The server may hold files/dirs
   that don't exist in this repo; `--delete` would erase them.
5. **ALWAYS back up before any deploy or DB-touching operation:** `./deploy/backup.sh`.
   Backups land in `${SENSOR_DATA_DIR}/backups` (on a server, point `SENSOR_DATA_DIR`
   at the big disk — never the root fs).
6. The **only supported deploy** is `./deploy/deploy.sh`. It backs up first (and
   aborts if the backup fails), syncs only source dirs with no `--delete`, never
   touches compose/`.env`/data, and rebuilds only the app services
   (`bridge`, `tsdb-writer`, `api`, `web`, `redpanda-console`).
7. **Restore only via `./deploy/restore.sh <file>`.** TimescaleDB hypertables need
   `timescaledb_pre_restore()` / `timescaledb_post_restore()` around the load — the
   script handles it; do not hand-roll a `psql <` restore.
8. **If you must change the volume/bind layout**, first capture the server's *actual*
   layout (rule “Where the data lives”), then migrate PGDATA deliberately with a fresh
   backup in hand. Do not let a `docker-compose.yml` edit silently remount the live DB.
9. `deploy.sh` syncs a **whitelist** of source dirs (`SYNC_PATHS`). A genuinely new
   top-level source dir must be added there or it won't deploy.
10. Local dev uses `./data` (the `SENSOR_DATA_DIR` default if you set one). Only
    `./data` is safe to delete, and only locally.

## Schema changes

`timescaledb/init.sql` only runs on a fresh/empty volume. On an existing DB, add new
columns to **both** `init.sql` and the `tsdb-writer` startup `ALTER TABLE`s (idempotent
`ADD COLUMN IF NOT EXISTS`); the writer re-applies them on restart. Restart
`tsdb-writer` before `api` so columns exist before the API queries them.
