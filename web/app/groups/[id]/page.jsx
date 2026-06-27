"use client";
import { useEffect, useMemo, useState } from "react";
import useSWR from "swr";
import Chart from "../../_components/Chart";
import TimeRangeSelector from "../../_components/TimeRangeSelector";
import InfoTooltip from "../../_components/InfoTooltip";
import { getApiBase } from "../../_lib/api";
import { useLiveSocket } from "../../_lib/useLiveSocket";
import { displayNameFor } from "../../_lib/displayName";
import { METRICS, RANGES, effectiveThresholds, deviceColor, formatValue } from "../../_lib/metrics";
import { useOffsets, offsetFor, applyOffset } from "../../_lib/offsets";

const fetcher = (url) => fetch(url).then((r) => r.json());

// Live mode keeps a rolling window of raw readings on screen.
const LIVE_WINDOW_MS = 5 * 60 * 1000;

// Telemetry timestamps arrive in two shapes: /ws/live carries the raw sensor
// `ts` as epoch *seconds* (a number), while /history-all returns an ISO string
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

export default function GroupOverviewPage({ params }) {
  const groupId = Number(params.id);
  const [mode, setMode] = useState("live"); // "live" | "history"
  const [range, setRange] = useState(RANGES[2]); // 24h
  const [includeHidden, setIncludeHidden] = useState(false);
  const [showThresholds, setShowThresholds] = useState(true);
  const offsets = useOffsets();
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

  const isLive = mode === "live";

  // Bucketed history — only fetched in history mode (no polling while live).
  const url =
    api && !isLive
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
  const { data: groups } = useSWR(
    api ? `${api}/groups` : null,
    fetcher,
    { refreshInterval: 30000 }
  );
  const { data: thresholdOverrides } = useSWR(
    api ? `${api}/thresholds` : null,
    fetcher,
    { refreshInterval: 30000 }
  );

  // Live stream: raw readings for every device, keyed by device_id and pruned
  // to the rolling window on insert. We can't filter to the group inside the
  // callback (useLiveSocket closes over it once, before /groups has loaded), so
  // we keep everything and filter to the group's members at render time. The
  // socket stays connected in both modes so switching to live shows data fast.
  const [livePoints, setLivePoints] = useState({});
  useLiveSocket((m) => {
    const t = tsToMillis(m.ts);
    if (Number.isNaN(t)) return;
    setLivePoints((prev) => {
      const cutoff = Date.now() - LIVE_WINDOW_MS;
      const arr = [...(prev[m.device_id] ?? []), { ...m, _t: t }]
        .filter((p) => p._t >= cutoff)
        .slice(-5000);
      return { ...prev, [m.device_id]: arr };
    });
  });

  // Tick once a second while live so the window keeps sliding and stale points
  // scroll off even when a sensor goes quiet.
  const [nowTick, setNowTick] = useState(0);
  useEffect(() => {
    if (!isLive) return;
    setNowTick(Date.now());
    const id = setInterval(() => setNowTick(Date.now()), 1000);
    return () => clearInterval(id);
  }, [isLive]);

  const group = useMemo(
    () => (groups ?? []).find((g) => g.id === groupId),
    [groups, groupId]
  );
  const groupIds = useMemo(() => new Set(group?.device_ids ?? []), [group]);

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

  const hiddenCount = useMemo(
    () => [...groupIds].filter((id) => hiddenSet.has(id)).length,
    [groupIds, hiddenSet]
  );

  // The group's roster (visible members unless "include hidden"), each with a
  // stable color assigned by position so colors match across live/history.
  const members = useMemo(() => {
    const ids = (group?.device_ids ?? []).filter(
      (id) => includeHidden || !hiddenSet.has(id)
    );
    return ids.map((id, i) => ({
      device_id: id,
      name: nameByDeviceId[id] || id,
      color: deviceColor(i),
    }));
  }, [group, includeHidden, hiddenSet, nameByDeviceId]);

  // Attach points to each member: a live rolling window, or bucketed history.
  const devices = useMemo(() => {
    if (isLive) {
      const cutoff = (nowTick || Date.now()) - LIVE_WINDOW_MS;
      return members.map((m) => ({
        ...m,
        points: (livePoints[m.device_id] ?? [])
          .filter((p) => p._t >= cutoff)
          .sort((a, b) => a._t - b._t),
      }));
    }
    if (!data) return [];
    const byId = new Map(data.map((d) => [d.device_id, d]));
    return members.map((m) => ({
      ...m,
      points: (byId.get(m.device_id)?.points ?? []).map((p) => ({
        ...p,
        _t: new Date(p.ts).getTime(),
      })),
    }));
  }, [isLive, members, data, livePoints, nowTick]);

  const hasAnyPoints = devices.some((d) => d.points.length > 0);
  const liveCount = devices.reduce((n, d) => n + d.points.length, 0);
  const formatX = isLive ? formatLiveX : pickFormatX(range.hours);

  // groups loaded but no matching id -> 404-ish state.
  const notFound = groups && !group;

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
          <h1 style={{ margin: 0 }}>{group ? group.name : "group"}</h1>
          <p style={{ opacity: 0.6, marginTop: 4, marginBottom: 0 }}>
            {isLive ? "live — last 5 min" : `group overview — ${range.label}`}
          </p>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: 16, flexWrap: "wrap" }}>
          <a href="/groups" style={{ color: "#58a6ff", fontSize: 13 }}>
            manage groups
          </a>
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
          <GroupModeToggle value={mode} onChange={setMode} />
          {!isLive && <TimeRangeSelector value={range.label} onChange={setRange} />}
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

      {members.length > 0 && (
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
          {members.map((d) => (
            <span key={d.device_id} style={{ color: d.color, fontSize: 13 }}>
              ● {d.name}
            </span>
          ))}
        </div>
      )}

      {notFound ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>
          group not found — it may have been deleted.{" "}
          <a href="/groups" style={{ color: "#58a6ff" }}>
            manage groups
          </a>
        </p>
      ) : (group && groupIds.size === 0) ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>
          no sensors in this group yet.{" "}
          <a href="/groups" style={{ color: "#58a6ff" }}>
            assign some
          </a>
        </p>
      ) : !isLive && isLoading && !data ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>loading…</p>
      ) : !hasAnyPoints ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>
          {isLive
            ? "waiting for live data…"
            : `no readings in the last ${range.label}.`}
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
                  {isLive && (
                    <span
                      style={{ color: "#3fb950", fontSize: 12, fontWeight: 400 }}
                      title="streaming live from the sensors"
                    >
                      ● live
                    </span>
                  )}
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
                  emptyLabel={isLive ? "waiting for live data…" : "no data"}
                />
              </section>
            );
          })}
          {isLive && (
            <p style={{ fontSize: 12, opacity: 0.5, marginTop: 0 }}>
              {liveCount} live readings · last 5 min
            </p>
          )}
        </div>
      )}
    </main>
  );
}

// Live vs history segmented control. Mirrors ModeToggle's styling but uses
// labels accurate for a group overview (history mode spans the chosen range,
// not a fixed 5-min window).
function GroupModeToggle({ value, onChange }) {
  const modes = [
    { value: "live", label: "live" },
    { value: "history", label: "history" },
  ];
  return (
    <div
      style={{
        display: "inline-flex",
        gap: 4,
        background: "#161b22",
        border: "1px solid #2a313c",
        borderRadius: 999,
        padding: 4,
      }}
    >
      {modes.map((m) => {
        const active = m.value === value;
        return (
          <button
            key={m.value}
            type="button"
            onClick={() => onChange(m.value)}
            style={{
              border: "none",
              background: active ? "#2a313c" : "transparent",
              color: active ? "#e6e6e6" : "#9ba3af",
              padding: "6px 12px",
              borderRadius: 999,
              fontSize: 13,
              cursor: "pointer",
              fontWeight: active ? 600 : 400,
            }}
          >
            {m.value === "live" && (
              <span style={{ color: active ? "#3fb950" : "#9ba3af", marginRight: 5 }}>
                ●
              </span>
            )}
            {m.label}
          </button>
        );
      })}
    </div>
  );
}
