# Web — GPS map (`/gps`)

**Status:** As-built
**Scope:** `web/app/gps/page.jsx` and `web/app/_components/GpsMap.jsx` — the Leaflet map of GPS-equipped sensors. GPS metric definitions live in `web/app/_lib/metrics.js` (`GPS_METRICS`).

## Behavior (as-built)

- **Client-only map:** `GpsMap` is loaded with `next/dynamic` and `ssr: false`
  because Leaflet touches `window` at import time; a "loading map…" placeholder
  shows meanwhile. Rendering uses `react-leaflet` (`MapContainer`, `TileLayer`,
  `CircleMarker`, `Polyline`, `Popup`) with OpenStreetMap tiles.
- **Data:** the same `GET /history-all?hours=<h>[&bucket_seconds=<b>]` endpoint
  as `/overview`, SWR-polled every 30 s, with the range chosen via
  `TimeRangeSelector` over the `RANGES` presets (default 24h). Device names
  from `GET /devices` (30 s) via `displayNameFor`.
- **Device selection:** no explicit GPS flag — a device qualifies by having at
  least one point with both `lat != null` and `lon != null` in the fetched
  range; all other points/devices are dropped. Colors are positional from
  `deviceColor(i)` / `DEVICE_PALETTE`, listed in a legend strip with per-device
  fix counts.
- **Map rendering** (`GpsMap.jsx`), per device: a polyline track (when > 1 fix)
  plus one `CircleMarker` per fix, the latest fix drawn larger/opaque. Each
  marker's popup shows device name, local timestamp, `lat, lon` (5 decimals),
  and any non-null `GPS_METRICS` values: altitude (m), sats, speed (km/h).
  `FitBounds` refits the viewport to all points on every data change
  (`setView` zoom 15 for a single point, `fitBounds` capped at zoom 17
  otherwise).
- **Empty state:** with zero GPS fixes in range, a hint replaces the map
  ("power on a GPS sensor and wait for a fix"). Hidden devices are *not*
  filtered out on this page.

## Requirements

None yet.

## Non-goals

- No live/WebSocket tracking — positions come only from polled history.
- No line charts for GPS fields; `GPS_METRICS` are deliberately kept out of
  `METRICS` so they don't appear on the overview/group chart pages.

## Open questions

- Should hidden devices be excluded here (they are on `/overview` by default)?
  Currently they always show if they have fixes.
