# Specs — spec-driven development for sensor-lab

This directory is the source of truth for *intended* behavior. Code shows what
the system happens to do; the spec says what it is *supposed* to do. Every area
of the stack (each web page, the API, the ingest pipeline) has one spec file.

Language: English, matching the codebase and README. (German was considered;
English won for consistency — code comments and UI strings are English too.)

## The workflow

1. **Spec first.** Before implementing a new feature or changing behavior, add
   a numbered requirement (`R1`, `R2`, …) to the relevant area spec, with
   Given/When/Then acceptance criteria. Status: `Proposed`.
2. **Review.** Agree on the requirement before writing code (human review, or
   plan approval when an agent does the work).
3. **Implement.** Build exactly what the requirement says. If reality forces a
   deviation, update the spec in the same commit — the spec never lies.
4. **Verify.** Walk the acceptance criteria one by one against the running
   system (manual checklist; there is no automated test suite).
5. **Flip status.** `Proposed → Implemented` in the same commit as the code.

Changing *existing* behavior follows the same loop: edit the `Behavior
(as-built)` section and/or the affected requirement in the same commit as the
code change. A PR that changes behavior without touching its spec is
incomplete.

## Files

| Spec | Covers |
|---|---|
| [`TEMPLATE.md`](TEMPLATE.md) | Skeleton for new spec files |
| [`web-dashboard.md`](web-dashboard.md) | Startpage `/` — sensor card grid, live updates, manage drawer |
| [`web-overview.md`](web-overview.md) | `/overview` — all-sensor charts, time ranges, CSV export |
| [`web-groups.md`](web-groups.md) | `/groups` management + `/groups/[id]` group overview, drag & drop board, CSV export |
| [`web-history.md`](web-history.md) | `/history/[device_id]` — single-device charts, live/history modes |
| [`web-gps.md`](web-gps.md) | `/gps` — Leaflet map of GPS-equipped sensors |
| [`web-settings.md`](web-settings.md) | `/settings` — metric threshold editor |
| [`api.md`](api.md) | FastAPI service — every route, auth, WebSocket, timestamp shapes |
| [`ingest.md`](ingest.md) | MQTT → PostgreSQL writer, schema migrations, DLQ |
| [`hw-firebeetle2-c6.md`](hw-firebeetle2-c6.md) | FireBeetle 2 ESP32-C6 (DFR1075) — the standard node board + deep-sleep skeleton |
| [`hw-bme280.md`](hw-bme280.md) | `BME280_fbc6` node — GY-BME280 reference sensor of the comparison pair |
| [`hw-bme680.md`](hw-bme680.md) | `BME680_fbc6` node — Gravity BME680, `gas_kohm` end-to-end |
| [`hw-ens160.md`](hw-ens160.md) | `air03` node — ENS160+AHT21 always-on air node on FireBeetle 2 C6 |
| [`hw-as3935.md`](hw-as3935.md) | `storm01` node — BME280 + AS3935 lightning sensor, solar-powered (Proposed) |

Out of scope: legacy firmware sketch internals (independent lifecycle; the
XIAO C6 has its own ROADMAP) — but the standard node board and its sensor
nodes are covered by the `hw-*` specs above — and deploy tooling (governed by
the mandatory rules in [`../AGENTS.md`](../AGENTS.md)).

## Deployment note

`specs/` is deliberately **not** in the `SYNC_PATHS` whitelist of
`deploy/deploy.sh`. Specs are developer documentation with no runtime role on
the server — same treatment as `AGENTS.md`/`CLAUDE.md`. Keep the deploy
whitelist as small as possible (AGENTS.md rule 9).
