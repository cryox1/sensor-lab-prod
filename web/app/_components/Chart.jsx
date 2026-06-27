"use client";
import { useMemo, useRef, useState } from "react";

const DEFAULT_W = 900;
const DEFAULT_H = 260;
const PAD_LEFT = 56;
const PAD_RIGHT = 16;
const PAD_TOP = 16;
const PAD_BOTTOM = 32;

export default function Chart({
  series,
  yLabel,
  formatY = (v) => (v == null ? "" : Number(v).toFixed(1)),
  formatX = (x) =>
    new Date(x).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }),
  formatTooltipX = (x) => new Date(x).toLocaleString(),
  formatTooltipY,
  height = DEFAULT_H,
  width = DEFAULT_W,
  emptyLabel = "no data",
  thresholds = [],
  showThresholds = true,
}) {
  const svgRef = useRef(null);
  const [cursor, setCursor] = useState(null);

  const stats = useMemo(() => {
    let xMin = Infinity,
      xMax = -Infinity,
      yMin = Infinity,
      yMax = -Infinity;
    const xs = new Set();
    for (const s of series) {
      for (const p of s.points) {
        if (p.y == null || Number.isNaN(p.y)) continue;
        xs.add(p.x);
        if (p.x < xMin) xMin = p.x;
        if (p.x > xMax) xMax = p.x;
        if (p.y < yMin) yMin = p.y;
        if (p.y > yMax) yMax = p.y;
      }
    }
    if (!isFinite(yMin)) {
      return null;
    }
    if (yMin === yMax) {
      yMin -= 1;
      yMax += 1;
    } else {
      const pad = (yMax - yMin) * 0.08;
      yMin -= pad;
      yMax += pad;
    }
    return { xMin, xMax, yMin, yMax, allXs: [...xs].sort((a, b) => a - b) };
  }, [series]);

  const plotW = width - PAD_LEFT - PAD_RIGHT;
  const plotH = height - PAD_TOP - PAD_BOTTOM;

  const yTicks = useMemo(() => {
    if (!stats) return [];
    const n = 5;
    const out = [];
    for (let i = 0; i <= n; i++) {
      out.push(stats.yMin + ((stats.yMax - stats.yMin) * i) / n);
    }
    return out;
  }, [stats]);

  const xTicks = useMemo(() => {
    if (!stats) return [];
    const n = 5;
    const out = [];
    for (let i = 0; i <= n; i++) {
      out.push(stats.xMin + ((stats.xMax - stats.xMin) * i) / n);
    }
    return out;
  }, [stats]);

  const paths = useMemo(() => {
    if (!stats) return [];
    const { xMin, xMax, yMin, yMax } = stats;
    const px = (x) => PAD_LEFT + ((x - xMin) / (xMax - xMin || 1)) * plotW;
    const py = (y) =>
      PAD_TOP + plotH - ((y - yMin) / (yMax - yMin || 1)) * plotH;
    return series.map((s) => {
      let d = "";
      let started = false;
      for (const p of s.points) {
        if (p.y == null || Number.isNaN(p.y)) {
          started = false;
          continue;
        }
        d += `${started ? "L" : "M"} ${px(p.x)} ${py(p.y)} `;
        started = true;
      }
      return { ...s, d: d.trim() };
    });
  }, [series, stats, plotW, plotH]);

  if (!stats) {
    return (
      <div
        style={{
          width: "100%",
          maxWidth: width,
          height,
          background: "#161b22",
          border: "1px solid #2a313c",
          borderRadius: 12,
          display: "flex",
          alignItems: "center",
          justifyContent: "center",
          color: "#7d8590",
          fontSize: 13,
        }}
      >
        {emptyLabel}
      </div>
    );
  }

  const { xMin, xMax, yMin, yMax, allXs } = stats;
  const px = (x) => PAD_LEFT + ((x - xMin) / (xMax - xMin || 1)) * plotW;
  const py = (y) =>
    PAD_TOP + plotH - ((y - yMin) / (yMax - yMin || 1)) * plotH;

  function handleMove(e) {
    const svg = svgRef.current;
    if (!svg || allXs.length === 0) return;
    const rect = svg.getBoundingClientRect();
    const xPx = ((e.clientX - rect.left) / rect.width) * width;
    if (xPx < PAD_LEFT || xPx > width - PAD_RIGHT) {
      setCursor(null);
      return;
    }
    const dataX = xMin + ((xPx - PAD_LEFT) / plotW) * (xMax - xMin);
    let nearest = allXs[0];
    let bestDelta = Math.abs(nearest - dataX);
    for (const x of allXs) {
      const d = Math.abs(x - dataX);
      if (d < bestDelta) {
        bestDelta = d;
        nearest = x;
      }
    }
    setCursor({ dataX: nearest, svgX: px(nearest) });
  }

  const tooltipRows = cursor
    ? series.map((s) => {
        const p = s.points.find((pp) => pp.x === cursor.dataX);
        return { label: s.label, color: s.color, value: p?.y ?? null };
      })
    : [];

  const formatTip = formatTooltipY ?? formatY;

  return (
    <div style={{ position: "relative", width: "100%", maxWidth: width }}>
      <svg
        ref={svgRef}
        viewBox={`0 0 ${width} ${height}`}
        width="100%"
        height={height}
        style={{
          background: "#161b22",
          border: "1px solid #2a313c",
          borderRadius: 12,
          display: "block",
        }}
        onMouseMove={handleMove}
        onMouseLeave={() => setCursor(null)}
      >
        {yTicks.map((v, i) => (
          <g key={`y-${i}`}>
            <line
              x1={PAD_LEFT}
              x2={width - PAD_RIGHT}
              y1={py(v)}
              y2={py(v)}
              stroke="#2a313c"
              strokeDasharray={i === 0 ? "" : "2 4"}
            />
            <text
              x={PAD_LEFT - 8}
              y={py(v)}
              fill="#7d8590"
              fontSize="11"
              textAnchor="end"
              dominantBaseline="middle"
            >
              {formatY(v)}
            </text>
          </g>
        ))}
        {xTicks.map((v, i) => (
          <text
            key={`x-${i}`}
            x={px(v)}
            y={height - PAD_BOTTOM + 18}
            fill="#7d8590"
            fontSize="11"
            textAnchor="middle"
          >
            {formatX(v)}
          </text>
        ))}
        {showThresholds &&
          thresholds.map((t, i) => {
            if (t.value < yMin || t.value > yMax) return null; // in-range only, no axis change
            const ty = py(t.value);
            return (
              <g key={`th-${i}`}>
                <line
                  x1={PAD_LEFT}
                  x2={width - PAD_RIGHT}
                  y1={ty}
                  y2={ty}
                  stroke={t.color}
                  strokeDasharray="5 4"
                  strokeWidth="1"
                  opacity="0.7"
                />
                <text
                  x={width - PAD_RIGHT - 4}
                  y={ty - 3}
                  fill={t.color}
                  fontSize="10"
                  textAnchor="end"
                  opacity="0.85"
                >
                  {t.label}
                </text>
              </g>
            );
          })}
        {paths.map((s, i) => (
          <path
            key={i}
            d={s.d}
            stroke={s.color}
            fill="none"
            strokeWidth="1.5"
          />
        ))}
        {cursor && (
          <>
            <line
              x1={cursor.svgX}
              x2={cursor.svgX}
              y1={PAD_TOP}
              y2={height - PAD_BOTTOM}
              stroke="#7d8590"
              strokeDasharray="2 3"
            />
            {tooltipRows.map((r, i) => {
              const p = series[i].points.find((pp) => pp.x === cursor.dataX);
              if (!p || p.y == null) return null;
              return (
                <circle
                  key={i}
                  cx={cursor.svgX}
                  cy={py(p.y)}
                  r="3.5"
                  fill={r.color}
                  stroke="#0e1116"
                  strokeWidth="1.5"
                />
              );
            })}
          </>
        )}
        {yLabel && (
          <text
            transform={`translate(14, ${PAD_TOP + plotH / 2}) rotate(-90)`}
            fill="#7d8590"
            fontSize="11"
            textAnchor="middle"
          >
            {yLabel}
          </text>
        )}
      </svg>
      {cursor && tooltipRows.some((r) => r.value != null) && (
        <Tooltip
          cursor={cursor}
          rows={tooltipRows}
          formatX={formatTooltipX}
          formatY={formatTip}
          width={width}
        />
      )}
    </div>
  );
}

function Tooltip({ cursor, rows, formatX, formatY, width }) {
  const ratio = cursor.svgX / width;
  const onRight = ratio > 0.55;
  return (
    <div
      style={{
        position: "absolute",
        top: 8,
        [onRight ? "left" : "right"]: undefined,
        [onRight ? "right" : "left"]: 8,
        background: "rgba(13,17,23,0.95)",
        border: "1px solid #2a313c",
        borderRadius: 8,
        padding: "8px 10px",
        fontSize: 12,
        pointerEvents: "none",
        minWidth: 160,
      }}
    >
      <div style={{ opacity: 0.6, marginBottom: 6 }}>
        {formatX(cursor.dataX)}
      </div>
      {rows.map((r) => (
        <div
          key={r.label}
          style={{
            display: "flex",
            justifyContent: "space-between",
            gap: 12,
            marginTop: 2,
          }}
        >
          <span style={{ color: r.color }}>● {r.label}</span>
          <span>{r.value == null ? "—" : formatY(r.value)}</span>
        </div>
      ))}
    </div>
  );
}
