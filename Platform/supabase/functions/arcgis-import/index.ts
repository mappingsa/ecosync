// ArcGIS -> EcoSync Data Platform import, as a Supabase Edge Function.
//
// The cloud version of importer/import_arcgis.py: same matching, same keys,
// so the two can run side by side without making duplicates. pg_cron calls it
// every hour (010_cloud_import.sql); each run is written to import_runs.
//
//   POST /functions/v1/arcgis-import            last 14 days (the hourly run)
//   POST /functions/v1/arcgis-import?days=60    a longer window
//   POST /functions/v1/arcgis-import?full=1     everything
//   POST /functions/v1/arcgis-import?dry=1      read and match, write nothing
//
// Needs the header x-import-secret (Supabase made and keeps it in Vault), so
// nobody else can start an import. Turn OFF "Verify JWT" for this function.
//
// Secrets (Edge Functions > Secrets), entered by the owner:
//   ARCGIS_API_KEY                    or
//   ARCGIS_USERNAME + ARCGIS_PASSWORD (the ArcGIS Online account that owns the layers)
// SUPABASE_URL and the service key are provided by Supabase automatically.

const ORG_NAME = "FieldPoint Environmental";
const SERVICES = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services";
const LAYERS: Record<string, [string, string]> = {
  points: [`${SERVICES}/MSA_Weed_Spray_Points/FeatureServer/0`, "treatment_points"],
  lines: [`${SERVICES}/Weed_Spray_Lines/FeatureServer/0`, "treatment_lines"],
  polygons: [`${SERVICES}/MSA_Weed_Spray_Polygons/FeatureServer/0`, "treatment_areas"],
};
const TRACK_URL = `${SERVICES}/MSA_Weed_Spray_Track/FeatureServer/0`;
const PORTAL = "https://www.arcgis.com";
const DAILY_TITLE = "EcoSync Daily Records";
const DAILY_SOURCE = "arcgis:EcoSync_Daily_Records";
const BUCKET = "attachments";
const BATCH = 500;

type Attrs = Record<string, any>;
type EsriFeature = { attributes?: Attrs; geometry?: any };

// --- matching (as mapping.py) -------------------------------------------------------

export const norm = (v: unknown) => String(v ?? "").replace(/\s+/g, " ").trim().toUpperCase();

export function attr(a: Attrs, name: string): any {
  if (name in a) return a[name];
  const low = name.toLowerCase();
  for (const k of Object.keys(a)) if (k.toLowerCase() === low) return a[k];
  return null;
}

export function buildIndex(rows: any[], nameKey: string, aliasesKey?: string): Map<string, string> {
  const ix = new Map<string, string>();
  for (const r of rows) {
    for (const n of [r[nameKey], ...(aliasesKey ? r[aliasesKey] ?? [] : [])]) {
      if (n && !ix.has(norm(n))) ix.set(norm(n), r.id);
    }
  }
  return ix;
}

export const match = (v: unknown, ix: Map<string, string>) => {
  const k = norm(v);
  return k ? ix.get(k) ?? null : null;
};

// --- Esri JSON -> EWKT ----------------------------------------------------------------

const pt = (p: number[]) => `${Number(p[0])} ${Number(p[1])}`;

/** Clockwise with y up = an Esri outer ring; anticlockwise = a hole. */
function clockwise(ring: number[][]): boolean {
  let s = 0;
  for (let i = 0; i < ring.length - 1; i++) s += (ring[i + 1][0] - ring[i][0]) * (ring[i + 1][1] + ring[i][1]);
  return s > 0;
}

/** Lines are always MULTILINESTRING and areas MULTIPOLYGON, matching the columns. */
export function esriToEwkt(g: any): string | null {
  if (!g) return null;
  if (g.x != null && g.y != null) return `SRID=4326;POINT(${pt([g.x, g.y])})`;
  if (g.paths?.length) {
    const parts = g.paths.filter((p: number[][]) => p.length >= 2);
    if (!parts.length) return null;
    return `SRID=4326;MULTILINESTRING(${parts.map((p: number[][]) => `(${p.map(pt).join(",")})`).join(",")})`;
  }
  if (g.rings?.length) {
    const polys: number[][][][] = [];
    for (const raw of g.rings as number[][][]) {
      if (raw.length < 4) continue;
      const ring = raw.map((p) => [p[0], p[1]]);
      const [f, l] = [ring[0], ring[ring.length - 1]];
      if (f[0] !== l[0] || f[1] !== l[1]) ring.push(f);
      if (clockwise(ring) || !polys.length) polys.push([ring]);
      else polys[polys.length - 1].push(ring); // a hole belongs to the last outer ring
    }
    if (!polys.length) return null;
    return `SRID=4326;MULTIPOLYGON(${polys.map((poly) => `(${poly.map((r) => `(${r.map(pt).join(",")})`).join(",")})`).join(",")})`;
  }
  return null;
}

