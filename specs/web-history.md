# Web — Device history (`/history/[device_id]`)

**Status:** As-built
**Scope:** `web/app/history/[device_id]/page.jsx` — single-device charts with live/history modes, per-metric stats, and the per-device offset editor. Multi-device charts are `web-overview.md` / `web-groups.md`.

## Behavior (as-built)

Source: `web/app/history/[device_id]/page.jsx`,
`web/app/_components/{Chart,Stats,ModeToggle,TimeRangeSelector,OffsetSettings,InfoTooltip}.jsx`.

- **Header:** back link, device display name as title (from `GET /devices`,
  fallback to the raw `device_id`), subtitle showing the id (when aliased) and
  the current mode ("live — last 5 min" or "history — <range>").
- **Mode toggle** (`ModeToggle.jsx`), default **live**:
  - **Live:** `useLiveSocket` collects raw `/ws/live` messages filtered to this
    `device_id` into a rolling 5-minute buffer (max 5000 points). `tsToMillis`
    normalizes WS epoch-seconds numbers (`* 1000`) and ISO strings
    (`Date.parse`) to ms. A 1 s tick keeps the window sliding when the sensor
    goes quiet. The socket stays connected in both modes.
  - **History:** `GET /history?device_id=<id>&hours=<h>[&bucket_seconds=<b>]`
    per the `RANGES` preset (default 24h), SWR-polled every 30 s; only fetched
    in history mode. `TimeRangeSelector` shows only in this mode.
- **Metric sections:** only `METRICS` with at least one non-null value in the
  current point set render ("active metrics"). Each section has:
  - heading with unit, `InfoTooltip` description; in live mode the latest
    non-null value (offset-applied) as a bold current readout plus a "● live"
    badge; a yellow `offset +x` badge whenever a display offset is active;
  - `Stats` chips (current / avg / min / max / samples) computed over the
    offset-adjusted values;
  - a single-series `Chart.jsx` SVG line chart in the metric's own color, with
    threshold lines from `effectiveThresholds(key, GET /thresholds)` and the
    "quality lines" checkbox (shown only if any active metric has lines).
- **Offsets editor:** a "⚙ offsets" button toggles `OffsetSettings` — one
  number input per active metric writing to the localStorage `sensorOffsets`
  store (`web/app/_lib/offsets.js`) via `setOffset`, plus "reset all"
  (`clearDeviceOffsets`). Changes apply live to this page's charts/stats and to
  the dashboard cards. Display-only; recorded data never changes.
- **X labels:** second resolution in live mode, `HH:MM` for ranges ≤ 24h,
  `MM/DD HH` for longer ranges. A footer states the point count.

## Requirements

None yet.

## Non-goals

- No comparison of multiple devices on one chart — that is `/overview` and
  `/groups/[id]`.
- No editing of device metadata here (rename/hide live on the dashboard).

## Open questions

- None.
