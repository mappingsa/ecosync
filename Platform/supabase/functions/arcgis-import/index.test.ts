// Run: npx tsx --test index.test.ts  (plain Node: fake ArcGIS and Supabase servers)
import { test } from "node:test";
import assert from "node:assert/strict";
import { handle } from "./index.ts";

const SECRET = "s3cret";
const env: Record<string, string> = { SUPABASE_URL: "https://db.test", SUPABASE_SERVICE_ROLE_KEY: "sb_secret_x", ARCGIS_USERNAME: "u", ARCGIS_PASSWORD: "p" };
(globalThis as any).Deno = { env: { get: (k: string) => env[k] } };

type Call = { method: string; url: URL; body: any; headers: Record<string, string> };
function server(dailyFiles = true) {
  const calls: Call[] = [];
  globalThis.fetch = (async (input: string, init: any = {}) => {
    const url = new URL(input);
    let body: any = init.body;
    if (body instanceof URLSearchParams) body = Object.fromEntries(body);
    else if (typeof body === "string") { try { body = JSON.parse(body); } catch { /* text */ } }
    calls.push({ method: init.method ?? "GET", url, body, headers: init.headers ?? {} });
    const json = (x: unknown, status = 200) => new Response(JSON.stringify(x), { status });
    const p = url.pathname;
    // --- ArcGIS
    if (p.endsWith("/generateToken")) return json({ token: "arc-token" });
    if (p.endsWith("/sharing/rest/search")) return json({ results: [{ title: "EcoSync Daily Records", url: "https://services2.arcgis.com/x/arcgis/rest/services/EcoSync_Daily_Records/FeatureServer" }] });
    if (p.includes("EcoSync_Daily_Records")) {
      if (p.endsWith("/queryAttachments")) return json({ attachmentGroups: dailyFiles ? [{ parentObjectId: 5, attachmentInfos: [{ id: 9, name: "photo 1.jpg", contentType: "image/jpeg", size: 3 }] }] : [] });
      if (p.endsWith("/attachments/9")) return new Response("abc", { status: 200 });
      return json({ features: [{ attributes: { OBJECTID: 5, GlobalID: "{G5}", job_number: "DEW NP Weed Control", record_date: "2026-11-23", operators: "Jayme Peltz" } }] });
    }
    if (p.endsWith("/query")) {
      if (body.returnCountOnly) return json({ count: 1 });
      if (p.includes("MSA_Weed_Spray_Points")) return json({ features: [
        { attributes: { OBJECTID: 1, GlobalID: "{A}", Job_Number: "c489.5 ", weed_species: "African Boxthorn", control_type: "", treatment_time: 1795400000000, gps_accuracy_m: 0.7, run_id: 12 }, geometry: { x: 140.6, y: -34.3 } },
        { attributes: { OBJECTID: 2, GlobalID: "{B}", Job_Number: "EP-C999", weed_species: "Mystery weed", control_type: "Spot Spray", treatment_time: 1795400001000 }, geometry: { x: 140.61, y: -34.31 } },
        { attributes: { OBJECTID: 3, GlobalID: "{C}", Job_Number: "EP-C489.5", treatment_time: null }, geometry: { x: 140.62, y: -34.32 } },
      ] });
      if (p.includes("MSA_Weed_Spray_Track")) return json({ features: [
        { attributes: { job_id: "u1", run_id: 12, treatment_time: 1795400000000, Job_Number: "EP-C489.5" }, geometry: { x: 140.6, y: -34.3 } },
        { attributes: { job_id: "u1", run_id: 12, treatment_time: 1795400060000, Job_Number: "EP-C489.5" }, geometry: { x: 140.61, y: -34.3 } },
      ] });
      return json({ features: [] });
    }
    // --- Supabase
    if (p === "/rest/v1/rpc/check_import_secret") return json(body.s === SECRET);
    const t = p.replace("/rest/v1/", "");
    if (init.method === "POST" && p.startsWith("/storage/")) return json({ Key: "ok" });
    if ((init.method ?? "GET") === "GET") {
      const rows: Record<string, any[]> = {
        organisations: [{ id: "org1" }],
        species: [{ id: "sb", name: "African Boxthorn", aliases: ["African Boxthor"], default_method_id: "mc" }],
        control_methods: [{ id: "mc", name: "Cut and swab", aliases: [] }, { id: "ms", name: "Spot Spray", aliases: [] }],
        jobs: [{ id: "jA", job_number: "EP-C489.5", aliases: ["C489.5"] }, { id: "jB", job_number: "DEW-NP", aliases: ["DEW NP Weed Control"] }],
        daily_records: [], attachments: [],
      };
      return json(url.searchParams.get("offset") === "0" ? rows[t] ?? [] : []);
    }
    if (t === "daily_records") return json([{ id: "d1", job_id: "jB", record_date: "2026-11-23" }]);
    return json([], 201);
  }) as typeof fetch;
  return calls;
}

