// The map workspace's logic, kept apart from the screen so it can be tested:
// filters, the attribute table, measuring, and downloads.

import type { Feature, TargetRow } from "../data/types";
import type { WorkspaceLayer } from "../data/platform";
import { areaM2, attr, bounds, lengthM, methodOf, speciesOf } from "../report/stats";

export interface Item {
  layer: WorkspaceLayer;
  f: Feature;
}

export interface Filters {
  jobs: string[]; // empty = all
  species: string[];
  methods: string[];
}

export const NO_FILTERS: Filters = { jobs: [], species: [], methods: [] };

export const LAYER_NAMES: Record<WorkspaceLayer, string> = {
  points: "Points", lines: "Lines", polygons: "Areas", tracks: "Track",
};

/** Tracks have a job but no species or method. */
const treated = (l: WorkspaceLayer) => l !== "tracks";

export const jobOf = (it: Item) => String(attr(it.f.properties, "Job_Number") ?? "").trim() || "(no job)";
export const keyOf = (it: Item) => `${it.layer}:${String(attr(it.f.properties, "id") ?? "")}`;

export type Facet = [value: string, count: number][];

/** The values present in the loaded records, with how many of each, for the filter lists. */
export function facets(items: Item[], targets: TargetRow[]): { jobs: Facet; species: Facet; methods: Facet } {
  const count = (vals: string[]): Facet => {
    const m = new Map<string, number>();
    for (const v of vals) m.set(v, (m.get(v) ?? 0) + 1);
    return [...m.entries()].sort((a, b) => a[0].localeCompare(b[0]));
  };
  const t = items.filter((i) => treated(i.layer));
  return {
    jobs: count(items.map(jobOf)),
    species: count(t.map((i) => speciesOf(i.f.properties))),
    methods: count(t.map((i) => methodOf(i.f.properties, targets))),
  };
}

export function applyFilters(items: Item[], fl: Filters, targets: TargetRow[]): Item[] {
  const jobs = new Set(fl.jobs), species = new Set(fl.species), methods = new Set(fl.methods);
  return items.filter((i) => {
    if (jobs.size && !jobs.has(jobOf(i))) return false;
    if (!treated(i.layer)) return true; // the track stays as context whatever species is picked
    if (species.size && !species.has(speciesOf(i.f.properties))) return false;
    if (methods.size && !methods.has(methodOf(i.f.properties, targets))) return false;
    return true;
  });
}

// --- attribute table ------------------------------------------------------------

export interface Row {
  key: string;
  layer: WorkspaceLayer;
  job: string;
  species: string;
  method: string;
  time: number; // epoch ms
  accuracy: number | null;
  size: number | null; // metres for lines and tracks, square metres for areas
}

export function toRow(it: Item, targets: TargetRow[]): Row {
  const p = it.f.properties;
  const acc = Number(attr(p, "gps_accuracy_m"));
  return {
    key: keyOf(it),
    layer: it.layer,
    job: jobOf(it),
    species: treated(it.layer) ? speciesOf(p) : "",
    method: treated(it.layer) ? methodOf(p, targets) : "",
    time: Number(attr(p, "treatment_time") ?? 0),
    accuracy: attr(p, "gps_accuracy_m") == null || !Number.isFinite(acc) ? null : acc,
    size: it.layer === "polygons" ? areaM2(it.f) : it.layer === "lines" || it.layer === "tracks" ? lengthM(it.f) : null,
  };
}

export type SortKey = keyof Omit<Row, "key">;

export function sortRows(rows: Row[], key: SortKey, asc: boolean): Row[] {
  const dir = asc ? 1 : -1;
  return [...rows].sort((a, b) => {
    const x = a[key], y = b[key];
    if (x == null && y == null) return 0;
    if (x == null) return 1; // blanks last either way
    if (y == null) return -1;
    return (typeof x === "number" && typeof y === "number" ? x - y : String(x).localeCompare(String(y))) * dir;
  });
}

// --- totals and measuring --------------------------------------------------------

export function totals(items: Item[]) {
  let points = 0, lines = 0, lengthMSum = 0, areas = 0, areaSum = 0, tracks = 0, trackM = 0;
  for (const i of items) {
    if (i.layer === "points") points++;
    else if (i.layer === "lines") { lines++; lengthMSum += lengthM(i.f); }
    else if (i.layer === "polygons") { areas++; areaSum += areaM2(i.f); }
    else { tracks++; trackM += lengthM(i.f); }
  }
  return { points, lines, lengthM: lengthMSum, areas, areaM2: areaSum, tracks, trackM };
}

/** Length of a drawn line, and area if it is closed into a shape (3+ points). */
export function measure(coords: [number, number][]): { lengthM: number; areaM2: number | null } {
  const line: Feature = { type: "Feature", properties: {}, geometry: { type: "LineString", coordinates: coords } };
  const ring = coords.length >= 3 ? [...coords, coords[0]] : null;
  return {
    lengthM: coords.length >= 2 ? lengthM(line) : 0,
    areaM2: ring ? areaM2({ type: "Feature", properties: {}, geometry: { type: "Polygon", coordinates: [ring] } }) : null,
  };
}

