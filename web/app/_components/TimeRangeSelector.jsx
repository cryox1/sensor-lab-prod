"use client";
import { RANGES } from "../_lib/metrics";

export default function TimeRangeSelector({ value, onChange }) {
  return (
    <div style={{ display: "inline-flex", gap: 4, background: "#161b22", border: "1px solid #2a313c", borderRadius: 999, padding: 4 }}>
      {RANGES.map((r) => {
        const active = r.label === value;
        return (
          <button
            key={r.label}
            onClick={() => onChange(r)}
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
            {r.label}
          </button>
        );
      })}
    </div>
  );
}
