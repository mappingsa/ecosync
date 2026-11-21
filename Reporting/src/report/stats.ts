// Report maths. Pure functions over neutral features: no ArcGIS, no browser,
// so they are unit-tested (stats.test.ts) and reused unchanged by the mobile app.

import type { Attributes, Feature, Position, TargetRow } from "../data/types";

// --- Working day -------------------------------------------------------------

/** Offset of `timeZone` from UTC at instant `ms`, in ms (ACDT = +10.5 h). */
function tzOffsetMs(ms: number, timeZone: string): number {
  const parts = new Intl.DateTimeFormat("en-US", {
    timeZone,
    hourCycle: "h23",
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(ms));
  const n = (t: string) => Number(parts.find((p) => p.type === t)?.value);
  const asUtc = Date.UTC(n("year"), n("month") - 1, n("day"), n("hour"), n("minute"), n("second"));
  return asUtc - Math.floor(ms / 1000) * 1000;
}

/** Local midnight of a YYYY-MM-DD date in `timeZone`, as epoch ms. */
function localMidnight(date: string, timeZone: string): number {
  const [y, m, d] = date.split("-").map(Number);
  const guess = Date.UTC(y, m - 1, d);
  let t = guess - tzOffsetMs(guess, timeZone);
  t = guess - tzOffsetMs(t, timeZone); // second pass settles a daylight-saving change
  return t;
}

/** [start, end) of a LOCAL working day. SA moves between ACST and ACDT, so never assume UTC. */
export function dayWindow(date: string, timeZone: string): [number, number] {
  const [y, m, d] = date.split("-").map(Number);
  const next = new Date(Date.UTC(y, m - 1, d + 1)).toISOString().slice(0, 10);
  return [localMidnight(date, timeZone), localMidnight(next, timeZone)];
}

// --- Attributes and job matching ----------------------------------------------

/** Case-insensitive attribute lookup: Job_Number on the unit layers, job_number on Form 1. */
export function attr(a: Attributes, name: string): unknown {
  if (name in a) return a[name];
  const low = name.toLowerCase();
  for (const k of Object.keys(a)) if (k.toLowerCase() === low) return a[k];
  return undefined;
}

/** Job numbers are typed by hand: ignore case and stray spaces, as End of Day does. */
export const normJob = (v: unknown) => String(v ?? "").trim().replace(/\s+/g, " ").toUpperCase();

/** A job as far as matching is concerned: its number plus any old names. */
export interface JobName {
  job_number: string;
  aliases?: string;
}

/**
 * The Jobs-table job number a typed value belongs to, matching the number or
 * any alias ("DEW NP Weed Control " -> "DEW-NP"). Unknown values come back
 * trimmed, so a job missing from the table still shows and can be added.
 */
export function canonicalJob(raw: unknown, jobs: JobName[] = []): string {
  const key = normJob(raw);
  if (!key) return "";
  for (const j of jobs) {
    const names = [j.job_number, ...String(j.aliases ?? "").split(";")];
    if (names.some((n) => normJob(n) === key)) return j.job_number.trim();
  }
  return String(raw ?? "").trim().replace(/\s+/g, " ");
}

export const sameJob = (raw: unknown, job: string, jobs: JobName[] = []) =>
  normJob(canonicalJob(raw, jobs)) === normJob(canonicalJob(job, jobs));

export function jobsIn(features: Feature[], jobs: JobName[] = []): { jobs: string[]; blank: number } {
  const seen = new Map<string, string>();
  let blank = 0;
  for (const f of features) {
    const name = canonicalJob(attr(f.properties, "Job_Number"), jobs);
    if (!name) blank++;
    else if (!seen.has(normJob(name))) seen.set(normJob(name), name);
  }
  return { jobs: [...seen.values()].sort(), blank };
}

export const forJob = (features: Feature[], job: string, jobs: JobName[] = []) =>
  features.filter((f) => sameJob(attr(f.properties, "Job_Number"), job, jobs));

// --- Accuracy (matches spray_stats.py) ------------------------------------------

export interface Accuracy {
  captured: number; // records, including those with no accuracy value
  measured: number; // records carrying gps_accuracy_m
  average: number | null;
  worst: number | null;
  withinPct: number | null; // floored, so 789 of 790 shows 99 %, never a false 100 %
}

export function accuracy(features: Feature[], thresholdM: number): Accuracy {
  const vals = features
    .map((f) => Number(attr(f.properties, "gps_accuracy_m")))
    .filter((v) => Number.isFinite(v) && v > 0);
  const n = vals.length;
  if (n === 0) return { captured: features.length, measured: 0, average: null, worst: null, withinPct: null };
  const within = vals.filter((v) => v <= thresholdM).length;
  return {
    captured: features.length,
    measured: n,
    average: vals.reduce((s, v) => s + v, 0) / n,
    worst: Math.max(...vals),
    withinPct: within === n ? 100 : Math.floor((100 * within) / n),
  };
}

// --- Geodesic measures ----------------------------------------------------------

const R = 6378137; // WGS84 semi-major axis, m
const rad = (d: number) => (d * Math.PI) / 180;

/** Area of one ring on the sphere, m² (always positive). */
function ringArea(ring: Position[]): number {
  let s = 0;
  for (let i = 0; i < ring.length - 1; i++) {
    const [l1, p1] = ring[i];
    const [l2, p2] = ring[i + 1];
    s += rad(l2 - l1) * (2 + Math.sin(rad(p1)) + Math.sin(rad(p2)));
  }
  return Math.abs((s * R * R) / 2);
}

export function areaM2(f: Feature): number {
  const g = f.geometry;
  if (!g) return 0;
  const poly = (rings: Position[][]) => rings.reduce((s, r, i) => s + (i === 0 ? 1 : -1) * ringArea(r), 0);
  if (g.type === "Polygon") return poly(g.coordinates);
  if (g.type === "MultiPolygon") return g.coordinates.reduce((s, p) => s + poly(p), 0);
  return 0;
}

function pathLength(path: Position[]): number {
  let s = 0;
  for (let i = 0; i < path.length - 1; i++) {
    const [l1, p1] = path[i];
    const [l2, p2] = path[i + 1];
    const a =
      Math.sin(rad(p2 - p1) / 2) ** 2 + Math.cos(rad(p1)) * Math.cos(rad(p2)) * Math.sin(rad(l2 - l1) / 2) ** 2;
    s += 2 * R * Math.asin(Math.min(1, Math.sqrt(a)));
  }
  return s;
}

export function lengthM(f: Feature): number {
  const g = f.geometry;
  if (!g) return 0;
  if (g.type === "LineString") return pathLength(g.coordinates);
  if (g.type === "MultiLineString") return g.coordinates.reduce((s, p) => s + pathLength(p), 0);
  return 0;
}

// --- Treatment summary -----------------------------------------------------------

export interface SummaryRow {
  species: string;
  method: string;
  points: number;
  lines: number;
  lengthM: number;
  polygons: number;
  areaM2: number;
}

export const speciesOf = (a: Attributes) => String(attr(a, "weed_species") ?? "").trim() || "(no species)";
/** The record's own control_type; if blank, the species' method from the Targets table; else Spray. */
export function methodOf(a: Attributes, targets: TargetRow[] = []): string {
  const recorded = String(attr(a, "control_type") ?? "").trim();
  if (recorded) return recorded;
  const sp = speciesOf(a).toLowerCase();
  const listed = targets.find((t) => t.species.toLowerCase() === sp)?.control_method ?? "";
  return listed.split(";")[0].trim() || "Spray"; // first method listed for the species
}

export function summarise(points: Feature[], lines: Feature[], polygons: Feature[], targets: TargetRow[] = []): SummaryRow[] {
  const rows = new Map<string, SummaryRow>();
  const row = (a: Attributes) => {
    const species = speciesOf(a);
    const method = methodOf(a, targets);
    const key = `${species}|${method}`;
    if (!rows.has(key)) rows.set(key, { species, method, points: 0, lines: 0, lengthM: 0, polygons: 0, areaM2: 0 });
    return rows.get(key)!;
  };
  points.forEach((f) => row(f.properties).points++);
  lines.forEach((f) => {
    const r = row(f.properties);
    r.lines++;
    r.lengthM += lengthM(f);
  });
  polygons.forEach((f) => {
    const r = row(f.properties);
    r.polygons++;
    r.areaM2 += areaM2(f);
  });
  return [...rows.values()].sort((a, b) => a.species.localeCompare(b.species) || a.method.localeCompare(b.method));
}

/** [west, south, east, north] of everything, or null for an empty day. */
export function bounds(features: Feature[]): [number, number, number, number] | null {
  let w = Infinity, s = Infinity, e = -Infinity, n = -Infinity;
  const visit = (p: Position) => {
    w = Math.min(w, p[0]); e = Math.max(e, p[0]);
    s = Math.min(s, p[1]); n = Math.max(n, p[1]);
  };
  const walk = (c: unknown): void => {
    if (Array.isArray(c) && typeof c[0] === "number") visit(c as Position);
    else if (Array.isArray(c)) c.forEach(walk);
  };
  features.forEach((f) => f.geometry && walk(f.geometry.coordinates));
  return Number.isFinite(w) ? [w, s, e, n] : null;
}

// --- Detail map (ported from spray_stats.py detail_extent) -----------------------

/** The narrowest interval holding pct % of the sorted values: where they are densest. */
export function shortestInterval(sorted: number[], pct: number): [number, number] {
  const n = sorted.length;
  const k = Math.max(2, Math.ceil((n * pct) / 100));
  if (k >= n) return [sorted[0], sorted[n - 1]];
  let best: [number, number, number] | null = null;
  for (let i = 0; i + k - 1 < n; i++) {
    const span = sorted[i + k - 1] - sorted[i];
    if (!best || span < best[0]) best = [span, sorted[i], sorted[i + k - 1]];
  }
  return [best![1], best![2]];
}

const DETAIL_STEPS = [90, 80, 70, 60, 50, 40];
const DETAIL_AREA_RATIO = 0.1; // the core must be under a tenth of the day's area
const DETAIL_MIN_SHARE = 0.25; // ...and still hold a quarter of the records
const DETAIL_FALLBACK = 80; // compact day: just zoom in a little
const MIN_SPAN_DEG = 0.0025; // ~250 m, so one plant does not zoom to infinity

/** One position per record: the point itself, or the mean of a line's/polygon's vertices. */
function recordPositions(features: Feature[]): Position[] {
  const out: Position[] = [];
  for (const f of features) {
    if (!f.geometry) continue;
    const vs: Position[] = [];
    const walk = (c: unknown): void => {
      if (Array.isArray(c) && typeof c[0] === "number") vs.push(c as Position);
      else if (Array.isArray(c)) c.forEach(walk);
    };
    walk(f.geometry.coordinates);
    if (vs.length) out.push([vs.reduce((a, v) => a + v[0], 0) / vs.length, vs.reduce((a, v) => a + v[1], 0) / vs.length]);
  }
  return out;
}

/**
 * Frame for the report's second map, the main working area. Same rules as End
 * of Day: the densest box that is under a tenth of the day's area and holds a
 * quarter of the records; if nothing stands out, the tightest 80 %; and if
 * the work is spread evenly, the busiest half. Null under 5 records.
 */
export function detailBounds(features: Feature[]): [number, number, number, number] | null {
  const pts = recordPositions(features);
  if (pts.length < 5) return null;
  const xs = pts.map((p) => p[0]).sort((a, b) => a - b);
  const ys = pts.map((p) => p[1]).sort((a, b) => a - b);
  const kx = 111.32 * Math.cos(rad((ys[0] + ys[ys.length - 1]) / 2));
  const box = (pct: number) => {
    const [a, b] = shortestInterval(xs, pct);
    const [c, e] = shortestInterval(ys, pct);
    return { km2: (b - a) * kx * ((e - c) * 110.57), b: [a, c, b, e] as [number, number, number, number] };
  };
  const full = box(100).km2;
  const floor = (b: [number, number, number, number]): [number, number, number, number] => {
    let [w, s, e, n] = b;
    if (e - w < MIN_SPAN_DEG) { const m = (e + w) / 2; w = m - MIN_SPAN_DEG / 2; e = m + MIN_SPAN_DEG / 2; }
    if (n - s < MIN_SPAN_DEG) { const m = (n + s) / 2; s = m - MIN_SPAN_DEG / 2; n = m + MIN_SPAN_DEG / 2; }
    return [w, s, e, n];
  };

  let best: { density: number; km2: number; b: [number, number, number, number] } | null = null;
  for (const pct of DETAIL_STEPS) {
    const { km2, b } = box(pct);
    if (km2 <= 0) continue;
    const inside = pts.filter((p) => p[0] >= b[0] && p[0] <= b[2] && p[1] >= b[1] && p[1] <= b[3]).length;
    if (inside < pts.length * DETAIL_MIN_SHARE) continue;
    const density = inside / km2;
    if (!best || density > best.density) best = { density, km2, b };
  }
  if (best && full > 0 && best.km2 <= full * DETAIL_AREA_RATIO) return floor(best.b);

  // No separate cluster: zoom in modestly on the main working area.
  const fb = box(DETAIL_FALLBACK);
  if (full > 0 && fb.km2 <= full * 0.5) return floor(fb.b);
  // Evenly spread (End of Day drew no detail map here). Frame the busiest
  // half of the records instead, so the report always has a closer view.
  return floor(box(50).b);
}
