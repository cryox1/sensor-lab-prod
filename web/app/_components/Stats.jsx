"use client";
import { formatValue } from "../_lib/metrics";

function computeStats(points, accessor) {
  let min = Infinity;
  let max = -Infinity;
  let sum = 0;
  let count = 0;
  let current = null;
  for (const p of points) {
    const v = accessor(p);
    if (v == null || Number.isNaN(v)) continue;
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
    count += 1;
    current = v;
  }
  if (count === 0) return null;
  return { min, max, avg: sum / count, current, count };
}

function Chip({ label, value }) {
  return (
    <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
      <span style={{ fontSize: 11, opacity: 0.55, textTransform: "uppercase", letterSpacing: 0.5 }}>{label}</span>
      <span style={{ fontSize: 15, fontWeight: 600 }}>{value}</span>
    </div>
  );
}

export default function Stats({ metric, points, accessor }) {
  const fn = accessor ?? ((p) => p[metric.key]);
  const s = computeStats(points, fn);
  if (!s) return null;
  return (
    <div style={{ display: "flex", gap: 24, flexWrap: "wrap", padding: "8px 0" }}>
      <Chip label="current" value={formatValue(metric, s.current)} />
      <Chip label="avg"     value={formatValue(metric, s.avg)} />
      <Chip label="min"     value={formatValue(metric, s.min)} />
      <Chip label="max"     value={formatValue(metric, s.max)} />
      <Chip label="samples" value={s.count.toString()} />
    </div>
  );
}
