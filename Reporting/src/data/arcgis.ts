// The ArcGIS Online DataSource. The ONLY file that knows ArcGIS REST: query
// parameters, Esri JSON geometry, the date-literal quirk. Ported from
// EcoSync\tools\spray_stats.py so the numbers match End of Day.

import { CONFIG } from "../config";
import type { AttachmentInfo, Attributes, DataSource, Feature, Geometry, Job, Lookups, Position, TargetRow } from "./types";
import { DAILY_FIELDS, toAttributes, toRecord, type DailyRecord } from "./dailySchema";

export class ArcGISError extends Error {}

async function call(url: string, token: string, params: Record<string, string>): Promise<any> {
  const r = await fetch(url, {
    method: "POST",
    body: new URLSearchParams({ f: "json", token, ...params }),
  });
  if (!r.ok) throw new ArcGISError(`${r.status} ${r.statusText} from ${shortName(url)}`);
  const j = await r.json();
  if (j.error) throw new ArcGISError(`${shortName(url)}: ${j.error.message || JSON.stringify(j.error)}`);
  return j;
}

const shortName = (url: string) => url.split("/services/").pop() ?? url;

// --- Esri JSON -> neutral geometry -----------------------------------------

/** Clockwise in lon/lat (y up) = an Esri outer ring; anticlockwise = a hole. */
function isClockwise(ring: Position[]): boolean {
  let s = 0;
  for (let i = 0; i < ring.length - 1; i++) {
    const [x1, y1] = ring[i];
    const [x2, y2] = ring[i + 1];
    s += (x2 - x1) * (y2 + y1);
  }
  return s > 0;
}

export function esriToGeometry(g: any): Geometry | null {
  if (!g) return null;
  if (typeof g.x === "number" && typeof g.y === "number") {
    return { type: "Point", coordinates: [g.x, g.y] };
  }
  if (Array.isArray(g.paths)) {
    const paths = g.paths as Position[][];
    if (paths.length === 0) return null;
    return paths.length === 1
      ? { type: "LineString", coordinates: paths[0] }
      : { type: "MultiLineString", coordinates: paths };
  }
  if (Array.isArray(g.rings)) {
    const polys: Position[][][] = [];
    for (const ring of g.rings as Position[][]) {
      if (ring.length < 4) continue;
      if (isClockwise(ring) || polys.length === 0) polys.push([ring]);
      else polys[polys.length - 1].push(ring); // hole belongs to the last outer ring
    }
    if (polys.length === 0) return null;
    return polys.length === 1
      ? { type: "Polygon", coordinates: polys[0] }
      : { type: "MultiPolygon", coordinates: polys };
  }
  return null;
}

// --- Date filter -------------------------------------------------------------

// Some services take epoch milliseconds against a date field, some insist on a
// TIMESTAMP literal in UTC. Probe once per layer and remember (as spray_stats.py).
const dateStyle = new Map<string, "epoch" | "timestamp">();

const utcStamp = (ms: number) => new Date(ms).toISOString().slice(0, 19).replace("T", " ");

async function dateWhere(url: string, token: string, field: string, t0: number, t1: number) {
  const epoch = `${field} >= ${t0} AND ${field} < ${t1}`;
  const stamp = `${field} >= TIMESTAMP '${utcStamp(t0)}' AND ${field} < TIMESTAMP '${utcStamp(t1)}'`;
  const key = `${url}|${field}`;
  if (!dateStyle.has(key)) {
    try {
      await call(`${url}/query`, token, { where: epoch, returnCountOnly: "true" });
      dateStyle.set(key, "epoch");
    } catch {
      dateStyle.set(key, "timestamp");
    }
  }
  return dateStyle.get(key) === "epoch" ? epoch : stamp;
}

// --- Paged query -------------------------------------------------------------