/** Track points -> one line per (unit, run_id), in time order; runs of one point are skipped. */
export function trackRuns(features: EsriFeature[]) {
  const runs = new Map<string, [number, number, number, Attrs][]>();
  for (const f of features) {
    const a = f.attributes ?? {}, g = f.geometry ?? {};
    if (g.x == null || g.y == null) continue;
    const rid = attr(a, "run_id");
    if (rid == null || rid === "") continue;
    const key = `${attr(a, "job_id") ?? ""}:${rid}`;
    if (!runs.has(key)) runs.set(key, []);
    runs.get(key)!.push([attr(a, "treatment_time") ?? 0, g.x, g.y, a]);
  }
  const out = [];
  for (const [key, pts] of runs) {
    pts.sort((x, y) => x[0] - y[0]);
    if (pts.length < 2) continue;
    out.push({
      key,
      job: attr(pts[0][3], "Job_Number"),
      start: pts[0][0],
      end: pts[pts.length - 1][0],
      ewkt: `SRID=4326;MULTILINESTRING((${pts.map((p) => pt([p[1], p[2]])).join(",")}))`,
    });
  }
  return out;
}

// --- Daily Records (job sheets) --------------------------------------------------------

const DAILY_NUMBERS = ["tank_total_l", "start_temp_c", "start_rh_pct", "start_wind_kmh", "start_delta_t",
  "end_temp_c", "end_rh_pct", "end_wind_kmh", "end_delta_t"];
const DAILY_TEXTS = ["start_time", "end_time", "start_wind_dir", "end_wind_dir", "weather_source", "observations", "flag_issues"];
const DAILY_LISTS = ["operators", "equipment", "chemicals"];

export const splitList = (v: unknown) => String(v ?? "").split(";").map((x) => x.trim()).filter(Boolean);
const num = (v: unknown) => {
  if (v == null || v === "") return null;
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
};

export function dailyRow(a: Attrs): Record<string, any> | null {
  const date = String(attr(a, "record_date") ?? "").trim();
  if (!/^\d{4}-\d{2}-\d{2}$/.test(date)) return null;
  const row: Record<string, any> = { record_date: date };
  for (const k of DAILY_NUMBERS) row[k] = num(attr(a, k));
  for (const k of DAILY_TEXTS) {
    const v = attr(a, k);
    row[k] = v == null || v === "" ? null : String(v);
  }
  for (const k of DAILY_LISTS) row[k] = splitList(attr(a, k));
  const entity = norm(attr(a, "entity"));
  row.letterhead = entity === "MSA" || entity === "FPE" ? entity : null;
  row.status = norm(attr(a, "status")) === "ISSUED" ? "issued" : "draft";
  return row;
}

export const attachmentKind = (name: string) =>
  /^signature.*\.png$/i.test(name ?? "") ? "signature" : /\.pdf$/i.test(name ?? "") ? "report" : "photo";

export const safeName = (name: string) => String(name ?? "file").replace(/[^A-Za-z0-9._-]+/g, "_").replace(/^_+|_+$/g, "") || "file";

const iso = (ms: number | null) => (ms ? new Date(ms).toISOString() : null);

// --- ArcGIS -----------------------------------------------------------------------------

async function arcgisToken(env: (k: string) => string | undefined): Promise<string> {
  const key = env("ARCGIS_API_KEY");
  if (key) return key;
  const username = env("ARCGIS_USERNAME"), password = env("ARCGIS_PASSWORD");
  if (!username || !password) throw new Error("ArcGIS sign-in isn't set: add ARCGIS_USERNAME and ARCGIS_PASSWORD (or ARCGIS_API_KEY) under Edge Functions > Secrets.");
  const r = await fetch(`${PORTAL}/sharing/rest/generateToken`, {
    method: "POST",
    body: new URLSearchParams({ username, password, referer: PORTAL, expiration: "60", f: "json" }),
  });
  const body = await r.json();
  // never echo the response: it can contain the credentials
  if (!body.token) throw new Error(`ArcGIS sign-in failed: ${body.error?.message ?? "unknown error"}`);
  return body.token;
}

