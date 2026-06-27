"use client";
import { offsetFor, setOffset, clearDeviceOffsets } from "../_lib/offsets";

// Per-device offset editor. Offsets are display-only: each value is added to the
// corresponding reading when rendered, in this browser only — the stored data is
// never changed. State lives in the localStorage-backed offsets store, so edits
// here propagate live to the charts, stats and dashboard.
export default function OffsetSettings({ deviceId, metrics, offsets }) {
  const hasAny = metrics.some((m) => offsetFor(offsets, deviceId, m.key) !== 0);

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
        <h2 style={{ fontSize: 16, fontWeight: 600, margin: 0 }}>reading offsets</h2>
        {hasAny && (
          <button
            type="button"
            onClick={() => clearDeviceOffsets(deviceId)}
            style={{
              background: "none",
              border: "none",
              color: "#999",
              cursor: "pointer",
              fontSize: 13,
              padding: 0,
            }}
          >
            reset all
          </button>
        )}
      </div>
      <p style={{ opacity: 0.6, fontSize: 13, margin: "4px 0 12px" }}>
        Display-only calibration: each offset is added to this sensor's readings
        when shown (charts, stats, dashboard). It is stored in this browser and
        never changes the recorded data.
      </p>

      <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
        {metrics.map((metric) => {
          const value = offsetFor(offsets, deviceId, metric.key);
          const step = metric.digits > 0 ? 0.1 : 1;
          return (
            <div
              key={metric.key}
              style={{ display: "flex", alignItems: "center", gap: 10, flexWrap: "wrap" }}
            >
              <span
                style={{
                  display: "inline-flex",
                  alignItems: "center",
                  gap: 6,
                  width: 140,
                  fontSize: 14,
                }}
              >
                <span style={{ color: metric.color }}>●</span>
                {metric.label}
              </span>
              <input
                type="number"
                step={step}
                value={value === 0 ? "" : value}
                placeholder="0"
                onChange={(e) => setOffset(deviceId, metric.key, e.target.value)}
                style={{
                  background: "#0d1117",
                  color: "#e6edf3",
                  border: "1px solid #2a313c",
                  borderRadius: 6,
                  padding: "5px 8px",
                  fontSize: 14,
                  width: 96,
                }}
              />
              <span style={{ opacity: 0.5, fontSize: 12 }}>{metric.unit}</span>
            </div>
          );
        })}
      </div>
    </section>
  );
}
