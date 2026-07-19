# Web — Overview (`/overview`)

**Status:** As-built
**Scope:** `web/app/overview/page.jsx` — the all-sensor multi-device charts page. Group-scoped overview is `web-groups.md`; single-device history is `web-history.md`.

## Behavior (as-built)

Source: `web/app/overview/page.jsx`, `web/app/_components/Chart.jsx`,
`web/app/_components/TimeRangeSelector.jsx`, `web/app/_lib/metrics.js`.

- **Data:** `GET /history-all?hours=<h>[&bucket_seconds=<b>]`, SWR-polled every
  30 s. Range comes from the `RANGES` presets in `metrics.js`
  (1h raw, 6h/60 s, 24h/300 s, 7d/1800 s, 30d/7200 s buckets); default 24h.
  Device metadata from `GET /devices` and threshold overrides from
  `GET /thresholds`, both polled every 30 s.
- **Charts:** one section per entry in `METRICS` (temp, humidity, heat index,
  eCO₂, TVOC, AQI, battery, pressure, gas, storm distance, strike energy,
  strikes), each a hand-rolled SVG line chart
  (`Chart.jsx`, no chart library): 900×260 viewBox, 5 y/x gridline ticks, gapped
  paths on null values, hover crosshair snapping to the nearest data x with a
  multi-series tooltip. A metric's chart is skipped entirely when no device has
  a non-null value for it. Each metric heading has an `InfoTooltip` with the
  metric description.
- **Series/colors:** one series per device; colors come from `deviceColor(i)`
  over the 10-entry `DEVICE_PALETTE`, assigned by position in the filtered
  device list, so a device keeps its color across all metric charts on the page.
  A legend strip above the charts lists each device in its color. Device labels
  use `display_name` (fallback device_id).
- **Hidden devices:** excluded by default; when any exist, an
  "include N hidden devices" checkbox adds them back (this reindexes palette
  colors, since colors are positional).
- **Thresholds:** dotted reference lines per metric from
  `effectiveThresholds(key, overrides)` — a `GET /thresholds` override wins over
  `DEFAULT_THRESHOLDS`; an empty-array override hides all lines. A "quality
  lines" checkbox (default on) toggles rendering. `Chart.jsx` only draws lines
  that fall inside the current y-range (they never stretch the axis).
- **Offsets:** client-side display offsets (`web/app/_lib/offsets.js`,
  localStorage `sensorOffsets`) are applied per device/metric to every plotted
  point via `applyOffset`. Presentation-only; the fetched data is unmodified.
- **X labels:** time-only (`HH:MM`) for ranges ≤ 24h, `MM/DD HH` beyond.

## Requirements

### R1 — CSV export of the displayed dataset (Implemented)

An "export CSV" button in the controls row downloads the currently displayed
dataset as a CSV file, generated client-side.

Acceptance criteria:

- **Given** history data is displayed for the selected range/bucket, **when**
  "export CSV" is clicked, **then** a file downloads containing exactly the
  displayed dataset in long format: header
  `ts,device_id,device_name,<metric columns>`, one row per (device, point),
  sorted by time within each device.
- **Given** the export, **then** `ts` is ISO-8601 UTC, fields are RFC-4180
  escaped (quotes doubled, fields containing `,`/`"`/newline quoted), and only
  columns with at least one non-null value across the dataset appear.
- **Given** the hidden-device toggle, **then** the export contains exactly the
  devices currently shown (hidden devices excluded unless the toggle includes
  them).
- **Given** no data points are loaded, **then** the button is disabled.
- The filename is `sensorlab_overview_<range>_<YYYY-MM-DD>.csv` (slugified).
- Client-side display offsets (offsets.js) are NOT applied to exported values —
  the export contains raw stored values. This is a deliberate decision.

## Non-goals

- No live/WebSocket mode on this page — it is history-only (live is on
  `/groups/[id]` and `/history/[device_id]`).
- No per-device filtering beyond the hidden toggle, and no zoom/pan on charts.

## Open questions

- None.
