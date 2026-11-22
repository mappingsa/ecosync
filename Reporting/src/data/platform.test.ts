import { test } from "node:test";
import assert from "node:assert/strict";
import { CONFIG } from "../config";
import { buildTargets, PlatformSource, recordToRow, rowToRecord, safeName, attachmentKind } from "./platform";
import { emptyRecord } from "./dailySchema";
import { colourMap } from "../report/colours";
import { methodOf } from "../report/stats";

CONFIG.platform.publishableKey = "sb_publishable_test";

test("targets keep the usual method first and per-method colours", () => {
  const methods = [{ id: "cut", name: "Cut and swab" }, { id: "spot", name: "Spot Spray" }];
  const species = [
    { id: "bx", name: "African Boxthorn", colour: "#2E7D32", default_method_id: "cut" },
    { id: "ga", name: "Gazania", colour: "#F39C12", default_method_id: null },
  ];
  const t = buildTargets(species, methods, [{ species_id: "bx", method_id: "spot", colour: "#C0392B" }]);
  assert.deepEqual(t, [
    { species: "African Boxthorn", control_method: "Cut and swab", colour: "#2E7D32" },
    { species: "African Boxthorn", control_method: "Spot Spray", colour: "#C0392B" },
    { species: "Gazania", control_method: "", colour: "#F39C12" },
  ]);
  const c = colourMap([
    { species: "African Boxthorn", method: "Cut and swab" },
    { species: "African Boxthorn", method: "Spot Spray" },
  ], t);
  assert.equal(c.get("african boxthorn|cut and swab"), "#2E7D32");
  assert.equal(c.get("african boxthorn|spot spray"), "#C0392B");
  assert.equal(methodOf({ weed_species: "African Boxthorn" }, t), "Cut and swab");
});

test("an override on the usual method colours that method", () => {
  const t = buildTargets([{ id: "s", name: "X", colour: "#111111", default_method_id: "m" }],
    [{ id: "m", name: "Spot Spray" }], [{ species_id: "s", method_id: "m", colour: "#222222" }]);
  assert.deepEqual(t, [{ species: "X", control_method: "Spot Spray", colour: "#222222" }]);
});

test("job sheet row <-> record", () => {
  const row = {
    id: "u1", record_date: "2026-11-18", letterhead: "FPE", start_time: "07:30", end_time: null,
    operators: ["Jayme Peltz", "Calvin"], equipment: [], chemicals: ["Glyphosate 10 mL/L"],
    tank_total_l: 400, start_delta_t: 4.2, end_delta_t: null, status: "issued", observations: null,
  };
  const r = rowToRecord(row, "EP-C489.5");
  assert.equal(r.objectId, "u1");
  assert.equal(r.job_number, "EP-C489.5");
  assert.equal(r.entity, "FPE");
  assert.equal(r.operators, "Jayme Peltz; Calvin");
  assert.equal(r.end_time, "");
  assert.equal(r.end_delta_t, null);
  const back = recordToRow(r);
  assert.deepEqual(back.operators, ["Jayme Peltz", "Calvin"]);
  assert.deepEqual(back.equipment, []);
  assert.equal(back.end_time, null);
  assert.equal(back.observations, null);
  assert.equal(back.letterhead, "FPE");
  assert.equal(back.tank_total_l, 400);
  assert.equal(back.status, "issued");
  assert.equal(recordToRow({ ...r, status: "anything", entity: " " }).status, "draft");
  assert.equal(recordToRow({ ...r, entity: " " }).letterhead, null);
});

test("names and kinds", () => {
  assert.equal(safeName("Spray Record EP-C489.5 2026-11-18 v2.pdf"), "Spray_Record_EP-C489.5_2026-11-18_v2.pdf");
  assert.equal(attachmentKind("signature.png"), "signature");
  assert.equal(attachmentKind("x.PDF"), "report");
  assert.equal(attachmentKind("photo.jpg"), "photo");
});

// --- against a fake server ---------------------------------------------------

type Call = { method: string; url: URL; body: any; headers: Record<string, string> };

function fakeServer(handler: (c: Call) => unknown) {
  const calls: Call[] = [];
  globalThis.fetch = (async (input: string, init: any = {}) => {
    const c: Call = {
      method: init.method ?? "GET",
      url: new URL(input),
      body: typeof init.body === "string" ? JSON.parse(init.body) : init.body,
      headers: init.headers ?? {},
    };
    calls.push(c);
    const out = handler(c);
    return new Response(out === undefined ? "" : JSON.stringify(out), { status: 200 });
  }) as typeof fetch;
  return calls;
}

const src = () => new PlatformSource(async () => "user-jwt", "user-1");
const JOBS = [{ id: "jA", job_number: "EP-C489.5", aliases: ["C489.5"] }, { id: "jB", job_number: "DEW-NP", aliases: ["DEW NP Weed Control"] }];

