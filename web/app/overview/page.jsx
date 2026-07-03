"use client";
import { useEffect, useMemo, useState } from "react";
import useSWR from "swr";
import Chart from "../_components/Chart";
import ExportCsvButton from "../_components/ExportCsvButton";
import TimeRangeSelector from "../_components/TimeRangeSelector";
import InfoTooltip from "../_components/InfoTooltip";
import { getApiBase } from "../_lib/api";
import { displayNameFor } from "../_lib/displayName";
import { METRICS, RANGES, effectiveThresholds, deviceColor, formatValue } from "../_lib/metrics";
import { useOffsets, offsetFor, applyOffset } from "../_lib/offsets";

const fetcher = (url) => fetch(url).then((r) => r.json());

function pickFormatX(hours) {
  if (hours <= 24) {
    return (x) =>
      new Date(x).toLocaleTimeString([], {
        hour: "2-digit",
        minute: "2-digit",
      });
  }
  return (x) => {
    const d = new Date(x);
    return `${d.toLocaleDateString([], {
      month: "2-digit",
      day: "2-digit",
    })} ${d.toLocaleTimeString([], { hour: "2-digit" })}`;
  };
}

export default function OverviewPage() {
  const [range, setRange] = useState(RANGES[2]); // 24h
  const [includeHidden, setIncludeHidden] = useState(false);
  const [showThresholds, setShowThresholds] = useState(true);
  const offsets = useOffsets();
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

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
  const { data: thresholdOverrides } = useSWR(
    api ? `${api}/thresholds` : null,
    fetcher,
    { refreshInterval: 30000 }
  );

  const nameByDeviceId = useMemo(() => {
    const map = {};
    for (const d of deviceMeta ?? []) map[d.device_id] = displayNameFor(d);
    return map;
  }, [deviceMeta]);

  const hiddenSet = useMemo(() => {
    const s = new Set();
    for (const d of deviceMeta ?? []) if (d.hidden) s.add(d.device_id);
    return s;
  }, [deviceMeta]);

  const hiddenCount = hiddenSet.size;

  const devices = useMemo(() => {
    if (!data) return [];
    const filtered = includeHidden
      ? data
      : data.filter((d) => !hiddenSet.has(d.device_id));
    return filtered.map((d, i) => ({
      device_id: d.device_id,
      name: nameByDeviceId[d.device_id] || d.device_id,
      color: deviceColor(i),
      points: d.points.map((p) => ({ ...p, _t: new Date(p.ts).getTime() })),
    }));
  }, [data, nameByDeviceId, hiddenSet, includeHidden]);

  const formatX = pickFormatX(range.hours);

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
          <h1 style={{ margin: 0 }}>overview</h1>
          <p style={{ opacity: 0.6, marginTop: 4, marginBottom: 0 }}>
            all sensors — {range.label}
          </p>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: 16, flexWrap: "wrap" }}>
          <label
            style={{
              display: "inline-flex",
              alignItems: "center",
              gap: 6,
              fontSize: 13,
              opacity: 0.8,
              cursor: "pointer",
            }}
          >
            <input
              type="checkbox"
              checked={showThresholds}
              onChange={(e) => setShowThresholds(e.target.checked)}
            />
            quality lines
          </label>
          <TimeRangeSelector value={range.label} onChange={setRange} />
          <ExportCsvButton
            devices={devices}
            scope="overview"
            rangeLabel={range.label}
          />
        </div>
      </div>

      {hiddenCount > 0 && (
        <label
          style={{
            display: "inline-flex",
            alignItems: "center",
            gap: 6,
            marginTop: 16,
            fontSize: 13,
            opacity: 0.8,
            cursor: "pointer",
          }}
        >
          <input
            type="checkbox"
            checked={includeHidden}
            onChange={(e) => setIncludeHidden(e.target.checked)}
          />
          include {hiddenCount} hidden device{hiddenCount === 1 ? "" : "s"}
        </label>
      )}

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
              ● {d.name}
            </span>
          ))}
        </div>
      )}

      {isLoading && !data ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>loading…</p>
      ) : devices.length === 0 ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>
          no readings in the last {range.label}.
        </p>
      ) : (
        <div style={{ marginTop: 24, display: "flex", flexDirection: "column", gap: 32 }}>
          {METRICS.map((metric) => {
            const series = devices
              .map((d) => ({
                label: d.name,
                color: d.color,
                points: d.points.map((p) => ({
                  x: p._t,
                  y: applyOffset(
                    p[metric.key],
                    offsetFor(offsets, d.device_id, metric.key)
                  ),
                })),
              }))
              .filter((s) => s.points.some((p) => p.y != null));
            if (series.length === 0) return null;
            return (
              <section key={metric.key}>
                <h2
                  style={{
                    fontSize: 16,
                    fontWeight: 600,
                    margin: "0 0 8px",
                    display: "flex",
                    alignItems: "center",
                    gap: 8,
                  }}
                >
                  <span>
                    {metric.label}{" "}
                    <span style={{ opacity: 0.5, fontWeight: 400 }}>
                      ({metric.unit})
                    </span>
                  </span>
                  <InfoTooltip text={metric.description} />
                </h2>
                <Chart
                  series={series}
                  yLabel={`${metric.label} (${metric.unit})`}
                  formatX={formatX}
                  formatY={(v) =>
                    v == null ? "" : Number(v).toFixed(metric.digits)
                  }
                  formatTooltipY={(v) => formatValue(metric, v)}
                  thresholds={effectiveThresholds(metric.key, thresholdOverrides)}
                  showThresholds={showThresholds}
                />
              </section>
            );
          })}
        </div>
      )}
    </main>
  );
}
