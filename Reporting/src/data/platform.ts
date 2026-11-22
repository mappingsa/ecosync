// The EcoSync Data Platform DataSource (Supabase: Postgres + PostGIS, Sydney).
// Like arcgis.ts, the ONLY file that knows this backend's REST API. Spray
// records are read through the report_* views (007_reporting_app.sql), which
// give the same field names as the ArcGIS layers, so the report code is
// unchanged. Everything goes through the signed-in person's own row-level
// security: they see and change only what their role allows.

import { CONFIG } from "../config";
import type { AttachmentInfo, Attributes, DataSource, Feature, Geometry, Job, Lookups, RecordId, TargetRow } from "./types";
import { joinList, splitList, toRecord, type DailyRecord } from "./dailySchema";

export class PlatformError extends Error {}

const VIEWS = { points: "report_points", lines: "report_lines", polygons: "report_areas", tracks: "report_tracks" } as const;

/** The layers the map workspace can show. */
export type WorkspaceLayer = keyof typeof VIEWS;
/** The layers that can be drawn and edited (tracks come from the unit only). */
export type EditableLayer = "points" | "lines" | "polygons";
const TABLES: Record<EditableLayer, string> = { points: "treatment_points", lines: "treatment_lines", polygons: "treatment_areas" };

export type Role = "owner" | "editor" | "viewer";

/** The pick lists for the edit form, with ids. */
export interface Domains {
  jobs: { id: string; job_number: string; active: boolean }[];
  species: { id: string; name: string; default_method_id: string | null; active: boolean }[];
  methods: { id: string; name: string; active: boolean }[];
}

/** What the edit form changes. Leave a key out to keep it. */
export interface FeatureValues {
  job_id?: string | null;
  species_id?: string | null;
  method_id?: string | null;
  treated_at?: string; // ISO
  notes?: string | null;
  geom?: string; // EWKT
}
const PAGE = 1000; // the most Supabase returns per request
const SIGN_SECONDS = 6 * 3600; // file links stay valid this long
const SIGNATURE = /^signature.*\.png$/i; // same rules as issueReport.ts
const PDF = /\.pdf$/i;

const norm = (s: unknown) => String(s ?? "").replace(/\s+/g, " ").trim().toUpperCase();
const txt = (v: unknown) => (v == null ? "" : String(v));
const orNull = (s: string | null | undefined) => (s && s.trim() ? s : null);

export function safeName(name: string): string {
  return name.replace(/[^A-Za-z0-9._-]+/g, "_").replace(/^_+|_+$/g, "") || "file";
}

export function guessType(name: string): string {
  const ext = name.toLowerCase().split(".").pop();
  return ({ jpg: "image/jpeg", jpeg: "image/jpeg", png: "image/png", pdf: "application/pdf" } as Record<string, string>)[ext ?? ""]
    ?? "application/octet-stream";
}

export function attachmentKind(name: string): "photo" | "signature" | "report" {
  return SIGNATURE.test(name) ? "signature" : PDF.test(name) ? "report" : "photo";
}

interface JobRef {
  id: string;
  job_number: string;
  aliases: string[] | null;
}

/** Database job sheet row -> the app's DailyRecord. */
export function rowToRecord(row: any, jobNumber: string): DailyRecord {
  return toRecord({
    ...row,
    job_number: jobNumber,
    entity: row.letterhead ?? "",
    operators: joinList(row.operators ?? []),
    equipment: joinList(row.equipment ?? []),
    chemicals: joinList(row.chemicals ?? []),
    updated_by: "", // a user id here, not a name
  }, row.id);
}

/** DailyRecord -> database columns (without org, job, id). */
export function recordToRow(r: DailyRecord): Record<string, unknown> {
  const num = (v: number | null) => (v == null || !Number.isFinite(v) ? null : v);
  return {
    record_date: r.record_date,
    letterhead: orNull(r.entity?.trim().toUpperCase()),
    start_time: orNull(r.start_time), end_time: orNull(r.end_time),
    operators: splitList(r.operators), equipment: splitList(r.equipment), chemicals: splitList(r.chemicals),
    tank_total_l: num(r.tank_total_l),
    start_temp_c: num(r.start_temp_c), start_rh_pct: num(r.start_rh_pct), start_wind_kmh: num(r.start_wind_kmh),
    start_wind_dir: orNull(r.start_wind_dir), start_delta_t: num(r.start_delta_t),
    end_temp_c: num(r.end_temp_c), end_rh_pct: num(r.end_rh_pct), end_wind_kmh: num(r.end_wind_kmh),
    end_wind_dir: orNull(r.end_wind_dir), end_delta_t: num(r.end_delta_t),
    weather_source: orNull(r.weather_source),
    observations: orNull(r.observations), flag_issues: orNull(r.flag_issues),
    status: r.status === "issued" ? "issued" : "draft",
  };
}

