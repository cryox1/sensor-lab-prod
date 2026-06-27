"use client";
import { Fragment, useEffect, useMemo } from "react";
import {
  MapContainer,
  TileLayer,
  CircleMarker,
  Polyline,
  Popup,
  useMap,
} from "react-leaflet";
import "leaflet/dist/leaflet.css";
import { GPS_METRICS, formatValue } from "../_lib/metrics";

// Fit the view to all plotted points whenever they change. Implemented as a
// child of MapContainer so it can grab the Leaflet map instance via useMap().
function FitBounds({ bounds }) {
  const map = useMap();
  useEffect(() => {
    if (bounds.length === 1) {
      map.setView(bounds[0], 15);
    } else if (bounds.length > 1) {
      map.fitBounds(bounds, { padding: [30, 30], maxZoom: 17 });
    }
  }, [map, bounds]);
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
export default function GpsMap({ devices }) {
  const bounds = useMemo(() => {
    const b = [];
    for (const d of devices) for (const p of d.points) b.push([p.lat, p.lon]);
    return b;
  }, [devices]);

  const center = bounds.length ? bounds[bounds.length - 1] : [0, 0];

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
      <FitBounds bounds={bounds} />
    </MapContainer>
  );
}
