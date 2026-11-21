import { test } from "node:test";
import assert from "node:assert/strict";
import { accuracy, areaM2, canonicalJob, dayWindow, jobsIn, lengthM, sameJob, summarise, forJob } from "./stats";
import { esriToGeometry } from "../data/arcgis";
import type { Feature } from "../data/types";

const pt = (p: Record<string, unknown>): Feature => ({ type: "Feature", geometry: { type: "Point", coordinates: [140, -34] }, properties: p });

test("day window: summer day is ACDT (+10:30), 24 h long", () => {
  const [a, b] = dayWindow("2026-11-20", "Australia/Adelaide");
  assert.equal(new Date(a).toISOString(), "2026-11-19T13:30:00.000Z");
  assert.equal(b - a, 24 * 3600e3);
});

test("day window: winter day is ACST (+09:30)", () => {
  const [a] = dayWindow("2026-07-01", "Australia/Adelaide");
  assert.equal(new Date(a).toISOString(), "2026-06-30T14:30:00.000Z");
});

test("day window: daylight-saving start (4 Oct 2026) is 23 h", () => {
  const [a, b] = dayWindow("2026-10-04", "Australia/Adelaide");
  assert.equal(b - a, 23 * 3600e3);
});

test("accuracy floors the within-3 m percentage and ignores missing values", () => {
  const feats = [...Array(789)].map(() => pt({ gps_accuracy_m: 1.2 }));
  feats.push(pt({ gps_accuracy_m: 4.0 }), pt({}));
  const a = accuracy(feats, 3);
  assert.equal(a.captured, 791);
  assert.equal(a.measured, 790);
  assert.equal(a.withinPct, 99);
  assert.equal(a.worst, 4);
});

test("job matching ignores case and stray spaces; blanks are counted", () => {
  const feats = [pt({ Job_Number: "Riverland Boxthorn Control " }), pt({ job_number: "riverland  boxthorn control" }), pt({ Job_Number: "" })];
  const { jobs, blank } = jobsIn(feats);
  assert.deepEqual(jobs, ["Riverland Boxthorn Control"]);
  assert.equal(blank, 1);
  assert.equal(forJob(feats, "RIVERLAND BOXTHORN CONTROL").length, 2);
});

test("area of a 100 m square near Berri is about 1 ha", () => {
  const lat = -34.28, lon = 140.6;
  const dLat = 100 / 111320, dLon = 100 / (111320 * Math.cos((lat * Math.PI) / 180));
  // Esri outer ring, clockwise
  const g = esriToGeometry({ rings: [[[lon, lat], [lon, lat + dLat], [lon + dLon, lat + dLat], [lon + dLon, lat], [lon, lat]]] });
  assert.equal(g?.type, "Polygon");
  const m2 = areaM2({ type: "Feature", geometry: g, properties: {} });
  assert.ok(Math.abs(m2 - 10000) < 150, `got ${m2}`);
});

test("esri polygon with a hole subtracts it", () => {
  const outer = [[0, 0], [0, 0.001], [0.001, 0.001], [0.001, 0], [0, 0]];
  const hole = [[0.0004, 0.0004], [0.0006, 0.0004], [0.0006, 0.0006], [0.0004, 0.0006], [0.0004, 0.0004]];
  const solid = areaM2({ type: "Feature", geometry: esriToGeometry({ rings: [outer] }), properties: {} });
  const holed = areaM2({ type: "Feature", geometry: esriToGeometry({ rings: [outer, hole] }), properties: {} });
  assert.ok(Math.abs(holed / solid - 0.96) < 0.001);
});

test("line length of 0.01 degree of latitude is about 1.1 km", () => {
  const g = esriToGeometry({ paths: [[[140, -34], [140, -34.01]]] });
  const m = lengthM({ type: "Feature", geometry: g, properties: {} });
  assert.ok(Math.abs(m - 1113) < 5, `got ${m}`);
});

test("summary groups by species and control method", () => {
  const rows = summarise(
    [pt({ weed_species: "African Boxthorn", control_type: "Cut and swab" }), pt({ weed_species: "African Boxthorn", control_type: "Cut and swab" }), pt({ weed_species: "Gazania" })],
    [],
    [],
  );
  assert.deepEqual(rows.map((r) => [r.species, r.method, r.points]), [
    ["African Boxthorn", "Cut and swab", 2],
    ["Gazania", "Spray", 1],
  ]);
});

test("blank control type falls back to the Targets table", () => {
  const rows = summarise([pt({ weed_species: "Boneseed" })], [], [], [{ species: "boneseed", control_method: "Cut and swab", colour: "#E9ED09" }]);
  assert.equal(rows[0].method, "Cut and swab");
});

test("aliases: an old Form 1 job name matches the renamed job", () => {
  const jobs = [{ job_number: "DEW-NP", aliases: "DEW NP Weed Control; DEW National Parks" }];
  assert.equal(canonicalJob("DEW NP Weed Control ", jobs), "DEW-NP");
  assert.ok(sameJob("dew np weed control", "DEW-NP", jobs));
  assert.ok(!sameJob("EP-C489.2", "DEW-NP", jobs));
  assert.equal(canonicalJob("New Job 1", jobs), "New Job 1");
  const { jobs: listed } = jobsIn([pt({ Job_Number: "DEW NP Weed Control" }), pt({ Job_Number: "DEW-NP" })], jobs);
  assert.deepEqual(listed, ["DEW-NP"]);
});

test("colour follows species AND method", async () => {
  const { colourMap, colourKey } = await import("./colours");
  const targets = [
    { species: "African Boxthorn", control_method: "Spray", colour: "#E31A1C" },
    { species: "African Boxthorn", control_method: "Cut and swab", colour: "#63D91E" },
  ];
  const m = colourMap([{ species: "African Boxthorn", method: "Cut and swab" }, { species: "African Boxthorn", method: "Spray" }], targets);
  assert.equal(m.get(colourKey("African Boxthorn", "Cut and swab")), "#63D91E");
  assert.equal(m.get(colourKey("African Boxthorn", "Spray")), "#E31A1C");
});

test("one Targets row can cover several methods", async () => {
  const { colourMap, colourKey } = await import("./colours");
  const targets = [
    { species: "African Boxthorn", control_method: "Cut and swab", colour: "#63D91E" },
    { species: "African Boxthorn", control_method: "Spot Spray; Foliar spray", colour: "#E31A1C" },
  ];
  const m = colourMap([{ species: "African Boxthorn", method: "Foliar Spray" }, { species: "African Boxthorn", method: "Spot Spray" }, { species: "African Boxthorn", method: "Cut and swab" }], targets);
  assert.equal(m.get(colourKey("African Boxthorn", "Foliar Spray")), "#E31A1C");
  assert.equal(m.get(colourKey("African Boxthorn", "Spot Spray")), "#E31A1C");
  assert.equal(m.get(colourKey("African Boxthorn", "Cut and swab")), "#63D91E");
});