async function queryAll(url: string, token: string, where: string, geometry: boolean): Promise<any[]> {
  const out: any[] = [];
  for (let page = 0; page < 100; page++) {
    const j = await call(`${url}/query`, token, {
      where,
      outFields: "*",
      returnGeometry: geometry ? "true" : "false",
      outSR: "4326",
      resultOffset: String(out.length),
    });
    const feats = j.features ?? [];
    out.push(...feats);
    if (!j.exceededTransferLimit || feats.length === 0) break;
  }
  return out;
}

/** Text value of a field whatever its case on the service; "" when absent. */
const text = (a: Attributes, name: string) => {
  const k = Object.keys(a).find((x) => x.toLowerCase() === name.toLowerCase());
  return k == null || a[k] == null ? "" : String(a[k]).trim();
};

const sqlText = (v: string) => `'${v.replace(/'/g, "''")}'`;

const DAILY_TITLE = "EcoSync Daily Records";
const DAILY_SERVICE = "EcoSync_Daily_Records";

export class ArcGISSource implements DataSource {
  constructor(private token: string) {}
  private dailyUrl: string | null = null;
  private dailyFields: Set<string> | null = null;

  /**
   * Adds any field in DAILY_FIELDS the table does not have yet (e.g. "entity",
   * added after the table was first made). Plain fields only - this never
   * recreates the service. If it cannot, saving still works and just leaves
   * the new field out.
   */
  private async ensureDailyFields(url: string): Promise<void> {
    const info = await call(url, this.token, {});
    const have = new Set<string>((info.fields ?? []).map((f: any) => String(f.name).toLowerCase()));
    const missing = DAILY_FIELDS.filter((f) => !have.has(f.name));
    if (missing.length) {
      try {
        await call(`${url.replace("/rest/services/", "/rest/admin/services/")}/addToDefinition`, this.token, {
          addToDefinition: JSON.stringify({
            fields: missing.map((f) =>
              f.kind === "number"
                ? { name: f.name, type: "esriFieldTypeDouble", alias: f.label, nullable: true, editable: true }
                : { name: f.name, type: "esriFieldTypeString", alias: f.label, length: f.length ?? 255, nullable: true, editable: true },
            ),
          }),
        });
        missing.forEach((f) => have.add(f.name));
      } catch {
        /* no rights to change the table: carry on without the new fields */
      }
    }
    this.dailyFields = have;
  }

  private async daily(): Promise<string> {
    if (!this.dailyUrl) this.dailyUrl = await this.findTable(DAILY_TITLE);
    if (!this.dailyUrl) throw new ArcGISError(`The ${DAILY_TITLE} table has not been set up yet.`);
    return this.dailyUrl;
  }

  async hasDailyRecords(): Promise<boolean> {
    this.dailyUrl = this.dailyUrl ?? (await this.findTable(DAILY_TITLE));
    if (this.dailyUrl && !this.dailyFields) await this.ensureDailyFields(this.dailyUrl);
    return this.dailyUrl != null;
  }