export const fmtLength = (m: number) => (m >= 1000 ? `${(m / 1000).toFixed(2)} km` : `${m.toFixed(0)} m`);
export const fmtArea = (m2: number) => (m2 >= 10000 ? `${(m2 / 10000).toFixed(2)} ha` : `${m2.toFixed(0)} m²`);

// --- downloads ---------------------------------------------------------------------

const localTime = (ms: number, timeZone: string) =>
  ms ? new Intl.DateTimeFormat("en-CA", {
    timeZone, year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
  }).format(new Date(ms)).replace(", ", " ") : "";

const csvCell = (v: unknown) => {
  const s = v == null ? "" : String(v);
  return /[",\n\r]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
};

/** One row per record; lon/lat is the point, or the middle of a line or area. */
export function toCsv(items: Item[], targets: TargetRow[], timeZone: string): string {
  const head = ["layer", "job", "species", "method", "time_local", "accuracy_m", "length_m", "area_m2", "lon", "lat", "id"];
  const lines = [head.join(",")];
  for (const it of items) {
    const r = toRow(it, targets);
    const b = bounds([it.f]);
    lines.push([
      LAYER_NAMES[it.layer], r.job, r.species, r.method, localTime(r.time, timeZone), r.accuracy,
      it.layer === "lines" || it.layer === "tracks" ? r.size?.toFixed(1) : "",
      it.layer === "polygons" ? r.size?.toFixed(1) : "",
      b ? ((b[0] + b[2]) / 2).toFixed(7) : "", b ? ((b[1] + b[3]) / 2).toFixed(7) : "",
      attr(it.f.properties, "id"),
    ].map(csvCell).join(","));
  }
  return lines.join("\r\n") + "\r\n";
}

export function toGeoJson(items: Item[], targets: TargetRow[], timeZone: string): string {
  return JSON.stringify({
    type: "FeatureCollection",
    features: items.filter((i) => i.f.geometry).map((it) => {
      const r = toRow(it, targets);
      return {
        type: "Feature",
        geometry: it.f.geometry,
        properties: {
          layer: LAYER_NAMES[it.layer], job: r.job, species: r.species || null, method: r.method || null,
          time_local: localTime(r.time, timeZone), accuracy_m: r.accuracy,
          ...(it.layer === "polygons" ? { area_m2: Number(r.size?.toFixed(1)) } : {}),
          ...(it.layer === "lines" || it.layer === "tracks" ? { length_m: Number(r.size?.toFixed(1)) } : {}),
          id: attr(it.f.properties, "id") ?? null,
        },
      };
    }),
  });
}

// --- status bar and search -----------------------------------------------------------

/** Map scale 1:N at a latitude and zoom (Web Mercator, 96 dpi screen). */
export function scaleDenominator(lat: number, zoom: number): number {
  const metresPerPixel = (156543.03392 * Math.cos((lat * Math.PI) / 180)) / 2 ** zoom;
  return metresPerPixel * (96 / 0.0254);
}

export const fmtScale = (n: number) => `1:${Math.round(n > 1000 ? n / 100 : n) * (n > 1000 ? 100 : 1)}`.replace(/\B(?=(\d{3})+(?!\d))/g, ",");

export const fmtCoord = ([lon, lat]: [number, number]) =>
  `${Math.abs(lat).toFixed(5)}° ${lat < 0 ? "S" : "N"}, ${Math.abs(lon).toFixed(5)}° ${lon < 0 ? "W" : "E"}`;

/**
 * "-34.30, 140.60", "34.3 S 140.6 E", "140.6 -34.3" -> [lon, lat]. Australian
 * data, so a pair is read whichever way round makes a sensible lat / lon.
 */
export function parseLatLon(text: string): [number, number] | null {
  const t = text.trim().toUpperCase();
  const m = t.match(/^(-?\d+(?:\.\d+)?)\s*°?\s*([NS])?\s*[,\s]\s*(-?\d+(?:\.\d+)?)\s*°?\s*([EW])?$/);
  if (!m) return null;
  let a = Number(m[1]), b = Number(m[3]);
  if (m[2] === "S") a = -Math.abs(a);
  if (m[2] === "N") a = Math.abs(a);
  if (m[4] === "W") b = -Math.abs(b);
  if (m[4] === "E") b = Math.abs(b);
  const ok = (lat: number, lon: number) => Math.abs(lat) <= 90 && Math.abs(lon) <= 180;
  if (Math.abs(a) <= 90 && ok(a, b) && !(Math.abs(b) <= 90 && Math.abs(a) > 90)) return [b, a]; // lat, lon
  if (ok(b, a)) return [a, b];                                                                     // lon, lat
  return null;
}
