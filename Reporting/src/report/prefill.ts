// Starting values for a new job sheet: that day's Survey123 Form 1 record if
// there is one (so the switch-over is gradual), else the job's defaults.

import type { Attributes, Job } from "../data/types";
import { emptyRecord, joinList, splitList, type DailyRecord } from "../data/dailySchema";
import { attr } from "./stats";

// Form 1 stores choice NAMES; these are the labels (spelling fixed).
const FORM1_CHOICES: Record<string, string> = {
  jayme_peltz: "Jayme Peltz (0429844652)",
  calvin_mackereth: "Calvin Mackereth (0493342879)",
  spot_spray: "Vehicle Mounted Spot Sprayer",
  foilar_spray: "Vehicle Mounted Foliar Sprayer",
  backpack: "Backpack Sprayer",
  cut_swab: "Cut and swab",
  drill_fill: "Drill and Fill",
  boom_spray: "Boom Spray",
  graslan_applicator: "Graslan Applicator",
  slasher: "Slasher",
  brushcutter: "Brushcutter",
  glyphosate_xtraquatic: "Glyphosate xtraquatic 450",
  glyphosate: "Glyphosate 450",
  fluproponate: "Fluproponate",
  garlon_triclopyr: "Garlon Triclopyr",
  metsulfuron_methyl: "Metsulfuron Methyl",
  sp700_surfacant: "SP700 Surfactant",
  pine_oil: "Pine oil",
  nemo_surfactant: "Nemo Surfactant",
  brushwet: "Brushwet/Consume",
  vigilant: "Vigilant",
  graslan: "Graslan",
  llontrel: "Lontrel",
  starane_advanced: "Starane Advanced",
  uptake_oil: "Uptake oil",
  wilt700_oil: "Wilt 700 oil",
  sulfomac: "Sulfomac",
  gauntlet: "Gauntlet 720 (MSMA)",
};

/** "jayme_peltz,backpack" -> ["Jayme Peltz (0429844652)", "Backpack Sprayer"] */
export function choiceLabels(v: unknown): string[] {
  return String(v ?? "")
    .split(",")
    .map((x) => x.trim())
    .filter(Boolean)
    .map((x) => FORM1_CHOICES[x.toLowerCase()] ?? x);
}

/** Survey123 time: "12:10:00.000", "12:10", or epoch ms -> "HH:MM" local. */
export function timeText(v: unknown, timeZone: string): string {
  if (v == null || v === "") return "";
  if (typeof v === "number" && Number.isFinite(v)) {
    return new Intl.DateTimeFormat("en-GB", { timeZone, hour: "2-digit", minute: "2-digit", hourCycle: "h23" }).format(new Date(v));
  }
  const m = String(v).match(/(\d{1,2}):(\d{2})/);
  return m ? `${m[1].padStart(2, "0")}:${m[2]}` : "";
}

const num = (v: unknown) => (v == null || v === "" || !Number.isFinite(Number(v)) ? null : Number(v));
const txt = (v: unknown) => (v == null ? "" : String(v).trim());

export function prefill(job: string, date: string, timeZone: string, form1: Attributes | null, jobRow: Job | null): DailyRecord {
  const r = emptyRecord(job, date);
  if (jobRow) {
    r.entity = (jobRow.entity ?? "").trim().toUpperCase();
    r.equipment = joinList(splitList(jobRow.default_equipment));
    r.chemicals = joinList(splitList(jobRow.default_chemicals));
  }
  if (!form1) return r;
  const g = (n: string) => attr(form1, n);
  r.start_time = timeText(g("start_time"), timeZone);
  r.end_time = timeText(g("end_time"), timeZone);
  r.operators = joinList(choiceLabels(g("operator_name")));
  r.equipment = joinList(choiceLabels(g("equipment"))) || r.equipment;
  r.chemicals = joinList(choiceLabels(g("chemicals"))) || r.chemicals;
  r.tank_total_l = num(g("total_volume"));
  r.observations = txt(g("Comments"));
  r.start_temp_c = num(g("temperature"));
  r.start_rh_pct = num(g("humidity"));
  r.start_wind_kmh = num(g("wind_speed"));
  r.start_wind_dir = txt(g("wind_dir_text"));
  r.end_temp_c = num(g("end_temperature"));
  r.end_rh_pct = num(g("end_humidity"));
  r.end_wind_kmh = num(g("end_wind_speed"));
  r.end_wind_dir = txt(g("end_wind_dir_text"));
  r.weather_source = "Form 1 weather lookup - check against meter";
  return r;
}