  /**
   * Creates the hosted table the same way Survey123 Connect does (createService,
   * then addToDefinition - see Form 1\debug), with attachments on for photos,
   * the signature and the issued PDF. Plain fields can be added later without
   * recreating the service.
   */
  async setupDailyRecords(username: string): Promise<void> {
    const user = `${CONFIG.portal}/sharing/rest/content/users/${encodeURIComponent(username)}`;
    const created = await call(`${user}/createService`, this.token, {
      outputType: "featureService",
      createParameters: JSON.stringify({
        name: DAILY_SERVICE,
        serviceDescription: "EcoSync Reporting job sheets: one row per job per day",
        hasStaticData: false,
        maxRecordCount: 2000,
        supportedQueryFormats: "JSON",
        capabilities: "Create,Delete,Query,Update,Editing",
        spatialReference: { wkid: 4326 },
        allowGeometryUpdates: true,
        units: "esriMeters",
        xssPreventionInfo: { xssPreventionEnabled: true, xssPreventionRule: "InputOnly", xssInputRule: "rejectInvalid" },
      }),
    });
    const serviceUrl: string | undefined = created.serviceurl || created.encodedServiceURL;
    if (!created.success || !serviceUrl) throw new ArcGISError(`Could not create the table service: ${JSON.stringify(created)}`);

    const fields = [
      { name: "objectid", type: "esriFieldTypeOID", alias: "ObjectID", nullable: false, editable: false },
      { name: "globalid", type: "esriFieldTypeGlobalID", alias: "GlobalID", length: 38, nullable: false, editable: false },
      ...DAILY_FIELDS.map((f) =>
        f.kind === "number"
          ? { name: f.name, type: "esriFieldTypeDouble", alias: f.label, nullable: true, editable: true }
          : { name: f.name, type: "esriFieldTypeString", alias: f.label, length: f.length ?? 255, nullable: true, editable: true },
      ),
    ];
    await call(`${serviceUrl.replace("/rest/services/", "/rest/admin/services/")}/addToDefinition`, this.token, {
      addToDefinition: JSON.stringify({
        tables: [{
          id: 0,
          name: "daily_records",
          type: "Table",
          displayField: "job_number",
          description: "",
          objectIdField: "objectid",
          globalIdField: "globalid",
          hasAttachments: true,
          capabilities: "Create,Delete,Query,Update,Editing",
          supportsRollbackOnFailureParameter: true,
          fields,
          indexes: [
            { name: "GlobalIDIndex", fields: "globalid", isUnique: true, isAscending: false, description: "GlobalID index" },
            { name: "JobDateIndex", fields: "job_number,record_date", isUnique: false, isAscending: true, description: "job and day" },
          ],
          templates: [],
        }],
      }),
    });
    // Title with spaces, so findTable() and people both recognise it.
    await call(`${user}/items/${created.itemId}/update`, this.token, {
      title: DAILY_TITLE,
      tags: "EcoSync",
      snippet: "Job sheets from EcoSync Reporting, one row per job per day.",
    });
    this.dailyUrl = `${serviceUrl}/0`;
  }

  async dailyRecord(job: string, date: string): Promise<DailyRecord | null> {
    const url = await this.daily();
    const j = await call(`${url}/query`, this.token, {
      where: `job_number = ${sqlText(job)} AND record_date = ${sqlText(date)}`,
      outFields: "*",
      orderByFields: "objectid DESC",
    });
    const f = (j.features ?? [])[0];
    if (!f) return null;
    const a = f.attributes as Attributes;
    const oid = Number(a.objectid ?? a.OBJECTID);
    return toRecord(a, oid);
  }

  async saveDailyRecord(r: DailyRecord): Promise<number> {
    const url = await this.daily();
    const attributes: Record<string, unknown> = toAttributes(r);
    if (!this.dailyFields) await this.ensureDailyFields(url);
    for (const k of Object.keys(attributes)) if (!this.dailyFields!.has(k)) delete attributes[k];
    const isNew = r.objectId == null;
    if (!isNew) attributes.objectid = r.objectId;
    const j = await call(`${url}/applyEdits`, this.token, {
      [isNew ? "adds" : "updates"]: JSON.stringify([{ attributes }]),
      rollbackOnFailure: "true",
    });
    const res = (isNew ? j.addResults : j.updateResults)?.[0];
    if (!res?.success) throw new ArcGISError(`Save failed: ${res?.error?.description ?? JSON.stringify(j)}`);
    return Number(res.objectId ?? r.objectId);
  }

  async attachments(recordId: number): Promise<AttachmentInfo[]> {
    const url = await this.daily();
    const j = await call(`${url}/${recordId}/attachments`, this.token, {});
    return (j.attachmentInfos ?? []).map((a: any) => ({ id: a.id, name: a.name, contentType: a.contentType, size: a.size }));
  }

  attachmentUrl(recordId: number, attachmentId: number): string {
    return `${this.dailyUrl}/${recordId}/attachments/${attachmentId}?token=${encodeURIComponent(this.token)}`;
  }

