# Web — Groups (`/groups` and `/groups/[id]`)

**Status:** As-built
**Scope:** `web/app/groups/page.jsx` (group management + device assignment) and `web/app/groups/[id]/page.jsx` (per-group overview with live/history modes). The startpage's group sections are covered in `web-dashboard.md`.

## Behavior (as-built)

### `/groups` — management (`web/app/groups/page.jsx`)

- **Data:** `GET /groups` and `GET /devices`, SWR-polled every 15 s.
- **Create:** name input (max 64 chars, Enter or button) → `POST /groups {name}`.
- **Group list:** each row shows name, member count, `overview →` link, inline
  rename (`PUT /groups/{id} {name}`, Enter saves / Escape cancels) and delete.
  Delete asks `window.confirm` and states that the group's sensors become
  ungrouped (the API cascades members to no group), then `DELETE /groups/{id}`.
- **Assignment:** every known device (sorted by display name) gets a `<select>`
  of all groups plus "— none —". Changing it sends
  `PUT /devices/{id}/group {group_id: number|null}` and revalidates both keys.
  A device belongs to at most one group.
- **Writes** use `writeHeaders()` from `web/app/_lib/api.js` — adds
  `X-API-Token` when `NEXT_PUBLIC_API_WRITE_TOKEN` is set at build time.

### `/groups/[id]` — group overview (`web/app/groups/[id]/page.jsx`)

- Structure mirrors `/overview` (per-`METRICS` SVG charts via `Chart.jsx`,
  positional `deviceColor` palette shared by legend and charts, "quality lines"
  toggle over `GET /thresholds` + `DEFAULT_THRESHOLDS`, include-hidden checkbox
  scoped to the group's members, client display offsets applied at render) but
  restricted to the group's `device_ids` and adds a **mode toggle**:
  - **Live (default):** `useLiveSocket` keeps a rolling 5-minute buffer of raw
    readings per device (max 5000 points/device). WS `ts` is epoch seconds;
    `tsToMillis` multiplies numbers by 1000 (ISO strings go through
    `Date.parse`). The buffer holds *all* devices (the socket callback closes
    over state before `/groups` loads) and is filtered to group members at
    render. A 1 s tick slides the window so stale points scroll off. X axis at
    second resolution; headings show a green "● live" badge and a footer counts
    live readings.
  - **History:** `GET /history-all?hours&bucket_seconds` per the `RANGES`
    preset (default 24h, 30 s polling; not fetched while live), filtered to the
    group's `device_ids`; `TimeRangeSelector` only shows in this mode.
  - The socket stays connected in both modes so switching back to live shows
    data immediately.
- Metadata: `GET /devices`, `GET /groups`, `GET /thresholds` every 30 s. If the
  groups list loads but contains no matching id, a "group not found" state
  links back to `/groups`; an empty group prompts to assign sensors.

## Requirements

### R1 — Drag & drop assignment board (Proposed)

The default view of `/groups` becomes a board: one droppable container per
group plus an "ungrouped" zone; each sensor is a draggable chip showing its
display name and monospace device_id. A small toggle switches to the classic
dropdown list view.

Acceptance criteria:

- **Given** the board view, **when** a sensor chip is dropped onto a group
  container, **then** exactly one `PUT /devices/{id}/group {group_id}` is sent
  (with write headers) and the chip appears in that container immediately
  (optimistic), surviving the next SWR refetch.
- **Given** a chip dropped on the "ungrouped" zone, **then** the request body
  is `{group_id: null}`.
- **Given** a chip dropped on its current container, outside any droppable, or
  the drag is cancelled (Escape), **then** no request is sent.
- **Given** the PUT fails (non-2xx or network error), **then** the optimistic
  move is reverted to server state and an error message is shown.
- Create, rename, and delete group work unchanged in both views; buttons/links
  inside group containers remain clickable (drag activation requires ~5px
  pointer movement).
- Drag & drop works with touch input (pointer sensor).
- The view choice (board/list) persists in localStorage (`groupsView`) across
  reloads; board is the default.
- An empty group renders a drop placeholder and remains a valid drop target.

### R2 — CSV export on the group overview (Implemented)

Same behavior as web-overview R1, scoped to the group page `/groups/[id]`.

Acceptance criteria:

- **Given** history mode, **when** "export CSV" is clicked, **then** the export
  contains exactly the group's displayed dataset for the selected range (same
  format rules as web-overview R1).
- **Given** live mode, **then** the export contains the current rolling live
  buffer, with `ts` correctly converted from epoch-seconds-based buffer entries
  to ISO-8601 UTC (a 1970 timestamp is a bug).
- The filename is `sensorlab_<group-name-slug>_<range|live-5min>_<YYYY-MM-DD>.csv`.
- **Given** no points, **then** the button is disabled.

## Non-goals

- No nested groups or multi-group membership — the data model is strictly one
  group per device.
- No group-level settings (thresholds, colors) — thresholds are global
  per-metric (`/settings`), colors are positional.

## Open questions

- None.
