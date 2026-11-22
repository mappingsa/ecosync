import { test } from "node:test";
import assert from "node:assert/strict";
import type { Feature, TargetRow } from "../data/types";
import { applyFilters, facets, measure, NO_FILTERS, sortRows, toCsv, toGeoJson, toRow, totals, type Item } from "./logic";

const targets: TargetRow[] = [{ species: "African Boxthorn", control_method: "Cut and swab", colour: "#2E7D32" }];
const pt = (id: string, job: string | null, species: string, method: string | null, t: number, acc: number | null = 0.8): Item => ({
  layer: "points",
  f: { type: "Feature", geometry: { type: "Point", coordinates: [136.1, -34.2] },
    properties: { id, Job_Number: job, weed_species: species, control_type: method, treatment_time: t, gps_accuracy_m: acc } },
});
// ~100 m square near Berri
const sq: Feature = { type: "Feature", properties: { id: "a1", Job_Number: "EP-C489.5", weed_species: "Gazania", control_type: "Spot Spray", treatment_time: 5 },
  geometry: { type: "Polygon", coordinates: [[[140.6, -34.3], [140.601087, -34.3], [140.601087, -34.300901], [140.6, -34.300901], [140.6, -34.3]]] } };
const track: Item = { layer: "tracks", f: { type: "Feature", properties: { id: "t1", Job_Number: "DEW-NP", treatment_time: 1 },
  geometry: { type: "MultiLineString", coordinates: [[[140.6, -34.3], [140.6, -34.31]]] } } };

const items: Item[] = [
  pt("p1", "EP-C489.5", "African Boxthorn", null, 3),       // method from targets
  pt("p2", "EP-C489.5", "African Boxthorn", "Spot Spray", 1, null),
  pt("p3", null, "Gazania", "Spot Spray", 2),
  { layer: "polygons", f: sq },
  track,
];

test("facets count jobs over every layer, species and methods over treatments only", () => {
  const f = facets(items, targets);
  assert.deepEqual(f.jobs, [["(no job)", 1], ["DEW-NP", 1], ["EP-C489.5", 3]]);
  assert.deepEqual(f.species, [["African Boxthorn", 2], ["Gazania", 2]]);
  assert.deepEqual(f.methods, [["Cut and swab", 1], ["Spot Spray", 3]]);
});

test("filters combine; the track stays whatever species is picked", () => {
  assert.equal(applyFilters(items, NO_FILTERS, targets).length, 5);
  const box = applyFilters(items, { ...NO_FILTERS, species: ["African Boxthorn"] }, targets);
  assert.deepEqual(box.map((i) => i.f.properties.id), ["p1", "p2", "t1"]);
  const cut = applyFilters(items, { jobs: ["EP-C489.5"], species: [], methods: ["Cut and swab"] }, targets);
  assert.deepEqual(cut.map((i) => i.f.properties.id), ["p1"]);
  assert.deepEqual(applyFilters(items, { ...NO_FILTERS, jobs: ["(no job)"] }, targets).map((i) => i.f.properties.id), ["p3"]);
});

test("table rows sort both ways with blanks last", () => {
  const rows = items.map((i) => toRow(i, targets));
  assert.deepEqual(sortRows(rows, "time", true).map((r) => r.key), ["points:p2", "tracks:t1", "points:p3", "points:p1", "polygons:a1"]);
  assert.deepEqual(sortRows(rows, "accuracy", false).map((r) => r.key).slice(-3), ["points:p2", "polygons:a1", "tracks:t1"]);
  assert.equal(rows[3].size! > 9_800 && rows[3].size! < 10_200, true);   // the 1 ha square
  assert.equal(rows[4].species, "");                                     // tracks have no species
});

test("totals", () => {
  const t = totals(items);
  assert.equal(t.points, 3);
  assert.equal(t.areas, 1);
  assert.ok(Math.abs(t.areaM2 - 10_000) < 200);
  assert.equal(t.tracks, 1);
  assert.ok(Math.abs(t.trackM - 1109) < 10);   // 0.01 degree of latitude
});

test("measuring a line and a closed shape", () => {
  assert.deepEqual(measure([]), { lengthM: 0, areaM2: null });
  const two = measure([[140.6, -34.3], [140.6, -34.31]]);
  assert.ok(Math.abs(two.lengthM - 1109) < 10);
  assert.equal(two.areaM2, null);
  const sqm = measure([[140.6, -34.3], [140.601087, -34.3], [140.601087, -34.300901], [140.6, -34.300901]]);
  assert.ok(Math.abs(sqm.areaM2! - 10_000) < 200);
});

test("CSV: local time, quoting, middle of an area", () => {
  const withComma = pt("p9", "EP-C489.5", "Boxthorn, African", "Spot Spray", Date.UTC(2026, 10, 18, 0, 0, 0));
  const csv = toCsv([withComma, { layer: "polygons", f: sq }], targets, "Australia/Adelaide").split("\r\n");
  assert.equal(csv[0], "layer,job,species,method,time_local,accuracy_m,length_m,area_m2,lon,lat,id");
  assert.equal(csv[1], 'Points,EP-C489.5,"Boxthorn, African",Spot Spray,2026-11-18 10:30:00,0.8,,,136.1000000,-34.2000000,p9');
  assert.match(csv[2], /^Areas,EP-C489\.5,Gazania,Spot Spray,.*,\d+\.\d,140\.6005435,-34\.3004505,a1$/);
});

test("GeoJSON keeps the shapes and tidy properties", () => {
  const g = JSON.parse(toGeoJson(items, targets, "Australia/Adelaide"));
  assert.equal(g.features.length, 5);
  assert.equal(g.features[0].properties.method, "Cut and swab");
  assert.equal(g.features[4].properties.layer, "Track");
  assert.equal(g.features[4].properties.species, null);
  assert.ok(g.features[3].properties.area_m2 > 9_800);
});

test("scale, coordinates and coordinate search", async () => {
  const { scaleDenominator, fmtScale, fmtCoord, parseLatLon } = await import("./logic");
  assert.ok(Math.abs(scaleDenominator(0, 0) - 591_658_000) < 5000);
  assert.equal(fmtScale(1234.5), "1:1,200");
  assert.equal(fmtScale(512), "1:512");
  assert.equal(fmtCoord([140.6, -34.3]), "34.30000° S, 140.60000° E");
  assert.deepEqual(parseLatLon("-34.30, 140.60"), [140.6, -34.3]);
  assert.deepEqual(parseLatLon("34.3 S 140.6 E"), [140.6, -34.3]);
  assert.deepEqual(parseLatLon("140.6 -34.3"), [140.6, -34.3]);
  assert.equal(parseLatLon("EP-C489.5"), null);
  assert.equal(parseLatLon("200, 300"), null);
});
