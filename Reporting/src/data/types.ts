import type { DailyRecord } from "./dailySchema";

// Vendor-neutral shapes. Report and map code only ever see these, never an
// ArcGIS response, so swapping ArcGIS for PostGIS or our own backend means
// writing another DataSource - not touching the report.

export type Position = [number, number]; // [lon, lat], WGS84

export type Geometry =
  | { type: "Point"; coordinates: Position }
  | { type: "LineString"; coordinates: Position[] }
  | { type: "MultiLineString"; coordinates: Position[][] }
  | { type: "Polygon"; coordinates: Position[][] }
  | { type: "MultiPolygon"; coordinates: Position[][][] };

export type Attributes = Record<string, unknown>;

export interface Feature {
  type: "Feature";
  geometry: Geometry | null;
  properties: Attributes;
}

export interface Session {
  token: string;
  username: string;
  expires: number; // epoch ms
}

export interface Job {
  job_number: string;
  client_code: string;
  entity: string; // "MSA" | "FPE" - picks the letterhead
  report_type: string;
  report_title: string;
  property_address: string;
  owner_contact: string;
  default_equipment: string;
  default_chemicals: string;
  targets: string;
  active: string;
  notes: string;
  logo: string; // optional: file in public/logos, or a web address - replaces the entity logo for this job
  aliases: string; // old names for this job, separated by ";" - matched as the same job
}

export interface TargetRow {
  species: string;
  control_method: string;
  colour: string;
}

export interface Lookups {
  jobs: Job[];
  targets: TargetRow[];
  clients: Map<string, string>; // code -> full name
  lists: Map<string, string[]>; // "operator" | "equipment" | "chemical" -> values
  missing: string[]; // lookup tables not found (app falls back to config)
}

export interface AttachmentInfo {
  id: number;
  name: string;
  contentType: string;
  size: number;
}

export interface DataSource {
  /** The Jobs, Targets, Clients and Lists tables. */
  lookups(): Promise<Lookups>;
  /** Every feature on a layer whose treatment time falls in [t0, t1). */
  dayFeatures(layer: "points" | "polygons" | "lines", t0: number, t1: number): Promise<Feature[]>;
  /** Form 1 job-sheet records for the day (attributes only). */
  jobSheets(t0: number, t1: number): Promise<Attributes[]>;

  /** False until the Daily Records table exists. */
  hasDailyRecords(): Promise<boolean>;
  /** One-time: create the Daily Records table. */
  setupDailyRecords(username: string): Promise<void>;
  dailyRecord(job: string, date: string): Promise<DailyRecord | null>;
  /** Insert or update; returns the record's id. */
  saveDailyRecord(r: DailyRecord): Promise<number>;
  attachments(recordId: number): Promise<AttachmentInfo[]>;
  attachmentUrl(recordId: number, attachmentId: number): string;
  addAttachment(recordId: number, file: Blob, name: string): Promise<void>;
  deleteAttachments(recordId: number, attachmentIds: number[]): Promise<void>;
}
