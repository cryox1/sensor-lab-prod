"use client";
import { useEffect, useState } from "react";
import useSWR, { mutate } from "swr";
import { getApiBase, writeHeaders } from "../_lib/api";
import {
  METRICS,
  DEFAULT_THRESHOLDS,
  effectiveThresholds,
  formatValue,
} from "../_lib/metrics";

const fetcher = (url) => fetch(url).then((r) => r.json());

const NEW_LINE_COLOR = "#7d8590";

export default function SettingsPage() {
  const [api, setApi] = useState(null);
  useEffect(() => {
    setApi(getApiBase());
  }, []);

  const thresholdsKey = api ? `${api}/thresholds` : null;
  const { data: overrides, isLoading } = useSWR(thresholdsKey, fetcher, {
    refreshInterval: 30000,
  });

  return (
    <main style={{ padding: "32px", maxWidth: 900, margin: "0 auto" }}>
      <a href="/" style={{ color: "#58a6ff", fontSize: 13 }}>
        ← back
      </a>
      <h1 style={{ margin: "8px 0 4px" }}>settings</h1>
      <p style={{ opacity: 0.6, marginTop: 4 }}>
        chart threshold lines — the dotted reference lines drawn on history and
        overview charts. Edit values, labels and colors per metric; lines only
        show on a chart when they fall within the visible value range.
      </p>

      {isLoading && !overrides ? (
        <p style={{ opacity: 0.7, marginTop: 24 }}>loading…</p>
      ) : (
        <div
          style={{
            marginTop: 24,
            display: "flex",
            flexDirection: "column",
            gap: 16,
          }}
        >
          {METRICS.map((metric) => (
            <MetricThresholdEditor
              key={metric.key}
              api={api}
              metric={metric}
              // remount + reseed when the saved override for this metric changes
              seedKey={JSON.stringify(overrides?.[metric.key] ?? null)}
              initial={effectiveThresholds(metric.key, overrides)}
              isCustom={Array.isArray(overrides?.[metric.key])}
              onChanged={() => mutate(thresholdsKey)}
            />
          ))}
        </div>
      )}
    </main>
  );
}