async function arcPost(url: string, token: string, params: Record<string, string>): Promise<any> {
  const r = await fetch(url, { method: "POST", body: new URLSearchParams({ f: "json", token, ...params }) });
  if (!r.ok) throw new Error(`${url.split("/services/").pop()}: HTTP ${r.status}`);
  const body = await r.json();
  if (body.error) throw new Error(`${url.split("/services/").pop()}: ${body.error.message ?? JSON.stringify(body.error)}`);
  return body;
}

const dateStyle = new Map<string, "epoch" | "stamp">();

/** treatment_time >= since: some services want epoch ms, some a TIMESTAMP literal. Probe once per layer. */
async function sinceWhere(url: string, token: string, since: number | null): Promise<string> {
  if (since == null) return "1=1";
  const epoch = `treatment_time >= ${since}`;
  const stamp = `treatment_time >= TIMESTAMP '${new Date(since).toISOString().slice(0, 19).replace("T", " ")}'`;
  if (!dateStyle.has(url)) {
    try {
      await arcPost(`${url}/query`, token, { where: epoch, returnCountOnly: "true" });
      dateStyle.set(url, "epoch");
    } catch {
      dateStyle.set(url, "stamp");
    }
  }
  return dateStyle.get(url) === "epoch" ? epoch : stamp;
}

async function fetchAll(url: string, token: string, where: string): Promise<EsriFeature[]> {
  const out: EsriFeature[] = [];
  for (let i = 0; i < 1000; i++) {
    const body = await arcPost(`${url}/query`, token, {
      where, outFields: "*", returnGeometry: "true", outSR: "4326",
      orderByFields: "OBJECTID", resultOffset: String(out.length), resultRecordCount: "2000",
    });
    const feats = body.features ?? [];
    out.push(...feats);
    if (!body.exceededTransferLimit || !feats.length) break;
  }
  return out;
}

// --- Supabase ---------------------------------------------------------------------------

class Supabase {
  private h: Record<string, string>;
  constructor(private url: string, key: string, readonly dryRun: boolean) {
    this.h = { apikey: key, "Content-Type": "application/json" };
    // legacy service_role keys are JWTs and also go in Authorization; sb_secret_ keys only as apikey
    if (!key.startsWith("sb_")) this.h.Authorization = `Bearer ${key}`;
  }

  private async req(path: string, init: RequestInit & { headers?: Record<string, string> } = {}) {
    const r = await fetch(`${this.url}${path}`, { ...init, headers: { ...this.h, ...(init.headers ?? {}) } });
    const text = await r.text();
    if (!r.ok) throw new Error(`Supabase ${path.split("?")[0]}: ${text.slice(0, 300)}`);
    return text ? JSON.parse(text) : null;
  }

  async get(table: string, params: Record<string, string>): Promise<any[]> {
    const out: any[] = [];
    for (;;) {
      const page = await this.req(`/rest/v1/${table}?${new URLSearchParams({ ...params, limit: "1000", offset: String(out.length) })}`);
      out.push(...page);
      if (page.length < 1000) return out;
    }
  }

  async upsert(table: string, rows: any[], conflict: string, returning?: string): Promise<any[]> {
    if (this.dryRun || !rows.length) return [];
    const q = new URLSearchParams({ on_conflict: conflict, ...(returning ? { select: returning } : {}) });
    const prefer = `resolution=merge-duplicates,${returning ? "return=representation" : "return=minimal"}`;
    const out: any[] = [];
    for (let i = 0; i < rows.length; i += BATCH) {
      const got = await this.req(`/rest/v1/${table}?${q}`, { method: "POST", headers: { Prefer: prefer }, body: JSON.stringify(rows.slice(i, i + BATCH)) });
      if (returning) out.push(...got);
    }
    return out;
  }

  async insert(table: string, row: any) {
    await this.req(`/rest/v1/${table}`, { method: "POST", headers: { Prefer: "return=minimal" }, body: JSON.stringify(row) });
  }

  async delete(table: string, params: Record<string, string>) {
    if (!this.dryRun) await this.req(`/rest/v1/${table}?${new URLSearchParams(params)}`, { method: "DELETE" });
  }

  async rpc(fn: string, args: Record<string, unknown>): Promise<any> {
    return this.req(`/rest/v1/rpc/${fn}`, { method: "POST", body: JSON.stringify(args) });
  }

