import { METRICS, GPS_METRICS } from "./metrics";

// Every field a point may carry, in export column order. Derived from the
// metric registry so newly added metrics show up in exports automatically.
// lat/lon are in neither METRICS nor GPS_METRICS (they're map coordinates,
// not chartable values) but belong in an export.
export const CSV_CANDIDATE_KEYS = [
  ...METRICS.map((m) => m.key),
  "lat",
  "lon",
  ...GPS_METRICS.map((m) => m.key),
];

// RFC 4180: quote fields containing comma, quote, or newline; double inner quotes.
function csvEscape(v) {
  if (v == null || (typeof v === "number" && Number.isNaN(v))) return "";
  const s = String(v);
  return /[",\r\n]/.test(s) ? `"${s.replaceAll('"', '""')}"` : s;
}

// Long-format CSV from the shape both overview pages already compute:
// [{ device_id, name, points: [{ _t (epoch ms), ...fields }] }]. One row per
// (device, point); only columns with at least one non-null value are emitted.
// Values are exported as stored — client display offsets are not applied.
export function buildCsv(devices, candidateKeys = CSV_CANDIDATE_KEYS) {
  const cols = candidateKeys.filter((k) =>
    devices.some((d) => d.points.some((p) => p[k] != null))
  );
  const lines = [["ts", "device_id", "device_name", ...cols].join(",")];
  for (const d of devices) {
    const points = [...d.points].sort((a, b) => a._t - b._t);
    for (const p of points) {
      lines.push(
        [
          new Date(p._t).toISOString(),
          csvEscape(d.device_id),
          csvEscape(d.name),
          ...cols.map((k) => csvEscape(p[k])),
        ].join(",")
      );
    }
  }
  return lines.join("\n") + "\n";
}

export function csvFilename(scope, rangeLabel) {
  const slug = (s) =>
    String(s)
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "") || "export";
  const date = new Date().toISOString().slice(0, 10);
  return `sensorlab_${slug(scope)}_${slug(rangeLabel)}_${date}.csv`;
}

export function downloadCsv(filename, text) {
  const blob = new Blob([text], { type: "text/csv;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}
