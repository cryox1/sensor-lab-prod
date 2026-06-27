"use client";
import { useState } from "react";

// A small "?" badge that reveals a short explanation. Shows on hover (desktop)
// and toggles on click/tap (touch). Styled to match the dark popover used by the
// chart Tooltip.
export default function InfoTooltip({ text, label = "?" }) {
  const [open, setOpen] = useState(false);

  return (
    <span
      style={{ position: "relative", display: "inline-flex", verticalAlign: "middle" }}
      onMouseEnter={() => setOpen(true)}
      onMouseLeave={() => setOpen(false)}
    >
      <button
        type="button"
        aria-label="what does this measure?"
        aria-expanded={open}
        onClick={() => setOpen((o) => !o)}
        style={{
          width: 16,
          height: 16,
          borderRadius: "50%",
          border: "1px solid #2a313c",
          background: "#0d1117",
          color: "#7d8590",
          fontSize: 11,
          lineHeight: "14px",
          fontWeight: 600,
          textAlign: "center",
          cursor: "help",
          padding: 0,
        }}
      >
        {label}
      </button>
      {open && (
        <span
          role="tooltip"
          style={{
            position: "absolute",
            top: "calc(100% + 6px)",
            left: 0,
            zIndex: 20,
            width: 260,
            background: "rgba(13,17,23,0.97)",
            border: "1px solid #2a313c",
            borderRadius: 8,
            padding: "8px 10px",
            fontSize: 12,
            fontWeight: 400,
            lineHeight: 1.45,
            color: "#e6edf3",
            boxShadow: "0 4px 16px rgba(0,0,0,0.4)",
            whiteSpace: "normal",
          }}
        >
          {text}
        </span>
      )}
    </span>
  );
}
