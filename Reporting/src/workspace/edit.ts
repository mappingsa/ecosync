// Drawing and reshaping, kept apart from the map so it can be tested.
//
// Every editable shape is held the same way: parts -> rings -> points, with
// polygon rings OPEN (no repeated closing point) while editing.
//   point:   [[[pt]]]
//   line:    one ring per part        [[path], [path]...]
//   area:    rings per part           [[outer, hole...], [outer]...]
// A vertex address {p, r, i} then works for all three.

import type { Geometry, Position } from "../data/types";

export type EditLayer = "points" | "lines" | "polygons";
export type Pt = [number, number];
export interface Shape {
  layer: EditLayer;
  parts: Pt[][][];
}
export interface Addr {
  p: number;
  r: number;
  i: number;
}

const same = (a: Position, b: Position) => a[0] === b[0] && a[1] === b[1];
const open = (ring: Position[]): Pt[] => {
  const r = ring.map((q) => [q[0], q[1]] as Pt);
  return r.length > 1 && same(r[0], r[r.length - 1]) ? r.slice(0, -1) : r;
};
const close = (ring: Pt[]): Pt[] => (ring.length ? [...ring, ring[0]] : ring);
const copy = (s: Shape): Shape => ({ layer: s.layer, parts: s.parts.map((p) => p.map((r) => r.map((q) => [q[0], q[1]] as Pt))) });

/** The fewest points a ring may have. */
export const minPoints = (layer: EditLayer) => (layer === "points" ? 1 : layer === "lines" ? 2 : 3);

export function fromGeometry(g: Geometry, layer: EditLayer): Shape {
  switch (g.type) {
    case "Point": return { layer, parts: [[[[g.coordinates[0], g.coordinates[1]]]]] };
    case "LineString": return { layer, parts: [[open(g.coordinates)]] };
    case "MultiLineString": return { layer, parts: g.coordinates.map((l) => [open(l)]) };
    case "Polygon": return { layer, parts: [g.coordinates.map(open)] };
    case "MultiPolygon": return { layer, parts: g.coordinates.map((poly) => poly.map(open)) };
  }
}

/** A new shape from the points tapped while drawing. */
export function fromDrawing(layer: EditLayer, pts: Pt[]): Shape {
  return layer === "points" ? { layer, parts: [[[pts[0]]]] } : { layer, parts: [[pts.map((q) => [q[0], q[1]] as Pt)]] };
}

export function isValid(s: Shape): boolean {
  const min = minPoints(s.layer);
  return s.parts.length > 0 && s.parts.every((p) => p.length > 0 && p.every((r) => r.length >= min));
}

/** GeoJSON to draw on the map (single-part shapes come back as single types). */
export function toGeometry(s: Shape): Geometry {
  if (s.layer === "points") return { type: "Point", coordinates: s.parts[0][0][0] };
  if (s.layer === "lines") {
    const paths = s.parts.map((p) => p[0]);
    return paths.length === 1 ? { type: "LineString", coordinates: paths[0] } : { type: "MultiLineString", coordinates: paths };
  }
  const polys = s.parts.map((p) => p.map(close));
  return polys.length === 1 ? { type: "Polygon", coordinates: polys[0] } : { type: "MultiPolygon", coordinates: polys };
}

/** For saving: EWKT in the database's column types (lines and areas are always MULTI). */
export function toEwkt(s: Shape): string {
  const n = (v: number) => String(Math.round(v * 1e8) / 1e8);
  const pt = (q: Pt) => `${n(q[0])} ${n(q[1])}`;
  const path = (r: Pt[]) => `(${r.map(pt).join(",")})`;
  if (s.layer === "points") return `SRID=4326;POINT(${pt(s.parts[0][0][0])})`;
  if (s.layer === "lines") return `SRID=4326;MULTILINESTRING(${s.parts.map((p) => path(p[0])).join(",")})`;
  return `SRID=4326;MULTIPOLYGON(${s.parts.map((p) => `(${p.map((r) => path(close(r))).join(",")})`).join(",")})`;
}

export interface Handle {
  addr: Addr;
  pt: Pt;
}

/** Every point of the shape, to drag. */
export function vertices(s: Shape): Handle[] {
  const out: Handle[] = [];
  s.parts.forEach((part, p) => part.forEach((ring, r) => ring.forEach((pt, i) => out.push({ addr: { p, r, i }, pt }))));
  return out;
}

/** A handle halfway along each side; dragging one adds a point after addr.i. */
export function midpoints(s: Shape): Handle[] {
  if (s.layer === "points") return [];
  const out: Handle[] = [];
  s.parts.forEach((part, p) => part.forEach((ring, r) => {
    const sides = s.layer === "polygons" ? ring.length : ring.length - 1;
    for (let i = 0; i < sides; i++) {
      const a = ring[i], b = ring[(i + 1) % ring.length];
      out.push({ addr: { p, r, i }, pt: [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2] });
    }
  }));
  return out;
}

export function moveVertex(s: Shape, a: Addr, pt: Pt): Shape {
  const c = copy(s);
  c.parts[a.p][a.r][a.i] = [pt[0], pt[1]];
  return c;
}

/** Adds pt after a.i; the new point is at {p, r, i: a.i + 1}. */
export function insertVertex(s: Shape, a: Addr, pt: Pt): Shape {
  const c = copy(s);
  c.parts[a.p][a.r].splice(a.i + 1, 0, [pt[0], pt[1]]);
  return c;
}

/** Removes a point, unless that would leave too few. Returns null if not allowed. */
export function deleteVertex(s: Shape, a: Addr): Shape | null {
  if (s.parts[a.p][a.r].length <= minPoints(s.layer)) return null;
  const c = copy(s);
  c.parts[a.p][a.r].splice(a.i, 1);
  return c;
}

export const addrKey = (a: Addr) => `${a.p}.${a.r}.${a.i}`;
export const parseAddr = (k: string): Addr => {
  const [p, r, i] = k.split(".").map(Number);
  return { p, r, i };
};

// --- local time for the edit form ---------------------------------------------------

/** Minutes east of UTC that a time zone is at a given moment (Adelaide: 570 or 630). */
function offsetMinutes(ms: number, timeZone: string): number {
  const f = new Intl.DateTimeFormat("en-US", {
    timeZone, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit",
  }).formatToParts(new Date(ms));
  const v = (t: string) => Number(f.find((x) => x.type === t)!.value);
  const asUtc = Date.UTC(v("year"), v("month") - 1, v("day"), v("hour"), v("minute"), v("second"));
  return Math.round((asUtc - Math.floor(ms / 1000) * 1000) / 60000);
}

/** epoch ms -> "YYYY-MM-DDTHH:mm" in the time zone (for <input type="datetime-local">). */
export function toLocalInput(ms: number, timeZone: string): string {
  const d = new Date(ms + offsetMinutes(ms, timeZone) * 60000);
  return d.toISOString().slice(0, 16);
}

/** "YYYY-MM-DDTHH:mm" in the time zone -> epoch ms. */
export function fromLocalInput(s: string, timeZone: string): number {
  const m = s.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})/);
  if (!m) return NaN;
  const wall = Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5]);
  // guess with the offset at that wall time, then correct once (handles daylight saving)
  let t = wall - offsetMinutes(wall, timeZone) * 60000;
  t = wall - offsetMinutes(t, timeZone) * 60000;
  return t;
}
