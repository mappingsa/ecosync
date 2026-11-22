import { test } from "node:test";
import assert from "node:assert/strict";
import {
  deleteVertex, fromDrawing, fromGeometry, fromLocalInput, insertVertex, isValid, midpoints, moveVertex, toEwkt, toGeometry,
  toLocalInput, vertices, addrKey, parseAddr,
} from "./edit";

const sq = [[140.6, -34.3], [140.61, -34.3], [140.61, -34.31], [140.6, -34.31], [140.6, -34.3]] as [number, number][];

test("shapes in and out keep their form; rings are open while editing", () => {
  const poly = fromGeometry({ type: "Polygon", coordinates: [sq] }, "polygons");
  assert.equal(poly.parts[0][0].length, 4);
  assert.deepEqual(toGeometry(poly), { type: "Polygon", coordinates: [sq] });
  const ml = fromGeometry({ type: "MultiLineString", coordinates: [[[1, 2], [3, 4]], [[5, 6], [7, 8]]] }, "lines");
  assert.equal(ml.parts.length, 2);
  assert.equal(toGeometry(ml).type, "MultiLineString");
  assert.deepEqual(toGeometry(fromGeometry({ type: "Point", coordinates: [1, 2] }, "points")), { type: "Point", coordinates: [1, 2] });
});

test("EWKT matches the database column types", () => {
  assert.equal(toEwkt(fromDrawing("points", [[140.6, -34.3]])), "SRID=4326;POINT(140.6 -34.3)");
  assert.equal(toEwkt(fromDrawing("lines", [[1, 2], [3, 4]])), "SRID=4326;MULTILINESTRING((1 2,3 4))");
  assert.equal(toEwkt(fromDrawing("polygons", [[0, 0], [1, 0], [1, 1]])), "SRID=4326;MULTIPOLYGON(((0 0,1 0,1 1,0 0)))");
  assert.equal(toEwkt(fromDrawing("points", [[140.123456789123, -34.1]])), "SRID=4326;POINT(140.12345679 -34.1)");
});

test("validity needs enough points", () => {
  assert.equal(isValid(fromDrawing("lines", [[1, 2]])), false);
  assert.equal(isValid(fromDrawing("lines", [[1, 2], [3, 4]])), true);
  assert.equal(isValid(fromDrawing("polygons", [[0, 0], [1, 0]])), false);
  assert.equal(isValid(fromDrawing("polygons", [[0, 0], [1, 0], [1, 1]])), true);
});

test("handles: a vertex for each point, a midpoint on each side (areas wrap round)", () => {
  const area = fromDrawing("polygons", [[0, 0], [2, 0], [2, 2]]);
  assert.equal(vertices(area).length, 3);
  assert.deepEqual(midpoints(area).map((h) => h.pt), [[1, 0], [2, 1], [1, 1]]);
  const line = fromDrawing("lines", [[0, 0], [2, 0], [2, 2]]);
  assert.equal(midpoints(line).length, 2);
  assert.equal(midpoints(fromDrawing("points", [[0, 0]])).length, 0);
});

test("move, insert and delete points without changing the original", () => {
  const a = fromDrawing("polygons", [[0, 0], [2, 0], [2, 2], [0, 2]]);
  const moved = moveVertex(a, { p: 0, r: 0, i: 1 }, [3, 0]);
  assert.deepEqual(moved.parts[0][0][1], [3, 0]);
  assert.deepEqual(a.parts[0][0][1], [2, 0]);
  const ins = insertVertex(a, { p: 0, r: 0, i: 1 }, [2, 1]);
  assert.deepEqual(ins.parts[0][0], [[0, 0], [2, 0], [2, 1], [2, 2], [0, 2]]);
  const del = deleteVertex(a, { p: 0, r: 0, i: 0 })!;
  assert.deepEqual(del.parts[0][0], [[2, 0], [2, 2], [0, 2]]);
  assert.equal(deleteVertex(del, { p: 0, r: 0, i: 0 }), null); // a triangle can't lose a corner
  assert.equal(deleteVertex(fromDrawing("lines", [[0, 0], [1, 1]]), { p: 0, r: 0, i: 0 }), null);
  assert.deepEqual(parseAddr(addrKey({ p: 1, r: 2, i: 3 })), { p: 1, r: 2, i: 3 });
});

test("local time in Adelaide both ways, across daylight saving", () => {
  const tz = "Australia/Adelaide";
  const summer = Date.UTC(2026, 10, 18, 0, 0); // 18 Nov 2026 10:30 ACDT
  assert.equal(toLocalInput(summer, tz), "2026-11-18T10:30");
  assert.equal(fromLocalInput("2026-11-18T10:30", tz), summer);
  const winter = Date.UTC(2026, 5, 18, 0, 0); // 18 Jun 2026 09:30 ACST
  assert.equal(toLocalInput(winter, tz), "2026-06-18T09:30");
  assert.equal(fromLocalInput("2026-06-18T09:30", tz), winter);
  assert.ok(Number.isNaN(fromLocalInput("rubbish", tz)));
});
