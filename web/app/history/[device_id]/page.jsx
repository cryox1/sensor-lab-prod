"use client";
import { useEffect, useMemo, useState } from "react";
import useSWR from "swr";
import Chart from "../../_components/Chart";
import Stats from "../../_components/Stats";
import TimeRangeSelector from "../../_components/TimeRangeSelector";
import ModeToggle from "../../_components/ModeToggle";
import InfoTooltip from "../../_components/InfoTooltip";
import OffsetSettings from "../../_components/OffsetSettings";
import { getApiBase } from "../../_lib/api";
import { useLiveSocket } from "../../_lib/useLiveSocket";
import { displayNameFor } from "../../_lib/displayName";
import { METRICS, RANGES, effectiveThresholds, formatValue } from "../../_lib/metrics";
import { useOffsets, offsetFor, applyOffset } from "../../_lib/offsets";

const fetcher = (url) => fetch(url).then((r) => r.json());

// Live mode keeps a rolling window of raw readings on screen.
const LIVE_WINDOW_MS = 5 * 60 * 1000;

// Telemetry timestamps arrive in two shapes: /ws/live carries the raw sensor
// `ts` as epoch *seconds* (a number), while /history returns an ISO string
// (from the DB). Normalize both to epoch milliseconds.
function tsToMillis(ts) {
  return typeof ts === "number" ? ts * 1000 : new Date(ts).getTime();
}

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

// The live window spans only 5 minutes, so label the axis at second resolution.
const formatLiveX = (x) =>
  new Date(x).toLocaleTimeString([], { minute: "2-digit", second: "2-digit" });

