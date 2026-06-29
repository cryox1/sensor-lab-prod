# Agent rules — sensor-lab

This stack collects real sensor data continuously. **The production database
has already been wiped once** by a deploy that overwrote the server's compose and
`rsync --delete`'d the tree. The rules below exist so that never happens again.
They are mandatory for any automated agent or human operator.

The stack is fully open-source: MQTT (Mosquitto) → `ingest` → **plain PostgreSQL**.
It previously ran through Redpanda (Kafka) into TimescaleDB; both were removed. Some
historical names below (`tsdb`, `redpanda`) survive only as the pre-migration data
dirs kept around for rollback.

> **Setting this up for the first time?** See `DEPLOYMENT.md` — it walks an agent
> through the questions to ask and the values to fill in for a working deployment.
> The placeholders below (`$SENSOR_LAB_HOST`, `SENSOR_DATA_DIR`, …) are explained there.

## Where the data lives (know this before touching deploys)

- **Your deploy server:** referred to here as `$SENSOR_LAB_HOST`, with the project
  checked out at `$SENSOR_LAB_REMOTE_DIR` (e.g. `/home/youruser/sensor-lab`) and compose
  project name `sensor-lab`.
- **The live database is the bind mount `${SENSOR_DATA_DIR}/postgres`** (e.g.
  `/mnt/data/sensor-lab/postgres` on a server), mounted at `/var/lib/postgresql/data` in
  the `sensor-postgres` container (compose service `postgres`, image `postgres:16`).
  Mosquitto → `${SENSOR_DATA_DIR}/mosquitto/{data,log}`. **Always verify the server's
  actual layout before assuming anything:** `ssh "$SENSOR_LAB_HOST" 'cd "$SENSOR_LAB_REMOTE_DIR" && docker compose config'`
  and `ssh "$SENSOR_LAB_HOST" 'docker inspect sensor-postgres --format "{{range .Mounts}}{{.Source}} -> {{.Destination}}{{println}}{{end}}"'`.
- **If your server also runs another, UNRELATED PostgreSQL**, sensor-lab's Postgres is a
  dedicated container, published on a non-default host port (`POSTGRES_HOST_PORT`,
  default `5433`) with its own data dir — isolated from any other instance. **NEVER**
  point `backup.sh`/`restore.sh`, a `psql`, or any DROP at that other PostgreSQL; the
  scripts only ever `docker compose exec postgres` within sensor-lab. Confirm the host
  port is free before deploy.
- **Treat any other on-disk data location as off-limits.** If you migrated from the old
  Redpanda/TimescaleDB stack, the **pre-migration data dirs** at
  `${SENSOR_DATA_DIR}/{tsdb,redpanda}` are the old TimescaleDB + Redpanda volumes, kept as
  rollback. They are **NOT** the live data — do not point the running stack at them, and
  do not delete them without a deliberate, backed-up decision. The same goes for any
  leftover named volume or previous bind-mount path: remounting the DB onto a
  different/empty location is exactly what causes a wipe.
- `SENSOR_DATA_DIR` also drives backups: `backup.sh` writes to `${SENSOR_DATA_DIR}/backups`
  — keep that on the big disk, never the root fs.

## Data-safety rules (do not violate)

1. **NEVER** run `docker compose down -v`, `docker volume rm`, or `docker volume prune`
   against this stack. `-v` / volume removal destroys the database.
2. **NEVER** `docker compose down` the whole stack on the server. The stateful
   services (`postgres`, `mosquitto`) must stay up across deploys so their data dirs
   are never remounted.
3. **NEVER overwrite the server's `docker-compose.yml` or `.env`.** `deploy.sh` does
   not sync them — keep it that way. Renaming the data dir, or switching a stateful
   service from its bind mount to a named volume (or vice-versa), remounts the DB
   onto a different/empty location → **wipe**.
4. **NEVER `rsync --delete` against the server tree.** The server may hold files/dirs
   that don't exist in this repo; `--delete` would erase them.
5. **ALWAYS back up before any deploy or DB-touching operation:** `./deploy/backup.sh`.
   Backups land in `${SENSOR_DATA_DIR}/backups` (on a server, point `SENSOR_DATA_DIR`
   at the big disk — never the root fs).
6. The **only supported deploy** is `./deploy/deploy.sh`. It backs up first (and
   aborts if the backup fails), syncs only source dirs with no `--delete`, never
   touches compose/`.env`/data, and rebuilds only the app services
   (`ingest`, `api`, `web`), leaving `postgres` / `mosquitto` running.
7. **Restore only via `./deploy/restore.sh <file>`.** It takes a fresh safety backup,
   drops/recreates only the sensor-lab `postgres` container's DB, and loads the plain
   `pg_dump`. Do not hand-roll a restore, and never run it against any other PostgreSQL.
8. **If you must change the data-dir/bind layout**, first capture the server's *actual*
   layout (rule “Where the data lives”), then migrate PGDATA deliberately with a fresh
   backup in hand. Do not let a `docker-compose.yml` edit silently remount the live DB.
9. `deploy.sh` syncs a **whitelist** of source dirs (`SYNC_PATHS`). A genuinely new
   top-level source dir must be added there or it won't deploy.
10. Local dev uses `./data` (the `SENSOR_DATA_DIR` default if you set one). Only
    `./data` is safe to delete, and only locally.

## Schema changes

`postgres/init.sql` only runs on a fresh/empty data dir. On an existing DB, add new
columns to **both** `init.sql` and the `ingest` startup `ALTER TABLE`s (idempotent
`ADD COLUMN IF NOT EXISTS`); `ingest` re-applies them on restart. Restart
`ingest` before `api` so columns exist before the API queries them.