  async putFile(path: string, data: ArrayBuffer, contentType: string | undefined) {
    if (this.dryRun) return;
    const r = await fetch(`${this.url}/storage/v1/object/${BUCKET}/${path}`, {
      method: "POST",
      headers: { ...this.h, "Content-Type": contentType || "application/octet-stream", "x-upsert": "true" },
      body: data,
    });
    if (!r.ok) throw new Error(`Supabase storage ${path}: ${(await r.text()).slice(0, 300)}`);
  }

  async removeFiles(paths: string[]) {
    if (this.dryRun || !paths.length) return;
    await this.req(`/storage/v1/object/${BUCKET}`, { method: "DELETE", body: JSON.stringify({ prefixes: paths }) });
  }
}

// --- the import ----------------------------------------------------------------------------

type Note = (source: string, field: string, value: unknown) => void;

async function importDaily(sb: Supabase, org: string, token: string, jobIx: Map<string, string>, note: Note, log: string[]) {
  const body = await arcPost(`${PORTAL}/sharing/rest/search`, token, { q: `title:"${DAILY_TITLE}" AND type:"Feature Service"`, num: "20" });
  const item = (body.results ?? []).find((i: any) => String(i.title ?? "").trim().toLowerCase() === DAILY_TITLE.toLowerCase() && i.url);
  if (!item) {
    log.push(`Daily Records: no '${DAILY_TITLE}' table found in ArcGIS - skipped`);
    return { sheets: 0, copied: 0, removed: 0, skipped: 0 };
  }
  const url = `${String(item.url).replace(/\/+$/, "")}/0`;
  const feats = await fetchAll(url, token, "1=1");

  // sheets first saved in the platform are left alone
  const keep = new Set((await sb.get("daily_records", { org_id: `eq.${org}`, source: `neq.${DAILY_SOURCE}`, select: "job_id,record_date" }))
    .map((r) => `${r.job_id}|${r.record_date}`));
  const newest = new Map<string, [number, Record<string, any>]>(); // the app reads the newest of any duplicates
  let skipped = 0;
  for (const f of feats) {
    const a = f.attributes ?? {};
    const oid = attr(a, "OBJECTID");
    const row = dailyRow(a);
    const jobRaw = attr(a, "job_number");
    const jobId = match(jobRaw, jobIx);
    if (jobId == null) note(DAILY_SOURCE, "job_number", jobRaw);
    if (!row || jobId == null || oid == null) { skipped++; continue; }
    const k = `${jobId}|${row.record_date}`;
    if (keep.has(k)) continue;
    if (!newest.has(k) || oid > newest.get(k)![0]) {
      Object.assign(row, { org_id: org, job_id: jobId, source: DAILY_SOURCE, source_ref: String(attr(a, "GlobalID") ?? oid) });
      newest.set(k, [Number(oid), row]);
    }
  }
  const saved = await sb.upsert("daily_records", [...newest.values()].map(([, r]) => r), "org_id,job_id,record_date", "id,job_id,record_date");
  const recId = new Map(saved.map((r) => [`${r.job_id}|${r.record_date}`, r.id as string]));
  const oidToKey = new Map([...newest].map(([k, [oid]]) => [oid, k]));

  // attachments: one call if the layer allows it, else per record
  const infos = new Map<number, any[]>([...oidToKey.keys()].map((o) => [o, []]));
  const oids = [...oidToKey.keys()].sort((x, y) => x - y);
  try {
    for (let i = 0; i < oids.length; i += 100) {
      const b = await arcPost(`${url}/queryAttachments`, token, { objectIds: oids.slice(i, i + 100).join(",") });
      for (const g of b.attachmentGroups ?? []) infos.set(g.parentObjectId, g.attachmentInfos ?? []);
    }
  } catch {
    for (const o of oids) infos.set(o, (await arcPost(`${url}/${o}/attachments`, token, {})).attachmentInfos ?? []);
  }

  const have = new Map<string, string>(); // source_ref -> storage_path, for files already copied
  for (const r of await sb.get("attachments", { org_id: `eq.${org}`, source_ref: "not.is.null", select: "source_ref,storage_path" })) {
    have.set(r.source_ref, r.storage_path);
  }
  let copied = 0;
  const wanted = new Set<string>();
  for (const [oid, atts] of infos) {
    const rid = recId.get(oidToKey.get(oid)!);
    for (const att of atts) {
      const ref = `${DAILY_SOURCE}:${oid}:${att.id}`;
      wanted.add(ref);
      if (have.has(ref) || (rid == null && !sb.dryRun)) continue;
      if (sb.dryRun) { copied++; continue; }
      try {
        const r = await fetch(`${url}/${oid}/attachments/${att.id}?${new URLSearchParams({ token })}`);
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        const data = await r.arrayBuffer();
        const path = `${org}/${rid}/${att.id}_${safeName(att.name)}`;
        await sb.putFile(path, data, att.contentType);
        await sb.upsert("attachments", [{
          org_id: org, daily_record_id: rid, kind: attachmentKind(att.name), storage_path: path, name: att.name || "file",
          content_type: att.contentType, size_bytes: att.size || data.byteLength, source_ref: ref,
        }], "org_id,storage_path");
        copied++;
      } catch (e) {
        log.push(`could not copy ${att.name} (sheet ${oid}): ${(e as Error).message}`); // tried again next run
      }
    }
  }
  // files deleted in ArcGIS go here too, for sheets read this run
  const prefixes = [...oidToKey.keys()].map((o) => `${DAILY_SOURCE}:${o}:`);
  const gone = [...have].filter(([ref]) => !wanted.has(ref) && prefixes.some((p) => ref.startsWith(p)));
  if (gone.length) {
    await sb.removeFiles(gone.map(([, p]) => p));
    for (const [ref] of gone) await sb.delete("attachments", { org_id: `eq.${org}`, source_ref: `eq.${ref}` });
  }
  return { sheets: newest.size, copied, removed: gone.length, skipped };
}