const call = (q = "", secret = SECRET) => handle(new Request(`https://db.test/functions/v1/arcgis-import${q}`, { method: "POST", headers: { "x-import-secret": secret } }));
const posts = (calls: Call[], path: string) => calls.filter((c) => c.method === "POST" && c.url.pathname === path);

test("refuses a call without the secret, and doesn't touch ArcGIS", async () => {
  const calls = server();
  const r = await call("", "wrong");
  assert.equal(r.status, 401);
  assert.equal(calls.some((c) => c.url.hostname.includes("arcgis")), false);
});

test("a run: matches lists, fills a blank method, notes unknowns, groups the track, copies the job sheet and its photo", async () => {
  const calls = server();
  const r = await call();
  const out = await r.json();
  assert.equal(out.ok, true, out.error);
  assert.match(out.summary, /^last 14 days: points 2, lines 0, polygons 0, track runs 1; job sheets 1, files copied 1$/);

  const pts = posts(calls, "/rest/v1/treatment_points")[0];
  assert.equal(pts.url.searchParams.get("on_conflict"), "org_id,source,source_ref");
  assert.match(pts.headers.Prefer, /merge-duplicates/);
  assert.equal(pts.headers.apikey, "sb_secret_x");
  assert.equal(pts.headers.Authorization, undefined); // new-style key: apikey only
  const [a, b] = pts.body;
  assert.deepEqual(
    { job: a.job_id, sp: a.species_id, m: a.method_id, ref: a.source_ref, src: a.source, run: a.run_id, acc: a.accuracy_m, geom: a.geom, t: a.treated_at },
    { job: "jA", sp: "sb", m: "mc", ref: "points:{A}", src: "arcgis:MSA_Weed_Spray_Points", run: "12", acc: 0.7, geom: "SRID=4326;POINT(140.6 -34.3)", t: new Date(1795400000000).toISOString() },
  );
  assert.equal(b.job_id, null);
  assert.equal(b.method_id, "ms");

  const homes = posts(calls, "/rest/v1/needs_a_home")[0].body.map((h: any) => `${h.field}:${h.value}`).sort();
  assert.deepEqual(homes, ["control_type:", "job_number:EP-C999", "weed_species:Mystery weed"].filter((x) => !x.endsWith(":")));

  const track = posts(calls, "/rest/v1/tracks")[0].body[0];
  assert.equal(track.source_ref, "track:u1:12");
  assert.equal(track.geom, "SRID=4326;MULTILINESTRING((140.6 -34.3,140.61 -34.3))");

  const sheet = posts(calls, "/rest/v1/daily_records")[0].body[0];
  assert.deepEqual({ job: sheet.job_id, ops: sheet.operators, ref: sheet.source_ref }, { job: "jB", ops: ["Jayme Peltz"], ref: "{G5}" });
  const file = calls.find((c) => c.method === "POST" && c.url.pathname.startsWith("/storage/v1/object/attachments/"))!;
  assert.equal(file.url.pathname, "/storage/v1/object/attachments/org1/d1/9_photo_1.jpg");
  assert.equal(posts(calls, "/rest/v1/attachments")[0].body[0].source_ref, "arcgis:EcoSync_Daily_Records:5:9");

  const run = posts(calls, "/rest/v1/import_runs")[0].body;
  assert.equal(run.ok, true);
  assert.equal(run.org_id, "org1");
  assert.equal(run.details.not_matched.length, 2);

  // 14 days back, as epoch (the probe answered)
  const q = calls.find((c) => c.url.pathname.includes("MSA_Weed_Spray_Points") && c.body?.outFields)!;
  assert.match(q.body.where, /^treatment_time >= \d{13}$/);
  assert.equal(q.body.token, "arc-token");
});

test("dry run writes nothing; full run asks for everything", async () => {
  let calls = server();
  const r = await (await call("?dry=1")).json();
  assert.equal(r.dryRun, true);
  assert.equal(calls.filter((c) => c.method !== "GET" && c.url.hostname === "db.test" && !c.url.pathname.includes("/rpc/")).length, 0);
  calls = server(false);
  await call("?full=1");
  const q = calls.find((c) => c.url.pathname.includes("MSA_Weed_Spray_Points") && c.body?.outFields)!;
  assert.equal(q.body.where, "1=1");
});

test("a failure is recorded in import_runs and reported", async () => {
  server();
  delete env.ARCGIS_PASSWORD;
  const calls = server();
  const r = await call();
  env.ARCGIS_PASSWORD = "p";
  assert.equal(r.status, 500);
  const out = await r.json();
  assert.match(out.error, /ArcGIS sign-in isn't set/);
  const run = posts(calls, "/rest/v1/import_runs")[0].body;
  assert.equal(run.ok, false);
});
