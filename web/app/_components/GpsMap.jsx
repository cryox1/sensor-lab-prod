"use client";
import { Fragment, useEffect, useMemo, useRef } from "react";
import {
  Circle,
  MapContainer,
  TileLayer,
  CircleMarker,
  Polyline,
  Popup,
  useMap,
} from "react-leaflet";
import "leaflet/dist/leaflet.css";
import { GPS_METRICS, formatValue } from "../_lib/metrics";

// Fit the view to all plotted sensor points whenever they change. Implemented
// as a child of MapContainer so it can grab the Leaflet map instance via
// useMap(). Strikes are deliberately excluded from `bounds` — fitting to a
// 250-km lightning ring would zoom the sensors out of sight. `fallback`
// (home) only centers the view when no sensor has a fix at all.
function FitBounds({ bounds, fallback }) {
  const map = useMap();
  // Apply the fallback only once — `bounds` gets a fresh identity on every
  // poll, and re-centering a sensor-less map every 30 s would fight the user.
  const fellBack = useRef(false);
  useEffect(() => {
    if (bounds.length === 1) {
      map.setView(bounds[0], 15);
    } else if (bounds.length > 1) {
      map.fitBounds(bounds, { padding: [30, 30], maxZoom: 17 });
    } else if (fallback && !fellBack.current) {
      fellBack.current = true;
      map.setView(fallback, 7);
    }
  }, [map, bounds, fallback]);
  return null;
}

// react-leaflet does not update a TileLayer's `attribution` prop after mount,
// so the Blitzortung credit (required by their usage terms) is added and
// removed through the map's attribution control instead.
function BlitzAttribution() {
  const map = useMap();
  useEffect(() => {
    const text =
      'Blitzdaten: <a href="https://www.blitzortung.org">Blitzortung.org</a>';
    map.attributionControl?.addAttribution(text);
    return () => map.attributionControl?.removeAttribution(text);
  }, [map]);
  return null;
}

function PointPopup({ name, point }) {
  return (
    <Popup>
      <div style={{ fontSize: 12, lineHeight: 1.5, color: "#111" }}>
        <strong>{name}</strong>
        <br />
        {new Date(point._t).toLocaleString()}
        <br />
        {point.lat.toFixed(5)}, {point.lon.toFixed(5)}
        {GPS_METRICS.map((m) =>
          point[m.key] != null ? (
            <span key={m.key}>
              <br />
              {m.label}: {formatValue(m, point[m.key]).trim()}
            </span>
          ) : null
        )}
      </div>
    </Popup>
  );
}

// devices: [{ device_id, name, color, points: [{ lat, lon, _t, alt_m, sats, speed_kmh }] }]
// points are assumed sorted ascending by time. Renders, per device, a track line
// plus a marker per fix, with the latest fix emphasized.
// strikes: [{ lat, lon, _t, distance_km }] — Blitzortung.org lightning, drawn
// with an age fade. home/radiusKm draw the coverage circle around the
// configured home location; home also centers the map when no sensor has a fix.
export default function GpsMap({
  devices,
  strikes = [],
  home = null,
  radiusKm = null,
}) {
  const bounds = useMemo(() => {
    const b = [];
    for (const d of devices) for (const p of d.points) b.push([p.lat, p.lon]);
    return b;
  }, [devices]);

  // Stable identity while lat/lon are unchanged, so FitBounds' effect doesn't
  // re-fire (and reset the user's pan/zoom) on every SWR refresh.
  const homeCenter = useMemo(
    () => (home ? [home.lat, home.lon] : null),
    [home?.lat, home?.lon]
  );

  const center = bounds.length ? bounds[bounds.length - 1] : homeCenter ?? [0, 0];

  return (
    <MapContainer
      center={center}
      zoom={13}
      scrollWheelZoom
      style={{ height: 520, width: "100%", borderRadius: 8 }}
    >
      <TileLayer
        attribution='&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
        url="https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png"
      />
      {devices.map((d) => {
        const latLngs = d.points.map((p) => [p.lat, p.lon]);
        const last = d.points.length - 1;
        return (
          <Fragment key={d.device_id}>
            {latLngs.length > 1 && (
              <Polyline
                positions={latLngs}
                pathOptions={{ color: d.color, weight: 2, opacity: 0.6 }}
              />
            )}
            {d.points.map((p, i) => (
              <CircleMarker
                key={p._t}
                center={[p.lat, p.lon]}
                radius={i === last ? 7 : 4}
                pathOptions={{
                  color: d.color,
                  weight: i === last ? 2 : 1,
                  fillColor: d.color,
                  fillOpacity: i === last ? 0.9 : 0.5,
                }}
              >
                <PointPopup name={d.name} point={p} />
              </CircleMarker>
            ))}
          </Fragment>
        );
      })}
      {homeCenter && radiusKm != null && (
        <Circle
          center={homeCenter}
          radius={radiusKm * 1000}
          pathOptions={{
            color: "#58a6ff",
            weight: 1,
            opacity: 0.35,
            fillOpacity: 0.03,
            dashArray: "4 6",
          }}
        />
      )}
      {strikes.map((s) => {
        const ageMin = (Date.now() - s._t) / 60000;
        return (
          <CircleMarker
            key={`${s._t}|${s.lat}|${s.lon}`}
            center={[s.lat, s.lon]}
            radius={ageMin < 5 ? 7 : ageMin < 20 ? 5 : 4}
            pathOptions={{
              color: "#e3b341",
              weight: 1,
              fillColor: "#f0c000",
              fillOpacity: Math.max(0.15, 0.9 - (ageMin / 60) * 0.75),
              opacity: Math.max(0.25, 1 - ageMin / 60),
            }}
          >
            <Popup>
              <div style={{ fontSize: 12, lineHeight: 1.5, color: "#111" }}>
                <strong>⚡ lightning strike</strong>
                <br />
                {new Date(s._t).toLocaleString()}
                <br />
                {s.lat.toFixed(4)}, {s.lon.toFixed(4)}
                {s.distance_km != null && (
                  <>
                    <br />
                    {s.distance_km} km from home
                  </>
                )}
                <br />
                <span style={{ opacity: 0.7 }}>Blitzortung.org</span>
              </div>
            </Popup>
          </CircleMarker>
        );
      })}
      {home != null && <BlitzAttribution />}
      <FitBounds bounds={bounds} fallback={homeCenter} />
    </MapContainer>
  );
}