export async function runImport(sb: Supabase, token: string, opts: { days: number; full: boolean }) {
  const log: string[] = [];
  const orgs = await sb.get("organisations", { name: `eq.${ORG_NAME}`, select: "id" });
  if (!orgs.length) throw new Error(`Organisation '${ORG_NAME}' not found - run 002_seed_fpe.sql first.`);
  const org = orgs[0].id as string;
  const q = { org_id: `eq.${org}` };
  const speciesRows = await sb.get("species", { ...q, select: "id,name,aliases,default_method_id" });
  const speciesIx = buildIndex(speciesRows, "name", "aliases");
  const methodIx = buildIndex(await sb.get("control_methods", { ...q, select: "id,name,aliases" }), "name", "aliases");
  const jobIx = buildIndex(await sb.get("jobs", { ...q, select: "id,job_number,aliases" }), "job_number", "aliases");
  const defaultMethod = new Map(speciesRows.map((r) => [r.id, r.default_method_id]));
  const since = opts.full ? null : Date.now() - opts.days * 864e5;

  const unmatched = new Map<string, number>();
  const note: Note = (source, field, value) => {
    if (value == null || value === "") return;
    const k = JSON.stringify([source, field, String(value).trim()]);
    unmatched.set(k, (unmatched.get(k) ?? 0) + 1);
  };

  const totals: Record<string, [number, number]> = {};
  for (const [name, [url, table]] of Object.entries(LAYERS)) {
    const feats = await fetchAll(url, token, await sinceWhere(url, token, since));
    const source = `arcgis:${url.split("/services/").pop()!.split("/")[0]}`;
    const rows: any[] = [];
    let skipped = 0;
    for (const f of feats) {
      const a = f.attributes ?? {};
      const geom = esriToEwkt(f.geometry);
      const when = attr(a, "treatment_time");
      if (!geom || !when) { skipped++; continue; }
      const [jobRaw, spRaw, mRaw] = [attr(a, "Job_Number"), attr(a, "weed_species"), attr(a, "control_type")];
      const jobId = match(jobRaw, jobIx), spId = match(spRaw, speciesIx);
      let mId = match(mRaw, methodIx);
      if (jobId == null) note(source, "job_number", jobRaw);
      if (spId == null) note(source, "weed_species", spRaw);
      if (mId == null) {
        note(source, "control_type", mRaw);
        mId = spId ? defaultMethod.get(spId) ?? null : null; // blank method: the species' usual one
      }
      const runId = attr(a, "run_id");
      rows.push({
        org_id: org, job_id: jobId, species_id: spId, method_id: mId,
        treated_at: iso(when), accuracy_m: num(attr(a, "gps_accuracy_m")),
        run_id: runId == null || runId === "" ? null : String(runId),
        capture_mode: attr(a, "capture_mode"),
        source, source_ref: `${name}:${attr(a, "GlobalID") ?? attr(a, "OBJECTID")}`, geom,
      });
    }
    await sb.upsert(table, rows, "org_id,source,source_ref");
    totals[name] = [rows.length, skipped];
  }

  const trackFeats = await fetchAll(TRACK_URL, token, await sinceWhere(TRACK_URL, token, since));
  const trackRows = trackRuns(trackFeats).map((r) => {
    const jobId = match(r.job, jobIx);
    if (jobId == null) note("arcgis:MSA_Weed_Spray_Track", "job_number", r.job);
    return {
      org_id: org, job_id: jobId, started_at: iso(r.start), ended_at: iso(r.end),
      source: "arcgis:MSA_Weed_Spray_Track", source_ref: `track:${r.key}`, geom: r.ewkt,
    };
  });
  await sb.upsert("tracks", trackRows, "org_id,source,source_ref");
  totals["track runs"] = [trackRows.length, 0];

  const daily = await importDaily(sb, org, token, jobIx, note, log);

  const now = new Date().toISOString();
  await sb.upsert("needs_a_home", [...unmatched].map(([k, n]) => {
    const [source, field, value] = JSON.parse(k);
    return { org_id: org, source, field, value, seen: n, last_seen: now };
  }), "org_id,source,field,value");

  const summary = `${opts.full ? "everything" : `last ${opts.days} days`}: ${Object.entries(totals).map(([k, v]) => `${k} ${v[0]}`).join(", ")}; job sheets ${daily.sheets}, files copied ${daily.copied}${daily.removed ? `, files removed ${daily.removed}` : ""}`;
  const skippedText = Object.entries(totals).filter(([k, v]) => k !== "track runs" && v[1]).map(([k, v]) => `${k} ${v[1]}`).join(", ");
  if (skippedText) log.push(`skipped (no geometry or time): ${skippedText}`);
  if (daily.skipped) log.push(`job sheets skipped (bad date or unknown job): ${daily.skipped}`);
  const notMatched = [...unmatched].sort((a, b) => b[1] - a[1]).map(([k, n]) => {
    const [, field, value] = JSON.parse(k);
    return { field, value, count: n };
  });
  return { org, summary, log, totals, daily, notMatched };
}