function MetricThresholdEditor({ api, metric, seedKey, initial, isCustom, onChanged }) {
  // Local editable draft; reseeded whenever the saved value changes (via key).
  const [lines, setLines] = useState(() =>
    initial.map((t) => ({ ...t }))
  );
  const [status, setStatus] = useState(null); // null | "saving" | "saved" | "error"

  // Reseed when the persisted override changes underneath us.
  useEffect(() => {
    setLines(initial.map((t) => ({ ...t })));
    setStatus(null);
    // initial is derived from seedKey; depend on seedKey to avoid loops.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [seedKey]);

  const step = metric.digits > 0 ? 0.1 : 1;

  function updateLine(i, patch) {
    setLines((ls) => ls.map((l, j) => (j === i ? { ...l, ...patch } : l)));
    setStatus(null);
  }
  function removeLine(i) {
    setLines((ls) => ls.filter((_, j) => j !== i));
    setStatus(null);
  }
  function addLine() {
    setLines((ls) => [...ls, { value: 0, label: "", color: NEW_LINE_COLOR }]);
    setStatus(null);
  }

  async function save() {
    setStatus("saving");
    try {
      const payload = {
        thresholds: lines.map((l) => ({
          value: Number(l.value),
          label: l.label ?? "",
          color: l.color || NEW_LINE_COLOR,
        })),
      };
      const res = await fetch(`${api}/thresholds/${metric.key}`, {
        method: "PUT",
        headers: writeHeaders(),
        body: JSON.stringify(payload),
      });
      if (!res.ok) throw new Error(await res.text());
      setStatus("saved");
      onChanged?.();
    } catch {
      setStatus("error");
    }
  }

  async function resetToDefault() {
    setStatus("saving");
    try {
      const res = await fetch(`${api}/thresholds/${metric.key}`, {
        method: "DELETE",
        headers: writeHeaders(),
      });
      if (!res.ok) throw new Error(await res.text());
      setStatus("saved");
      onChanged?.();
    } catch {
      setStatus("error");
    }
  }

  const hasDefault = !!DEFAULT_THRESHOLDS[metric.key];

  return (
    <section
      style={{
        background: "#161b22",
        border: "1px solid #2a313c",
        borderRadius: 12,
        padding: "16px 18px",
      }}
    >
      <div
        style={{
          display: "flex",
          alignItems: "baseline",
          justifyContent: "space-between",
          gap: 12,
          flexWrap: "wrap",
        }}
      >
        <h2 style={{ fontSize: 16, fontWeight: 600, margin: 0 }}>
          {metric.label}{" "}
          <span style={{ opacity: 0.5, fontWeight: 400 }}>({metric.unit})</span>
          {isCustom ? (
            <span style={{ color: "#7ee787", fontSize: 12, marginLeft: 8 }}>
              customized
            </span>
          ) : (
            <span style={{ opacity: 0.4, fontSize: 12, marginLeft: 8 }}>
              default
            </span>
          )}
        </h2>
        <span style={{ fontSize: 12, minHeight: 16 }}>
          {status === "saving" && <span style={{ opacity: 0.6 }}>saving…</span>}
          {status === "saved" && <span style={{ color: "#7ee787" }}>saved ✓</span>}
          {status === "error" && (
            <span style={{ color: "#f85149" }}>save failed</span>
          )}
        </span>
      </div>

      <div
        style={{
          marginTop: 12,
          display: "flex",
          flexDirection: "column",
          gap: 8,
        }}
      >
        {lines.length === 0 && (
          <p style={{ opacity: 0.5, fontSize: 13, margin: 0 }}>
            no threshold lines — this metric's charts show none.
          </p>
        )}
        {lines.map((line, i) => (
          <div
            key={i}
            style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap" }}
          >
            <input
              type="number"
              step={step}
              value={line.value}
              onChange={(e) => updateLine(i, { value: e.target.value })}
              style={inputStyle(96)}
            />
            <span style={{ opacity: 0.5, fontSize: 12, width: 36 }}>
              {metric.unit}
            </span>
            <input
              type="text"
              value={line.label}
              maxLength={24}
              placeholder="label"
              onChange={(e) => updateLine(i, { label: e.target.value })}
              style={inputStyle(160)}
            />
            <input
              type="color"
              value={normalizeHex(line.color)}
              onChange={(e) => updateLine(i, { color: e.target.value })}
              title="line color"
              style={{
                width: 36,
                height: 30,
                padding: 0,
                border: "1px solid #2a313c",
                borderRadius: 6,
                background: "#0d1117",
                cursor: "pointer",
              }}
            />
            <button
              type="button"
              onClick={() => removeLine(i)}
              title="remove"
              style={linkButtonStyle("#999")}
            >
              remove
            </button>
          </div>
        ))}
      </div>

      <div style={{ display: "flex", gap: 16, marginTop: 14, flexWrap: "wrap" }}>
        <button type="button" onClick={addLine} style={linkButtonStyle("#58a6ff")}>
          + add line
        </button>
        <button
          type="button"
          onClick={save}
          disabled={status === "saving"}
          style={linkButtonStyle("#7ee787")}
        >
          save
        </button>
        {hasDefault && isCustom && (
          <button
            type="button"
            onClick={resetToDefault}
            disabled={status === "saving"}
            style={linkButtonStyle("#999")}
          >
            reset to default
          </button>
        )}
      </div>
    </section>
  );
}

function inputStyle(width) {
  return {
    background: "#0d1117",
    color: "#e6edf3",
    border: "1px solid #2a313c",
    borderRadius: 6,
    padding: "5px 8px",
    fontSize: 14,
    width,
  };
}

function linkButtonStyle(color) {
  return {
    background: "none",
    border: "none",
    color,
    cursor: "pointer",
    fontSize: 13,
    padding: 0,
  };
}

// <input type="color"> requires a 6-digit hex; expand #abc → #aabbcc and fall
// back to a neutral gray for anything unparseable.
function normalizeHex(c) {
  if (typeof c !== "string") return NEW_LINE_COLOR;
  const m = c.trim();
  if (/^#[0-9a-fA-F]{6}$/.test(m)) return m;
  if (/^#[0-9a-fA-F]{3}$/.test(m)) {
    return "#" + m.slice(1).split("").map((ch) => ch + ch).join("");
  }
  return NEW_LINE_COLOR;
}