test("day features page through the view and keep ArcGIS field names", async () => {
  const many = Array.from({ length: 1000 }, (_, i) => ({
    id: `p${i}`, Job_Number: "EP-C489.5", weed_species: "African Boxthorn", control_type: "Spot Spray",
    gps_accuracy_m: 0.8, treatment_time: 1, geometry: { type: "Point", coordinates: [136, -34] },
  }));
  const calls = fakeServer((c) => (c.url.searchParams.get("offset") === "0" ? many : many.slice(0, 5)));
  const f = await src().dayFeatures("points", Date.UTC(2026, 10, 17, 13, 30), Date.UTC(2026, 10, 18, 13, 30));
  assert.equal(f.length, 1005);
  assert.deepEqual(f[0].geometry, { type: "Point", coordinates: [136, -34] });
  assert.equal(f[0].properties.Job_Number, "EP-C489.5");
  assert.equal("geometry" in f[0].properties, false);
  assert.equal(calls[0].url.pathname, "/rest/v1/report_points");
  assert.equal(calls[0].url.searchParams.get("and"), "(treated_at.gte.2026-11-17T13:30:00.000Z,treated_at.lt.2026-11-18T13:30:00.000Z)");
  assert.equal(calls[0].headers.Authorization, "Bearer user-jwt");
  assert.equal(calls[0].headers.apikey, "sb_publishable_test");
});

test("job sheet: found by alias, new one upserted, existing one patched", async () => {
  const calls = fakeServer((c) => {
    const t = c.url.pathname.split("/").pop();
    if (t === "jobs") return JOBS;
    if (t === "members") return [{ org_id: "org1" }];
    if (t === "daily_records" && c.method === "GET") return [{ id: "d1", record_date: "2026-11-20", operators: ["A"], letterhead: "MSA" }];
    if (t === "daily_records") return [{ id: c.method === "PATCH" ? "d1" : "d2" }];
    throw new Error("unexpected " + c.url);
  });
  const s = src();
  const r = await s.dailyRecord("DEW NP Weed Control", "2026-11-20");
  assert.equal(r?.job_number, "DEW-NP");
  assert.equal(r?.objectId, "d1");
  assert.equal(r?.entity, "MSA");
  const get = calls.find((c) => c.url.pathname.endsWith("daily_records"))!;
  assert.equal(get.url.searchParams.get("job_id"), "eq.jB");

  assert.equal(await s.saveDailyRecord({ ...r!, tank_total_l: 250 }), "d1");
  const patch = calls.find((c) => c.method === "PATCH")!;
  assert.equal(patch.url.searchParams.get("id"), "eq.d1");
  assert.equal(patch.body.source, "app");
  assert.equal(patch.body.org_id, "org1");
  assert.equal(patch.body.tank_total_l, 250);

  assert.equal(await s.saveDailyRecord(emptyRecord("EP-C489.5", "2026-11-22")), "d2");
  const post = calls.find((c) => c.method === "POST")!;
  assert.equal(post.url.searchParams.get("on_conflict"), "org_id,job_id,record_date");
  assert.equal(post.body.job_id, "jA");
  assert.match(post.headers.Prefer, /merge-duplicates/);

  await assert.rejects(s.saveDailyRecord(emptyRecord("EP-C999", "2026-11-22")), /isn't in the EcoSync database/);
});

test("attachments: signed links, upload, delete", async () => {
  const calls = fakeServer((c) => {
    const p = c.url.pathname;
    if (p.endsWith("/attachments") && p.startsWith("/rest") && c.method === "GET" && c.url.searchParams.get("daily_record_id")) {
      return [
        { id: "a1", name: "photo 1.jpg", content_type: "image/jpeg", size_bytes: 10, storage_path: "org1/d1/1_photo_1.jpg" },
        { id: "a2", name: "Spray Record v1.pdf", content_type: null, size_bytes: 20, storage_path: "org1/d1/2_Spray_Record_v1.pdf" },
      ];
    }
    if (p === "/storage/v1/object/sign/attachments") {
      return c.body.paths.map((x: string) => ({ path: x, signedURL: `/object/sign/attachments/${x}?token=t` }));
    }
    if (p.endsWith("/members")) return [{ org_id: "org1" }];
    if (p.endsWith("/attachments") && c.method === "GET") return [{ id: "a1", storage_path: "org1/d1/1_photo_1.jpg" }];
    return undefined;
  });
  const s = src();
  const atts = await s.attachments("d1");
  assert.deepEqual(atts.map((a) => [a.id, a.contentType]), [["a1", "image/jpeg"], ["a2", "application/pdf"]]);
  assert.equal(s.attachmentUrl("d1", "a2"), `${CONFIG.platform.url}/storage/v1/object/sign/attachments/org1/d1/2_Spray_Record_v1.pdf?token=t`);

  await s.addAttachment("d1", new Blob(["x"], { type: "application/pdf" }), "Spray Record EP-C489.5 2026-11-18 v2.pdf");
  const up = calls.find((c) => c.url.pathname.startsWith("/storage/v1/object/attachments/org1/d1/"))!;
  assert.match(up.url.pathname, /_Spray_Record_EP-C489\.5_2026-11-18_v2\.pdf$/);
  assert.equal(up.headers["Content-Type"], "application/pdf");
  const row = calls.find((c) => c.method === "POST" && c.url.pathname === "/rest/v1/attachments")!;
  assert.equal(row.body.kind, "report");
  assert.equal(row.body.daily_record_id, "d1");

  await s.deleteAttachments("d1", ["a1"]);
  const rm = calls.find((c) => c.method === "DELETE" && c.url.pathname === "/storage/v1/object/attachments")!;
  assert.deepEqual(rm.body, { prefixes: ["org1/d1/1_photo_1.jpg"] });
  assert.ok(calls.some((c) => c.method === "DELETE" && c.url.searchParams.get("id") === "in.(a1)"));
  assert.equal(s.attachmentUrl("d1", "a1"), "");
});
