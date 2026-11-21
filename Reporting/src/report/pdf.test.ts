import { test } from "node:test";
import assert from "node:assert/strict";
import { PDFDocument } from "pdf-lib";
import { buildReportPdf, pdfSafe } from "./pdf";
import { detailBounds } from "./stats";
import { emptyRecord } from "../data/dailySchema";
import type { Feature } from "../data/types";

test("text the standard PDF fonts cannot draw is folded, not fatal", () => {
  assert.equal(pdfSafe("Alinytjara Wiluṟara"), "Alinytjara Wilurara");
  assert.equal(pdfSafe("28.5 °C – “ok”"), '28.5 °C – "ok"');
});

test("a report with no maps, photos, logo or signature still builds", async () => {
  const bytes = await buildReportPdf({
    entityName: "FieldPoint Environmental", logo: null, jobNumber: "AW-2628", client: "Alinytjara Wiluṟara Landscape Board",
    address: "", ownerContact: "", reportTitle: "", dateLabel: "21/11/2026", record: emptyRecord("AW-2628", "2026-11-21"),
    warnings: [], rows: [], areaHa: 0, counts: { points: 0, lines: 0, polygons: 0 },
    accuracy: { captured: 0, measured: 0, average: null, worst: null, withinPct: null }, thresholdM: 3, receiver: "",
    overviewMap: null, detailMap: null, legend: [], photos: [], signature: null, issuedBy: "test", issuedAt: "now", version: 1,
  });
  const doc = await PDFDocument.load(bytes);
  assert.ok(doc.getPageCount() >= 1);
  assert.equal(doc.getPage(0).getWidth().toFixed(0), "595"); // A4 portrait
});

const pt = (x: number, y: number): Feature => ({ type: "Feature", geometry: { type: "Point", coordinates: [x, y] }, properties: {} });

test("detail map: a dense patch plus a road gets its own frame", () => {
  const patch = Array.from({ length: 60 }, (_, i) => pt(140 + (i % 10) * 0.0001, -34 + Math.floor(i / 10) * 0.0001));
  const road = Array.from({ length: 30 }, (_, i) => pt(140.02 + i * 0.01, -34.01));
  const d = detailBounds([...patch, ...road]);
  assert.ok(d && d[2] < 140.01, JSON.stringify(d));
});

test("detail map: evenly spread work still gets a closer view (busiest half)", () => {
  const road = Array.from({ length: 40 }, (_, i) => pt(140 + i * 0.01, -34));
  const d = detailBounds(road)!;
  assert.ok(d[2] - d[0] < 0.25, JSON.stringify(d)); // about half the 0.39 deg road
  assert.equal(detailBounds(road.slice(0, 3)), null);
});

test("detail map: one compact area still gets a modest zoom", () => {
  const grid = Array.from({ length: 49 }, (_, i) => pt(140 + (i % 7) * 0.002, -34 + Math.floor(i / 7) * 0.002));
  assert.ok(detailBounds(grid));
});
