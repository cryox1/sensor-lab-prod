export const METRICS = [
  { key: "temp_c",       label: "temperature", unit: "°C",  color: "#ff7b72", digits: 1,
    description: "Ambient air temperature measured by the sensor." },
  { key: "humidity",     label: "humidity",    unit: "%",   color: "#58a6ff", digits: 0,
    description: "Relative humidity: how much moisture the air holds versus the most it could hold at this temperature." },
  { key: "heat_index_c", label: "heat index",  unit: "°C",  color: "#f0883e", digits: 1,
    description: "“Feels-like” temperature combining heat and humidity — how warm the air actually feels." },
  { key: "eco2_ppm",     label: "eCO₂",        unit: "ppm", color: "#7ee787", digits: 0,
    description: "Estimated equivalent CO₂ (parts per million), derived from VOC readings. ~400 ppm is fresh outdoor air; higher means stuffier air." },
  { key: "tvoc_ppb",     label: "TVOC",        unit: "ppb", color: "#d2a8ff", digits: 0,
    description: "Total volatile organic compounds (parts per billion): airborne gases from cleaning products, furniture, cooking, breath, etc." },
  { key: "aqi",          label: "AQI",         unit: "/5",  color: "#56d4dd", digits: 0,
    description: "The sensor's overall air-quality index on a 1–5 scale; lower is better." },
  { key: "batt_v",       label: "battery",     unit: "V",   color: "#3fb950", digits: 2,
    description: "Battery voltage of a deep-sleep node, read via an on-board resistor divider. A single LiPo cell is ~4.2 V full and ~3.3 V nearly empty — the downward slope tracks remaining runtime." },
  { key: "pressure_hpa", label: "pressure",    unit: "hPa", color: "#79c0ff", digits: 1,
    description: "Atmospheric (barometric) pressure in hectopascals, from a BME280. ~1013 hPa is average sea level; the reading drops with altitude and tends to dip before stormy weather." },
];

export const METRICS_BY_KEY = Object.fromEntries(METRICS.map((m) => [m.key, m]));

// GPS fields, shown in the map popups on /gps. Kept out of METRICS on purpose so
// they don't add line charts to the per-metric overview/group pages — they're
// rendered as points on a map instead. Same {key,label,unit,digits} shape so
// formatValue() works on them too.
export const GPS_METRICS = [
  { key: "alt_m",     label: "altitude", unit: "m",    digits: 0 },
  { key: "sats",      label: "sats",     unit: "",     digits: 0 },
  { key: "speed_kmh", label: "speed",    unit: "km/h", digits: 1 },
];

// Built-in categorization boundaries, drawn as dotted reference lines on charts.
// Keyed by metric key; each entry is ascending {value, label, color}. Air-quality
// metrics use a yellow → red severity ramp (higher = worse); temperature/humidity
// use comfort-band markers (both extremes flagged). These are defaults only — the
// user can override any metric's lines on the /settings page (persisted via the API,
// see GET/PUT /thresholds). Use effectiveThresholds() to merge overrides over these.
export const DEFAULT_THRESHOLDS = {
  temp_c: [
    { value: 18, label: "cold", color: "#58a6ff" },
    { value: 26, label: "warm", color: "#f0883e" },
    { value: 30, label: "hot",  color: "#f85149" },
  ],
  humidity: [
    { value: 30, label: "dry",   color: "#f0883e" },
    { value: 60, label: "humid", color: "#58a6ff" },
  ],
  eco2_ppm: [
    { value: 1000, label: "stuffy", color: "#d29922" }, // 1000–2000: open a window
    { value: 2000, label: "poor",   color: "#f0883e" }, // 2000–5000: headaches likely
    { value: 5000, label: "bad",    color: "#f85149" },
  ],
  tvoc_ppb: [
    { value: 250,  label: "acceptable", color: "#d29922" },
    { value: 500,  label: "ventilate",  color: "#f0883e" },
    { value: 1000, label: "poor",       color: "#f85149" },
    { value: 3000, label: "source",     color: "#da3633" },
  ],
  aqi: [
    { value: 3, label: "moderate",  color: "#d29922" },
    { value: 4, label: "poor",      color: "#f0883e" },
    { value: 5, label: "unhealthy", color: "#f85149" },
  ],
  batt_v: [
    { value: 3.0, label: "critical", color: "#f85149" }, // LiPo near cutoff
    { value: 3.3, label: "low",      color: "#f0883e" }, // getting low, recharge soon
  ],
};

// Resolve the threshold lines to draw for a metric: a user override (from the
// API, keyed by metric key) wins over the built-in default. An override that is
// an empty array intentionally hides all lines for that metric.
export function effectiveThresholds(metricKey, overrides) {
  const o = overrides?.[metricKey];
  if (Array.isArray(o)) return o;
  return DEFAULT_THRESHOLDS[metricKey] ?? [];
}

export const DEVICE_PALETTE = [
  "#58a6ff",
  "#ff7b72",
  "#7ee787",
  "#d2a8ff",
  "#f0883e",
  "#79c0ff",
  "#ffa657",
  "#a5d6ff",
  "#ffab70",
  "#56d4dd",
];

export function deviceColor(index) {
  return DEVICE_PALETTE[index % DEVICE_PALETTE.length];
}

export function formatValue(metric, v) {
  if (v == null || Number.isNaN(v)) return "—";
  return `${Number(v).toFixed(metric.digits)} ${metric.unit}`;
}

export const RANGES = [
  { label: "1h",  hours: 1,   bucketSeconds: null },
  { label: "6h",  hours: 6,   bucketSeconds: 60 },
  { label: "24h", hours: 24,  bucketSeconds: 300 },
  { label: "7d",  hours: 168, bucketSeconds: 1800 },
  { label: "30d", hours: 720, bucketSeconds: 7200 },
];
