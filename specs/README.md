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

Out of scope: `firmware/` (independent lifecycle; has its own ROADMAP), deploy
tooling (governed by the mandatory rules in [`../AGENTS.md`](../AGENTS.md)).

## Deployment note

`specs/` is deliberately **not** in the `SYNC_PATHS` whitelist of
`deploy/deploy.sh`. Specs are developer documentation with no runtime role on
the server — same treatment as `AGENTS.md`/`CLAUDE.md`. Keep the deploy
whitelist as small as possible (AGENTS.md rule 9).
