import { compass } from "../report/weather";

export interface Reading {
  temp_c: number;
  rh_pct: number;
  wind_kmh: number;
  wind_dir: string;
  at: number; // unix seconds of the reading
}

// The lookup lives on the published site (public/_worker.js). While running on
// this PC (npm run dev) the app asks the published site instead.
const BASE = import.meta.env.DEV ? "https://ecosync-reporting.pages.dev" : "";

/** Weather at a place and moment (unix seconds), from OpenWeather via our own site. */
export async function lookupWeather(lat: number, lon: number, unixSec: number): Promise<Reading> {
  const r = await fetch(`${BASE}/api/weather?lat=${lat}&lon=${lon}&dt=${Math.round(unixSec)}`);
  const j = await r.json().catch(() => ({ error: `Weather lookup failed (${r.status})` }));
  if (!r.ok || j.error) throw new Error(j.error ?? `Weather lookup failed (${r.status})`);
  return {
    temp_c: Math.round(j.temp_c * 10) / 10,
    rh_pct: Math.round(j.rh_pct),
    wind_kmh: Math.round(j.wind_kmh * 10) / 10,
    wind_dir: compass(j.wind_deg),
    at: j.at,
  };
}