/**
 * Colours per species and method, in the shape of the old Targets table: one
 * row per species with its usual method first (methodOf() takes the first
 * method listed when a record has none), then a row for each method with its
 * own colour (Boxthorn sprayed = red).
 */
export function buildTargets(species: any[], methods: any[], overrides: any[]): TargetRow[] {
  const methodName = new Map(methods.map((m) => [m.id, m.name as string]));
  const out: TargetRow[] = [];
  for (const s of species) {
    const own = overrides.filter((o) => o.species_id === s.id);
    const def = s.default_method_id as string | null;
    const defColour = own.find((o) => o.method_id === def)?.colour ?? s.colour;
    out.push({ species: s.name, control_method: def ? methodName.get(def) ?? "" : "", colour: defColour });
    for (const o of own) {
      if (o.method_id !== def && methodName.has(o.method_id)) {
        out.push({ species: s.name, control_method: methodName.get(o.method_id)!, colour: o.colour });
      }
    }
  }
  return out;
}

export class PlatformSource implements DataSource {
  private jobsP: Promise<JobRef[]> | null = null;
  private urls = new Map<string, string>(); // attachment id -> signed link

  constructor(private token: () => Promise<string>, private userId: string) {}

  // --- plumbing ------------------------------------------------------------

  private async call(path: string, init: { method?: string; body?: unknown; raw?: Blob; prefer?: string; type?: string } = {}): Promise<any> {
    const h: Record<string, string> = {
      apikey: CONFIG.platform.publishableKey,
      Authorization: `Bearer ${await this.token()}`,
    };
    if (init.prefer) h.Prefer = init.prefer;
    let body: BodyInit | undefined;
    if (init.raw) {
      body = init.raw;
      h["Content-Type"] = init.type || "application/octet-stream";
    } else if (init.body !== undefined) {
      body = JSON.stringify(init.body);
      h["Content-Type"] = "application/json";
    }
    const r = await fetch(`${CONFIG.platform.url}${path}`, { method: init.method ?? "GET", headers: h, body });
    const text = await r.text();
    let j: any = null;
    try { j = text ? JSON.parse(text) : null; } catch { j = text; }
    if (!r.ok) {
      const msg = (j && (j.message || j.error_description || j.error || j.msg)) || `${r.status} ${r.statusText}`;
      throw new PlatformError(`EcoSync database: ${msg}`);
    }
    return j;
  }

  private rest(table: string, params: Record<string, string>, init: Parameters<PlatformSource["call"]>[1] = {}) {
    return this.call(`/rest/v1/${table}?${new URLSearchParams(params)}`, init);
  }

  /** Every row, a page at a time. */
  private async all(table: string, params: Record<string, string>): Promise<any[]> {
    const out: any[] = [];
    for (;;) {
      const page: any[] = await this.rest(table, { ...params, limit: String(PAGE), offset: String(out.length) });
      out.push(...page);
      if (page.length < PAGE) return out;
    }
  }

  private memberP: Promise<{ org_id: string; role: Role }> | null = null;

  private member(): Promise<{ org_id: string; role: Role }> {
    this.memberP ??= this.rest("members", { select: "org_id,role", user_id: `eq.${this.userId}` }).then((rows: any[]) => {
      if (!rows.length) {
        throw new PlatformError("Your account isn't in an organisation yet. Ask the owner to invite your email (see 007_reporting_app.sql).");
      }
      return rows[0];
    });
    this.memberP.catch(() => (this.memberP = null));
    return this.memberP;
  }

  private org(): Promise<string> {
    return this.member().then((m) => m.org_id);
  }

  /** owner / editor / viewer in this organisation. */
  async myRole(): Promise<Role> {
    return (await this.member()).role;
  }

  private jobs(): Promise<JobRef[]> {
    this.jobsP ??= this.all("jobs", { select: "id,job_number,aliases" });
    this.jobsP.catch(() => (this.jobsP = null));
    return this.jobsP;
  }

  private async job(jobNumber: string): Promise<JobRef | null> {
    const k = norm(jobNumber);
    return (await this.jobs()).find((j) => norm(j.job_number) === k || (j.aliases ?? []).some((a) => norm(a) === k)) ?? null;
  }

