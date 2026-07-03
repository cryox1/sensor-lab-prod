# Web — Dashboard (startpage `/`)

**Status:** As-built
**Scope:** `web/app/page.jsx` plus the `SensorCard.jsx` card grid, the manage-devices drawer, and header navigation. Does not cover charts (see `web-overview.md`, `web-history.md`).

## Behavior (as-built)

Source: `web/app/page.jsx`, `web/app/_components/SensorCard.jsx`,
`web/app/_components/ManageDevicesDrawer.jsx`, `web/app/_components/NameEditor.jsx`.

- **Data sources (SWR polling):** `GET /devices` every 10 s (metadata: display
  name, hidden flag, last_seen), `GET /groups` every 10 s (group names +
  `device_ids`), `GET /latest` every 15 s (newest stored row per device — keeps
  deep-sleep nodes visible between infrequent live messages). API base resolved
  client-side via `getApiBase()` (`web/app/_lib/api.js`).
- **Live overlay:** `useLiveSocket` (`web/app/_lib/useLiveSocket.js`) connects to
  `/ws/live` with exponential-backoff reconnect (1 s → 30 s cap) and stores the
  latest message per `device_id`. **Merge rule:** the card shows the `GET /latest`
  row unless a live message is at least as new. WS `ts` is epoch *seconds*
  (number, → `ts * 1000`), REST `ts` is an ISO string (→ `Date.parse`); both are
  normalized to milliseconds before comparing (`liveMs >= prevMs` wins).
- **Layout:** one `<section>` per group (API returns groups sorted by name),
  each with an `overview →` link to `/groups/{id}`, showing only that group's
  visible members; groups with no visible members are omitted. All visible
  devices not in any group render in a trailing "Ungrouped" section (its heading
  only appears when at least one group exists). Grid: `auto-fill, minmax(260px, 1fr)`.
- **Card content** (`SensorCard.jsx`): monospace `device_id`, display name
  (from `displayNameFor`, falls back to device_id) with inline `NameEditor`
  (✎ → input → `PUT /devices/{id}/display-name {display_name}`), climate block
  (temp large, humidity, "feels like" heat index) when `temp_c != null`, air
  block (eCO₂, TVOC, AQI/5) when `eco2_ppm != null`, pressure and battery rows
  when present, `last seen` from `/devices` meta, and a `history →` link to
  `/history/{device_id}`. A corner ✕ hides the device
  (`PUT /devices/{id}/visibility {hidden: true}`) and revalidates `/devices`.
- **Hidden-device filtering:** devices with `hidden: true` are excluded from
  the grid. The device universe is the union of `/devices` ids and ids seen in
  readings, so a brand-new sensor appears from its first live message. When
  devices are hidden, a footer line ("N hidden — manage") appears; when *all*
  are hidden the empty state says so instead of "No devices yet".
- **Manage drawer** (`ManageDevicesDrawer.jsx`): right-side overlay (Escape or
  backdrop click closes), lists all devices sorted visible-first then by name,
  with per-device rename and a hide/show toggle (`PUT /devices/{id}/visibility`).
  Hidden devices still record telemetry — hiding is display-only.
- **Display offsets:** readings on cards pass through
  `applyOffset(value, offsetFor(offsets, id, metricKey))` from
  `web/app/_lib/offsets.js`. Offsets live in localStorage key `sensorOffsets`
  (`{device_id: {metric_key: number}}`), are reactive across components/tabs
  via `useSyncExternalStore`, and are presentation-only — never sent to the API.
  Pressure and battery rows are rendered without offsets.
- **Writes** send `writeHeaders()` (`X-API-Token` when
  `NEXT_PUBLIC_API_WRITE_TOKEN` is configured).
- **Header links:** manage (drawer), `/groups`, `/settings`, `/gps`, `/overview`.

## Requirements

None yet.

## Non-goals

- No historical charts or sparklines on the cards — history lives on
  `/history/[device_id]`, `/overview` and `/groups/[id]`.
- No device deletion — hiding is the only removal concept; data keeps recording.
- No server-side calibration — offsets are per-browser display state only.

## Open questions

- None.
