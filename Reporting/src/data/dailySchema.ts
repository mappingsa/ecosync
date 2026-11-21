// The Daily Records table: one row per job per day (the job sheet).
// Defined once here; the ArcGIS setup, the save code and a future PostGIS
// table are all built from this list.

export type FieldKind = "text" | "number";

export const DAILY_FIELDS: { name: string; kind: FieldKind; length?: number; label: string }[] = [
  { name: "job_number", kind: "text", length: 100, label: "Job number" },
  { name: "record_date", kind: "text", length: 10, label: "Date (YYYY-MM-DD, local)" },
  { name: "start_time", kind: "text", length: 5, label: "Start time" },
  { name: "end_time", kind: "text", length: 5, label: "End time" },
  { name: "operators", kind: "text", length: 1000, label: "Operators" },
  { name: "equipment", kind: "text", length: 1000, label: "Equipment" },
  { name: "chemicals", kind: "text", length: 2000, label: "Chemicals and rates" },
  { name: "tank_total_l", kind: "number", label: "Total sprayed out (L)" },
  { name: "start_temp_c", kind: "number", label: "Temperature at start (°C)" },
  { name: "start_rh_pct", kind: "number", label: "Relative humidity at start (%)" },
  { name: "start_wind_kmh", kind: "number", label: "Wind speed at start (km/h)" },
  { name: "start_wind_dir", kind: "text", length: 10, label: "Wind direction at start" },
  { name: "start_delta_t", kind: "number", label: "Delta T at start" },
  { name: "end_temp_c", kind: "number", label: "Temperature at finish (°C)" },
  { name: "end_rh_pct", kind: "number", label: "Relative humidity at finish (%)" },
  { name: "end_wind_kmh", kind: "number", label: "Wind speed at finish (km/h)" },
  { name: "end_wind_dir", kind: "text", length: 10, label: "Wind direction at finish" },
  { name: "end_delta_t", kind: "number", label: "Delta T at finish" },
  { name: "weather_source", kind: "text", length: 100, label: "Weather source" },
  { name: "observations", kind: "text", length: 8000, label: "Field observations" },
  { name: "flag_issues", kind: "text", length: 4000, label: "Flag issues" },
  { name: "status", kind: "text", length: 20, label: "Status" },
  { name: "entity", kind: "text", length: 10, label: "Letterhead (MSA or FPE)" },
  { name: "updated_by", kind: "text", length: 100, label: "Last saved by" },
  { name: "updated_at", kind: "text", length: 30, label: "Last saved at" },
];

export interface DailyRecord {
  objectId?: number;
  job_number: string;
  record_date: string;
  start_time: string;
  end_time: string;
  operators: string; // "; "-separated
  equipment: string;
  chemicals: string;
  tank_total_l: number | null;
  start_temp_c: number | null;
  start_rh_pct: number | null;
  start_wind_kmh: number | null;
  start_wind_dir: string;
  start_delta_t: number | null;
  end_temp_c: number | null;
  end_rh_pct: number | null;
  end_wind_kmh: number | null;
  end_wind_dir: string;
  end_delta_t: number | null;
  weather_source: string;
  observations: string;
  flag_issues: string;
  status: string;
  entity: string; // letterhead for this report; starts as the job's entity
  updated_by: string;
  updated_at: string;
}

export function emptyRecord(job: string, date: string): DailyRecord {
  const r: any = { job_number: job, record_date: date, status: "draft" };
  for (const f of DAILY_FIELDS) if (!(f.name in r)) r[f.name] = f.kind === "number" ? null : "";
  return r as DailyRecord;
}

/** Plain attribute object -> DailyRecord, case-insensitive, with types enforced. */
export function toRecord(a: Record<string, unknown>, objectId?: number): DailyRecord {
  const get = (n: string) => a[Object.keys(a).find((k) => k.toLowerCase() === n) ?? n];
  const r: any = { objectId };
  for (const f of DAILY_FIELDS) {
    const v = get(f.name);
    if (f.kind === "number") r[f.name] = v == null || v === "" || !Number.isFinite(Number(v)) ? null : Number(v);
    else r[f.name] = v == null ? "" : String(v);
  }
  return r as DailyRecord;
}

/** DailyRecord -> attribute object for saving (no objectId, text trimmed to field length). */
export function toAttributes(r: DailyRecord): Record<string, string | number | null> {
  const out: Record<string, string | number | null> = {};
  for (const f of DAILY_FIELDS) {
    const v = (r as any)[f.name];
    out[f.name] = f.kind === "number" ? (v == null || !Number.isFinite(v) ? null : v) : String(v ?? "").slice(0, f.length);
  }
  return out;
}

export const splitList = (s: string) => s.split(";").map((x) => x.trim()).filter(Boolean);
export const joinList = (xs: string[]) => xs.join("; ");
