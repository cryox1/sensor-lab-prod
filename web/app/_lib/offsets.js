"use client";
import { useSyncExternalStore } from "react";

// Per-sensor, per-metric display offsets. These are a pure client-side
// presentation adjustment — added to a reading only when it is rendered. They
// never touch the database or the API; the stored readings are unchanged. The
// whole map lives in one localStorage key as:
//   { [device_id]: { [metric_key]: number } }
const STORAGE_KEY = "sensorOffsets";

const EMPTY = {};

// Cached parsed snapshot so useSyncExternalStore sees a stable reference between
// renders (it bails out of re-rendering when the snapshot is referentially equal).
let cache = null;
let cacheRaw = null;

const subscribers = new Set();

function readRaw() {
  if (typeof window === "undefined") return null;
  try {
    return window.localStorage.getItem(STORAGE_KEY);
  } catch {
    return null;
  }
}

function getSnapshot() {
  const raw = readRaw();
  if (raw === cacheRaw && cache !== null) return cache;
  cacheRaw = raw;
  if (!raw) {
    cache = EMPTY;
    return cache;
  }
  try {
    const parsed = JSON.parse(raw);
    cache = parsed && typeof parsed === "object" ? parsed : EMPTY;
  } catch {
    cache = EMPTY;
  }
  return cache;
}

function getServerSnapshot() {
  return EMPTY;
}

function subscribe(cb) {
  subscribers.add(cb);
  // Cross-tab updates: localStorage fires "storage" only in *other* tabs.
  const onStorage = (e) => {
    if (e.key === STORAGE_KEY) cb();
  };
  if (typeof window !== "undefined") window.addEventListener("storage", onStorage);
  return () => {
    subscribers.delete(cb);
    if (typeof window !== "undefined") window.removeEventListener("storage", onStorage);
  };
}

function notify() {
  // Invalidate the cache so the next getSnapshot re-reads, then wake subscribers.
  cacheRaw = null;
  for (const cb of subscribers) cb();
}

function write(next) {
  if (typeof window === "undefined") return;
  try {
    if (next && Object.keys(next).length > 0) {
      window.localStorage.setItem(STORAGE_KEY, JSON.stringify(next));
    } else {
      window.localStorage.removeItem(STORAGE_KEY);
    }
  } catch {
    // ignore quota / privacy-mode errors — offsets are best-effort UI state
  }
  notify();
}

// React hook: returns the current offsets map (reactive across components and tabs).
export function useOffsets() {
  return useSyncExternalStore(subscribe, getSnapshot, getServerSnapshot);
}

// Read a single offset; missing entries default to 0.
export function offsetFor(offsets, deviceId, metricKey) {
  const v = offsets?.[deviceId]?.[metricKey];
  return typeof v === "number" && !Number.isNaN(v) ? v : 0;
}

// Apply an offset to a reading for display. Null/NaN readings stay as-is.
export function applyOffset(value, offset) {
  if (value == null || Number.isNaN(value)) return value;
  return value + offset;
}

// Set (or clear, when 0) one device/metric offset and persist.
export function setOffset(deviceId, metricKey, value) {
  const current = getSnapshot();
  const num = Number(value);
  const next = { ...current, [deviceId]: { ...current[deviceId] } };
  if (!num || Number.isNaN(num)) {
    delete next[deviceId][metricKey];
  } else {
    next[deviceId][metricKey] = num;
  }
  if (Object.keys(next[deviceId]).length === 0) delete next[deviceId];
  write(next);
}

// Remove all offsets for a single device.
export function clearDeviceOffsets(deviceId) {
  const current = getSnapshot();
  if (!current[deviceId]) return;
  const next = { ...current };
  delete next[deviceId];
  write(next);
}