  // --- lookups and spray records -------------------------------------------

  async lookups(): Promise<Lookups> {
    const [jobs, species, methods, overrides, clients, lists] = await Promise.all([
      this.all("jobs", { select: "job_number,title,letterhead,property_address,owner_contact,default_equipment,default_chemicals,aliases,active,notes,clients(code)", order: "job_number" }),
      this.all("species", { select: "id,name,colour,default_method_id", active: "eq.true", order: "name" }),
      this.all("control_methods", { select: "id,name" }),
      this.all("species_method_colours", { select: "species_id,method_id,colour" }),
      this.all("clients", { select: "code,name" }),
      this.all("list_values", { select: "list,value", active: "eq.true", order: "list,value" }),
    ]);
    this.jobsP = null; // pick up new jobs on "Reload tables"
    const listMap = new Map<string, string[]>();
    for (const r of lists) listMap.set(r.list, [...(listMap.get(r.list) ?? []), r.value]);
    return {
      jobs: jobs.map((j): Job => ({
        job_number: txt(j.job_number),
        client_code: txt(j.clients?.code),
        entity: txt(j.letterhead),
        report_type: "",
        report_title: txt(j.title),
        property_address: txt(j.property_address),
        owner_contact: txt(j.owner_contact),
        default_equipment: txt(j.default_equipment),
        default_chemicals: txt(j.default_chemicals),
        targets: "",
        active: j.active ? "yes" : "no",
        notes: txt(j.notes),
        logo: "",
        aliases: joinList(j.aliases ?? []),
      })),
      targets: buildTargets(species, methods, overrides),
      clients: new Map(clients.map((c) => [txt(c.code).toUpperCase(), txt(c.name)])),
      lists: listMap,
      missing: [],
    };
  }

  async dayFeatures(layer: "points" | "polygons" | "lines", t0: number, t1: number): Promise<Feature[]> {
    return this.rangeFeatures(layer, t0, t1);
  }

  /** Every record on a layer with a time in [t0, t1) - any length of time, tracks included. */
  async rangeFeatures(layer: WorkspaceLayer, t0: number, t1: number): Promise<Feature[]> {
    const rows = await this.all(VIEWS[layer], {
      select: "*",
      and: `(treated_at.gte.${new Date(t0).toISOString()},treated_at.lt.${new Date(t1).toISOString()})`,
      order: "treated_at,id",
    });
    return rows.map(({ geometry, ...properties }) => ({
      type: "Feature" as const,
      geometry: (geometry ?? null) as Geometry | null,
      properties: properties as Attributes,
    }));
  }

  // --- editing ------------------------------------------------------------

  async domains(): Promise<Domains> {
    const [jobs, species, methods] = await Promise.all([
      this.all("jobs", { select: "id,job_number,active", order: "job_number" }),
      this.all("species", { select: "id,name,default_method_id,active", order: "name" }),
      this.all("control_methods", { select: "id,name,active", order: "name" }),
    ]);
    return { jobs, species, methods };
  }

  /** One record as the map shows it (after a save), or null if it's gone. */
  async featureById(layer: EditableLayer, id: string): Promise<Feature | null> {
    const rows = await this.rest(VIEWS[layer], { select: "*", id: `eq.${id}` });
    if (!rows.length) return null;
    const { geometry, ...properties } = rows[0];
    return { type: "Feature", geometry: geometry ?? null, properties };
  }

  /** A record drawn in the workspace. Returns its id. */
  async createFeature(layer: EditableLayer, v: FeatureValues & { geom: string; treated_at: string }): Promise<string> {
    const rows = await this.rest(TABLES[layer], { select: "id" }, {
      method: "POST",
      prefer: "return=representation",
      body: { ...v, org_id: await this.org(), source: "app", capture_mode: "drawn" },
    });
    if (!rows?.length) throw new PlatformError("Not saved: your role may not allow adding records.");
    return rows[0].id;
  }

  /** Change a record. From then on the ArcGIS import leaves it alone (009_editing.sql). */
  async updateFeature(layer: EditableLayer, id: string, v: FeatureValues): Promise<void> {
    const rows = await this.rest(TABLES[layer], { id: `eq.${id}`, select: "id" }, { method: "PATCH", body: v, prefer: "return=representation" });
    if (!rows?.length) throw new PlatformError("Not saved: the record wasn't found or your role doesn't allow editing.");
  }

  /** Hides a record (kept in the database, so the import can't bring it back). */
  async deleteFeature(layer: EditableLayer, id: string): Promise<void> {
    await this.updateFeature(layer, id, { deleted_at: new Date().toISOString() } as FeatureValues);
  }

