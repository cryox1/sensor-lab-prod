"use client";

// Segmented control for the history view: live stream vs the bucketed
// timeline chart. Styled to match TimeRangeSelector's pills.
const MODES = [
  { value: "live", label: "live" },
  { value: "history", label: "5-min chart" },
];

export default function ModeToggle({ value, onChange }) {
  return (
    <div style={{ display: "inline-flex", gap: 4, background: "#161b22", border: "1px solid #2a313c", borderRadius: 999, padding: 4 }}>
      {MODES.map((m) => {
        const active = m.value === value;
        return (
          <button
            key={m.value}
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
              <span style={{ color: active ? "#3fb950" : "#9ba3af", marginRight: 5 }}>●</span>
            )}
            {m.label}
          </button>
        );
      })}
    </div>
  );
}