// --- HTTP entry point ------------------------------------------------------------------------

export async function handle(req: Request): Promise<Response> {
  const env = (k: string): string | undefined => (globalThis as any).Deno.env.get(k);
  const url = env("SUPABASE_URL")!;
  let key = env("SUPABASE_SERVICE_ROLE_KEY") ?? "";
  if (!key) {
    try { key = JSON.parse(env("SUPABASE_SECRET_KEYS") ?? "{}").default ?? ""; } catch { /* fall through */ }
  }
  if (!url || !key) return Response.json({ ok: false, error: "Supabase settings missing" }, { status: 500 });

  const q = new URL(req.url).searchParams;
  const sb = new Supabase(url, key, q.get("dry") === "1");
  const secret = req.headers.get("x-import-secret") ?? "";
  if (!secret || !(await sb.rpc("check_import_secret", { s: secret }))) {
    return Response.json({ ok: false, error: "not allowed" }, { status: 401 });
  }

  const started = new Date().toISOString();
  const opts = { days: Math.max(1, Math.min(3650, Number(q.get("days") ?? 14) || 14)), full: q.get("full") === "1" };
  try {
    const token = await arcgisToken(env);
    const r = await runImport(sb, token, opts);
    if (!sb.dryRun) {
      await sb.insert("import_runs", {
        org_id: r.org, started_at: started, finished_at: new Date().toISOString(), ok: true,
        summary: r.summary, details: { totals: r.totals, daily: r.daily, notes: r.log, not_matched: r.notMatched },
      });
    }
    return Response.json({ ok: true, dryRun: sb.dryRun, summary: r.summary, notes: r.log, notMatched: r.notMatched });
  } catch (e) {
    const error = (e as Error).message;
    try {
      const orgs = await sb.get("organisations", { name: `eq.${ORG_NAME}`, select: "id" });
      if (orgs.length) await sb.insert("import_runs", { org_id: orgs[0].id, started_at: started, finished_at: new Date().toISOString(), ok: false, summary: error });
    } catch { /* nothing more to do */ }
    return Response.json({ ok: false, error }, { status: 500 });
  }
}

const deno = (globalThis as any).Deno;
if (deno?.serve) deno.serve(handle); // in Supabase; tests import the functions above without starting a server