  /** Form 1 stays in Survey123: no Form 1 records here. */
  async jobSheets(): Promise<Attributes[]> {
    return [];
  }

  // --- job sheets ----------------------------------------------------------

  async hasDailyRecords(): Promise<boolean> {
    return true; // the table is part of the platform
  }

  async setupDailyRecords(): Promise<void> {}

  async dailyRecord(jobNumber: string, date: string): Promise<DailyRecord | null> {
    const j = await this.job(jobNumber);
    if (!j) return null;
    const rows = await this.rest("daily_records", { select: "*", job_id: `eq.${j.id}`, record_date: `eq.${date}` });
    return rows.length ? rowToRecord(rows[0], j.job_number) : null;
  }

  async saveDailyRecord(r: DailyRecord): Promise<RecordId> {
    const j = await this.job(r.job_number);
    if (!j) throw new PlatformError(`Job ${r.job_number} isn't in the EcoSync database yet - add it there first.`);
    // source "app": saved here, so the ArcGIS import leaves this sheet alone from now on.
    const row = { ...recordToRow(r), org_id: await this.org(), job_id: j.id, source: "app", updated_by: this.userId || null };
    const saved: any[] = typeof r.objectId === "string"
      ? await this.rest("daily_records", { id: `eq.${r.objectId}`, select: "id" }, { method: "PATCH", body: row, prefer: "return=representation" })
      : await this.rest("daily_records", { on_conflict: "org_id,job_id,record_date", select: "id" },
          { method: "POST", body: row, prefer: "resolution=merge-duplicates,return=representation" });
    if (!saved?.length) throw new PlatformError("Save failed: the database did not return the job sheet (check your role is editor or owner).");
    return saved[0].id as string;
  }

  // --- photos, signature, PDFs ---------------------------------------------

  async attachments(recordId: RecordId): Promise<AttachmentInfo[]> {
    const rows = await this.all("attachments", {
      select: "id,name,content_type,size_bytes,storage_path",
      daily_record_id: `eq.${recordId}`,
      order: "created_at,name",
    });
    if (rows.length) {
      const signed: any[] = await this.call("/storage/v1/object/sign/attachments", {
        method: "POST",
        body: { expiresIn: SIGN_SECONDS, paths: rows.map((r) => r.storage_path) },
      });
      const byPath = new Map(signed.filter((s) => s.signedURL).map((s) => [s.path, s.signedURL as string]));
      for (const r of rows) {
        const link = byPath.get(r.storage_path);
        if (link) this.urls.set(r.id, `${CONFIG.platform.url}/storage/v1${link.startsWith("/") ? "" : "/"}${link}`);
      }
    }
    return rows.map((r) => ({
      id: r.id as string,
      name: txt(r.name),
      contentType: r.content_type || guessType(txt(r.name)),
      size: Number(r.size_bytes ?? 0),
    }));
  }

  attachmentUrl(_recordId: RecordId, attachmentId: RecordId): string {
    return this.urls.get(String(attachmentId)) ?? "";
  }

  async addAttachment(recordId: RecordId, file: Blob, name: string): Promise<void> {
    const org = await this.org();
    const path = `${org}/${recordId}/${Date.now()}_${safeName(name)}`;
    const type = file.type || guessType(name);
    await this.call(`/storage/v1/object/attachments/${encodeURI(path)}`, { method: "POST", raw: file, type });
    try {
      await this.rest("attachments", {}, {
        method: "POST",
        prefer: "return=minimal",
        body: { org_id: org, daily_record_id: recordId, kind: attachmentKind(name), storage_path: path, name, content_type: type, size_bytes: file.size },
      });
    } catch (e) {
      await this.removeFiles([path]).catch(() => undefined); // don't leave an orphan file
      throw e;
    }
  }

  async deleteAttachments(_recordId: RecordId, attachmentIds: RecordId[]): Promise<void> {
    if (attachmentIds.length === 0) return;
    const ids = `in.(${attachmentIds.join(",")})`;
    const rows: any[] = await this.rest("attachments", { select: "id,storage_path", id: ids });
    await this.removeFiles(rows.map((r) => r.storage_path));
    await this.rest("attachments", { id: ids }, { method: "DELETE" });
    for (const id of attachmentIds) this.urls.delete(String(id));
  }

  private async removeFiles(paths: string[]): Promise<void> {
    if (paths.length) await this.call("/storage/v1/object/attachments", { method: "DELETE", body: { prefixes: paths } });
  }
}
