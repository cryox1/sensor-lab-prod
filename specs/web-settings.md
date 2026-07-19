# Web — Settings (`/settings`)

**Status:** As-built
**Scope:** `web/app/settings/page.jsx` — the per-metric chart-threshold editor. How thresholds are drawn is covered in the chart pages' specs; API storage/validation in `api.md`.

## Behavior (as-built)

Source: `web/app/settings/page.jsx`, defaults in `web/app/_lib/metrics.js`,
API validation in `api/main.py` (`_clean_thresholds`).

- **Purpose:** edit the dotted reference lines ("quality lines") drawn on the
  history/overview/group charts. Lines only render on a chart when they fall
  inside the visible y-range.
- **Data:** `GET /thresholds`, SWR-polled every 30 s. The response maps
  `metric_key → [{value,label,color}, …]` for customized metrics only.
- **Editor:** one card per entry in `METRICS` (12 metrics), seeded with
  `effectiveThresholds(key, overrides)` — the saved override if present, else
  `DEFAULT_THRESHOLDS[key]` (temp, humidity, eCO₂, TVOC, AQI, battery have
  built-in defaults; heat index, pressure, gas and the three lightning
  metrics default to none). A badge shows
  "customized" (override exists, even an empty one) vs "default". The draft is
  local state, remounted/reseeded when the persisted override changes.
- **Per line:** number input (step 0.1 when the metric has decimal digits, else
  1), label text (maxLength 24), and an `<input type="color">` picker.
  `normalizeHex` expands `#abc` → `#aabbcc` and falls back to `#7d8590` for
  unparseable colors, since the color input requires 6-digit hex. Lines can be
  added (defaults: value 0, empty label, `#7d8590`) and removed; removing all
  lines and saving stores an empty override that intentionally hides all lines
  for that metric.
- **Save:** `PUT /thresholds/{metric_key}` with
  `{thresholds: [{value,label,color}]}` and `writeHeaders()`. Inline status:
  saving… / saved ✓ / save failed.
- **Reset:** "reset to default" (shown only when a default exists and the
  metric is customized) sends `DELETE /thresholds/{metric_key}`, reverting to
  `DEFAULT_THRESHOLDS`.
- **API validation** (server-side, `api/main.py`): metric key must be one of
  the 12 known `THRESHOLD_METRICS` (404 otherwise), at most 12 lines per metric
  (400), color must match `^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$` (400), labels
  are trimmed and truncated to 24 chars, and lines are stored sorted ascending
  by value (JSONB, one row per metric). Writes require the write token when
  configured.

## Requirements

None yet.

## Non-goals

- No per-device thresholds — lines are global per metric.
- No client-side validation beyond the color normalization; the API is the
  gatekeeper (the UI surfaces failures as "save failed").
- Display offsets are not managed here — they are per-device, on
  `/history/[device_id]`.

## Open questions

- None.
