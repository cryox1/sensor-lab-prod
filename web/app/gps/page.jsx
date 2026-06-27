"use client";
import dynamic from "next/dynamic";
import { useEffect, useMemo, useState } from "react";
import useSWR from "swr";
import TimeRangeSelector from "../_components/TimeRangeSelector";
import { getApiBase } from "../_lib/api";
import { displayNameFor } from "../_lib/displayName";
import { RANGES, deviceColor } from "../_lib/metrics";

// Leaflet touches `window` at import time, so the map must not be server-rendered.
const GpsMap = dynamic(() => import("../_components/GpsMap"), {
  ssr: false,
  loading: () => <p style={{ opacity: 0.7 }}>loading map…</p>,
});

const fetcher = (url) => fetch(url).then((r) => r.json());

export default function GpsPage() {
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

  const [range, setRange] = useState(RANGES[2]); // 24h

  const url = api
    ? `${api}/history-all?hours=${range.hours}${
        range.bucketSeconds ? `&bucket_seconds=${range.bucketSeconds}` : ""
      }`
    : null;
  const { data, isLoading } = useSWR(url, fetcher, { refreshInterval: 30000 });
  const { data: deviceMeta } = useSWR(
    api ? `${api}/devices` : null,
    fetcher,
    { refreshInterval: 30000 }
  );

  const nameByDeviceId = useMemo(() => {
    const map = {};
    for (const d of deviceMeta ?? []) map[d.device_id] = displayNameFor(d);
    return map;
  }, [deviceMeta]);

  // Keep only devices that reported a location; that naturally selects GPS sensors.
  const devices = useMemo(() => {
    if (!data) return [];
    return data
      .map((d) => ({
        device_id: d.device_id,
        points: d.points
          .filter((p) => p.lat != null && p.lon != null)
          .map((p) => ({ ...p, _t: new Date(p.ts).getTime() })),
      }))
      .filter((d) => d.points.length > 0)
      .map((d, i) => ({
        ...d,
        name: nameByDeviceId[d.device_id] || d.device_id,
        color: deviceColor(i),
      }));
  }, [data, nameByDeviceId]);

  const totalPoints = devices.reduce((n, d) => n + d.points.length, 0);

  return (
    <main style={{ padding: "32px", maxWidth: 1100, margin: "0 auto" }}>
      <a href="/" style={{ color: "#58a6ff", fontSize: 13 }}>
        ← back
      </a>
      <div
        style={{
          display: "flex",
          alignItems: "baseline",
          justifyContent: "space-between",
          marginTop: 8,
          flexWrap: "wrap",
          gap: 16,
        }}
      >
        <div>
          <h1 style={{ margin: 0 }}>gps map</h1>
          <p style={{ opacity: 0.6, marginTop: 4, marginBottom: 0 }}>
            GPS sensors — {range.label}
          </p>
        </div>
        <TimeRangeSelector value={range.label} onChange={setRange} />
      </div>

      {devices.length > 0 && (
        <div
          style={{
            display: "flex",
            flexWrap: "wrap",
            gap: 16,
            marginTop: 16,
            padding: "8px 12px",
            background: "#161b22",
            border: "1px solid #2a313c",
            borderRadius: 8,
          }}
        >
          {devices.map((d) => (
            <span key={d.device_id} style={{ color: d.color, fontSize: 13 }}>
              ● {d.name} ({d.points.length})
            </span>
          ))}
        </div>
      )}

      <div style={{ marginTop: 16 }}>
        {isLoading && !data ? (
          <p style={{ opacity: 0.7 }}>loading…</p>
        ) : totalPoints === 0 ? (
          <p style={{ opacity: 0.7 }}>
            no GPS readings in the last {range.label}. Power on a GPS sensor and
            wait for it to get a fix (a clear view of the sky helps).
          </p>
        ) : (
          <GpsMap devices={devices} />
        )}
      </div>
    </main>
  );
}