  async addAttachment(recordId: number, file: Blob, name: string): Promise<void> {
    const url = await this.daily();
    const form = new FormData();
    form.append("attachment", file, name);
    form.append("f", "json");
    form.append("token", this.token);
    const r = await fetch(`${url}/${recordId}/addAttachment`, { method: "POST", body: form });
    const j = await r.json();
    if (j.error || !j.addAttachmentResult?.success) {
      throw new ArcGISError(`Upload of ${name} failed: ${j.error?.message ?? JSON.stringify(j.addAttachmentResult?.error ?? j)}`);
    }
  }

  async deleteAttachments(recordId: number, attachmentIds: number[]): Promise<void> {
    if (attachmentIds.length === 0) return;
    const url = await this.daily();
    await call(`${url}/${recordId}/deleteAttachments`, this.token, { attachmentIds: attachmentIds.join(",") });
  }

  /** Layer 0 of the hosted table whose item title is exactly `title`, or null. */
  private async findTable(title: string): Promise<string | null> {
    const j = await call(`${CONFIG.portal}/sharing/rest/search`, this.token, {
      q: `title:"${title}" AND type:"Feature Service"`,
      num: "20",
    });
    const item = (j.results ?? []).find((r: any) => String(r.title).trim().toLowerCase() === title.toLowerCase());
    return item?.url ? `${item.url}/0` : null;
  }

  private async table(title: string): Promise<Attributes[] | null> {
    const url = await this.findTable(title);
    if (!url) return null;
    return (await queryAll(url, this.token, "1=1", false)).map((f) => f.attributes as Attributes);
  }

  async lookups(): Promise<Lookups> {
    const t = CONFIG.tables;
    const [jobs, targets, clients, lists] = await Promise.all([
      this.table(t.jobs), this.table(t.targets), this.table(t.clients), this.table(t.lists),
    ]);
    const missing = Object.entries({ [t.jobs]: jobs, [t.targets]: targets, [t.clients]: clients, [t.lists]: lists })
      .filter(([, v]) => v == null)
      .map(([k]) => k);

    const jobFields: (keyof Job)[] = [
      "job_number", "client_code", "entity", "report_type", "report_title", "property_address",
      "owner_contact", "default_equipment", "default_chemicals", "targets", "active", "notes", "aliases", "logo",
    ];
    const listMap = new Map<string, string[]>();
    for (const r of lists ?? []) {
      const k = text(r, "list").toLowerCase();
      if (k) listMap.set(k, [...(listMap.get(k) ?? []), text(r, "value")]);
    }
    return {
      jobs: (jobs ?? [])
        .map((r) => Object.fromEntries(jobFields.map((f) => [f, text(r, f)])) as unknown as Job)
        .filter((j) => j.job_number),
      targets: (targets ?? [])
        .map((r) => ({ species: text(r, "species"), control_method: text(r, "control_method"), colour: text(r, "colour") }) as TargetRow)
        .filter((r) => r.species),
      clients: new Map((clients ?? []).map((r) => [text(r, "client_code").toUpperCase(), text(r, "client_name")])),
      lists: listMap,
      missing,
    };
  }

  async dayFeatures(layer: "points" | "polygons" | "lines", t0: number, t1: number): Promise<Feature[]> {
    const url = CONFIG.layers[layer];
    const where = await dateWhere(url, this.token, "treatment_time", t0, t1);
    const raw = await queryAll(url, this.token, where, true);
    return raw.map((f) => ({
      type: "Feature" as const,
      geometry: esriToGeometry(f.geometry),
      properties: f.attributes as Attributes,
    }));
  }

  async jobSheets(t0: number, t1: number): Promise<Attributes[]> {
    const url = CONFIG.layers.form1;
    const where = await dateWhere(url, this.token, "spray_date", t0, t1);
    const raw = await queryAll(url, this.token, where, false);
    return raw.map((f) => f.attributes as Attributes);
  }
}
