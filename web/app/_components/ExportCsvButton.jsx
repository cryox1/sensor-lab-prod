"use client";
import { buildCsv, csvFilename, downloadCsv } from "../_lib/csv";

// Downloads the dataset the page is currently showing as a long-format CSV.
// `devices` is the shape the overview pages already compute:
// [{ device_id, name, points: [{ _t, ...fields }] }].
export default function ExportCsvButton({ devices, scope, rangeLabel }) {
  const hasPoints = devices.some((d) => d.points.length > 0);
  return (
    <button
      type="button"
      disabled={!hasPoints}
      onClick={() =>
        downloadCsv(csvFilename(scope, rangeLabel), buildCsv(devices))
      }
      title={hasPoints ? "download the displayed data as CSV" : "no data to export"}
      style={{
        background: "#21262d",
        color: "#e6edf3",
        border: "1px solid #2a313c",
        borderRadius: 999,
        padding: "6px 14px",
        fontSize: 13,
        cursor: hasPoints ? "pointer" : "default",
        opacity: hasPoints ? 1 : 0.5,
      }}
    >
      export CSV
    </button>
  );
}