export default function HistoryPage({ params }) {
  const { device_id } = params;
  const [mode, setMode] = useState("live"); // "live" | "history"
  const [range, setRange] = useState(RANGES[2]); // 24h
  const [showThresholds, setShowThresholds] = useState(true);
  const [showOffsets, setShowOffsets] = useState(false);
  const offsets = useOffsets();
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

  const isLive = mode === "live";

  // Bucketed history — only fetched while in history mode (no polling in live).
  const url =
    api && !isLive
      ? `${api}/history?device_id=${device_id}&hours=${range.hours}${
          range.bucketSeconds ? `&bucket_seconds=${range.bucketSeconds}` : ""
        }`
      : null;
  const { data, isLoading } = useSWR(url, fetcher, { refreshInterval: 30000 });

  // Live stream: raw readings straight from /ws/live, pruned to the rolling
  // window on insert. The socket stays connected in both modes (one cheap
  // connection), so switching to live shows recent data immediately.
  const [livePoints, setLivePoints] = useState([]);
  useLiveSocket((m) => {
    if (m.device_id !== device_id) return;
    const t = tsToMillis(m.ts);
    if (Number.isNaN(t)) return;
    setLivePoints((prev) =>
      [...prev, { ...m, _t: t }]
        .filter((p) => p._t >= Date.now() - LIVE_WINDOW_MS)
        .slice(-5000)
    );
  });

  // Tick once a second while live so the window keeps sliding and stale points
  // scroll off even when the sensor goes quiet.
  const [nowTick, setNowTick] = useState(0);
  useEffect(() => {
    if (!isLive) return;
    setNowTick(Date.now());
    const id = setInterval(() => setNowTick(Date.now()), 1000);
    return () => clearInterval(id);
  }, [isLive]);

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
  const deviceName =
    displayNameFor(
      (deviceMeta ?? []).find((d) => d.device_id === device_id)
    ) || device_id;

  const historyPoints = useMemo(
    () => (data ?? []).map((p) => ({ ...p, _t: tsToMillis(p.ts) })),
    [data]
  );

  const liveWindow = useMemo(() => {
    const cutoff = (nowTick || Date.now()) - LIVE_WINDOW_MS;
    return livePoints.filter((p) => p._t >= cutoff).sort((a, b) => a._t - b._t);
  }, [livePoints, nowTick]);

  const points = isLive ? liveWindow : historyPoints;

  const activeMetrics = useMemo(
    () => METRICS.filter((m) => points.some((p) => p[m.key] != null)),
    [points]
  );

  const formatX = isLive ? formatLiveX : pickFormatX(range.hours);

  const hasThresholds = activeMetrics.some(
    (m) => effectiveThresholds(m.key, thresholdOverrides).length > 0
  );

  // Latest non-null reading for a metric (the live "current" readout).
  const currentValue = (metric, offset) => {
    for (let i = points.length - 1; i >= 0; i--) {
      const v = points[i][metric.key];
      if (v != null) return applyOffset(v, offset);
    }
    return null;
  };

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
          <h1 style={{ margin: 0 }}>{deviceName}</h1>
          <p style={{ opacity: 0.6, marginTop: 4, marginBottom: 0 }}>
            {deviceName !== device_id ? `${device_id} — ` : ""}
            {isLive ? "live — last 5 min" : `history — ${range.label}`}
          </p>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: 16, flexWrap: "wrap" }}>
          {hasThresholds && (
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
          )}
          <button
            type="button"
            onClick={() => setShowOffsets((o) => !o)}
            title="set display offsets for this sensor"
            style={{
              color: showOffsets ? "#e6edf3" : "#58a6ff",
              fontSize: 13,
              background: showOffsets ? "#1f2630" : "transparent",
              cursor: "pointer",
              padding: "6px 12px",
              border: "1px solid #2a313c",
              borderRadius: 999,
            }}
          >
            ⚙ offsets
          </button>
          <ModeToggle value={mode} onChange={setMode} />
          {!isLive && <TimeRangeSelector value={range.label} onChange={setRange} />}
        </div>
      </div>

      {showOffsets && activeMetrics.length > 0 && (
        <div style={{ marginTop: 16 }}>
          <OffsetSettings
            deviceId={device_id}
            metrics={activeMetrics}
            offsets={offsets}
          />
        </div>
      )}

      {!isLive && isLoading && !data ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>loading…</p>
      ) : activeMetrics.length === 0 ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>
          {isLive
            ? "waiting for live data…"
            : `no readings in the last ${range.label}.`}
        </p>
      ) : (
        <div style={{ marginTop: 24, display: "flex", flexDirection: "column", gap: 32 }}>
          {activeMetrics.map((metric) => {
            const offset = offsetFor(offsets, device_id, metric.key);
            const series = [
              {
                label: metric.label,
                color: metric.color,
                points: points.map((p) => ({
                  x: p._t,
                  y: applyOffset(p[metric.key], offset),
                })),
              },
            ];
            const cur = isLive ? currentValue(metric, offset) : null;
            return (
              <section key={metric.key}>
                <h2
                  style={{
                    fontSize: 16,
                    fontWeight: 600,
                    margin: "0 0 4px",
                    display: "flex",
                    alignItems: "center",
                    gap: 8,
                    flexWrap: "wrap",
                  }}
                >
                  <span>
                    {metric.label}{" "}
                    <span style={{ opacity: 0.5, fontWeight: 400 }}>
                      ({metric.unit})
                    </span>
                  </span>
                  <InfoTooltip text={metric.description} />
                  {isLive && cur != null && (
                    <span style={{ fontSize: 18, fontWeight: 700 }}>
                      {formatValue(metric, cur)}
                    </span>
                  )}
                  {isLive && (
                    <span
                      style={{ color: "#3fb950", fontSize: 12, fontWeight: 400 }}
                      title="streaming live from the sensor"
                    >
                      ● live
                    </span>
                  )}
                  {offset !== 0 && (
                    <span
                      style={{ color: "#d29922", fontSize: 12, fontWeight: 400 }}
                      title="display offset applied"
                    >
                      offset {offset > 0 ? "+" : ""}
                      {offset} {metric.unit}
                    </span>
                  )}
                </h2>
                <Stats
                  metric={metric}
                  points={points}
                  accessor={(p) => applyOffset(p[metric.key], offset)}
                />
                <div style={{ marginTop: 4 }}>
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
                    emptyLabel={isLive ? "waiting for live data…" : "no data"}
                  />
                </div>
              </section>
            );
          })}
          <p style={{ fontSize: 12, opacity: 0.5, marginTop: 0 }}>
            {isLive
              ? `${points.length} live readings · last 5 min`
              : `${points.length} readings`}
          </p>
        </div>
      )}
    </main>
  );
}
