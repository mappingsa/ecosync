// Gathers everything for the PDF (maps drawn off-screen, photos and signature
// fetched from the record's attachments, the letterhead logo), builds it, and
// attaches it to the Daily Record as the next version.

import { CONFIG } from "../config";
import type { DailyRecord } from "../data/dailySchema";
import type { DataSource, Feature, TargetRow } from "../data/types";
import { buildReportPdf, type Brand, type ImageBytes, type LegendItem } from "../report/pdf";
import { bounds, detailBounds, type Accuracy, type SummaryRow } from "../report/stats";
import { colourKey } from "../report/colours";
import { weatherWarnings } from "../report/weather";
import { renderMapPng } from "./mapLayers";

export interface IssueContext {
  source: DataSource;
  entityName: string;
  brand?: Brand;
  logoUrl: string | null;
  jobNumber: string;
  client: string;
  address: string;
  ownerContact: string;
  reportTitle: string;
  features: { points: Feature[]; lines: Feature[]; polygons: Feature[] };
  rows: SummaryRow[];
  areaHa: number;
  accuracy: Accuracy;
  colours: Map<string, string>;
  targets: TargetRow[];
}

export interface IssueOptions {
  /** Print the weather warnings on the PDF. The readings and Delta T always print. */
  includeWarnings: boolean;
}

export const SIGNATURE = /^signature.*\.png$/i;
export const PDF = /\.pdf$/i;

const kind = (contentType: string, name: string): "png" | "jpg" | null =>
  /png/i.test(contentType) || /\.png$/i.test(name) ? "png" : /jpe?g/i.test(contentType) || /\.jpe?g$/i.test(name) ? "jpg" : null;

async function fetchBytes(url: string): Promise<Uint8Array | null> {
  try {
    const r = await fetch(url);
    return r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
  } catch {
    return null;
  }
}

const dmy = (iso: string) => iso.split("-").reverse().join("/");

export async function issueReport(
  ctx: IssueContext,
  rec: DailyRecord,
  username: string,
  progress: (msg: string) => void,
  options: IssueOptions = { includeWarnings: false },
): Promise<{ blob: Blob; name: string; version: number }> {
  const id = rec.objectId!;
  const atts = await ctx.source.attachments(id);
  const version = atts.filter((a) => PDF.test(a.name)).length + 1;

  progress("Fetching photos and signature…");
  const photos: (ImageBytes & { name: string })[] = [];
  for (const a of atts.filter((x) => !SIGNATURE.test(x.name) && !PDF.test(x.name))) {
    const type = kind(a.contentType, a.name);
    const bytes = type && (await fetchBytes(ctx.source.attachmentUrl(id, a.id)));
    if (type && bytes) photos.push({ bytes, type, name: a.name });
  }
  const sigAtt = atts.filter((x) => SIGNATURE.test(x.name)).pop();
  const sigBytes = sigAtt ? await fetchBytes(ctx.source.attachmentUrl(id, sigAtt.id)) : null;
  const logoBytes = ctx.logoUrl ? await fetchBytes(ctx.logoUrl) : null;
  const logoType = ctx.logoUrl ? kind("", ctx.logoUrl) : null;

  progress("Drawing the maps…");
  const all = [...ctx.features.points, ...ctx.features.lines, ...ctx.features.polygons];
  const ob = bounds(all);
  const db = detailBounds(all);
  const overview = ob ? await renderMapPng(ctx.features, ctx.colours, ctx.targets, ob) : null;
  const detail = db ? await renderMapPng(ctx.features, ctx.colours, ctx.targets, db) : null;

  const legend: LegendItem[] = [];
  for (const r of ctx.rows) {
    const colour = ctx.colours.get(colourKey(r.species, r.method)) ?? "#888888";
    const kinds = [r.points && "point", r.lines && "line", r.polygons && "polygon"].filter(Boolean) as LegendItem["shape"][];
    for (const shape of kinds) {
      const word = shape === "point" ? "spray point" : shape === "line" ? "line" : "treated area";
      legend.push({ label: `${r.species} – ${r.method} ${word}`, colour, shape });
    }
  }

  progress("Building the PDF…");
  const now = new Date();
  const pdf = await buildReportPdf({
    entityName: ctx.entityName,
    brand: ctx.brand,
    logo: logoBytes && logoType ? { bytes: logoBytes, type: logoType } : null,
    jobNumber: ctx.jobNumber,
    client: ctx.client,
    address: ctx.address,
    ownerContact: ctx.ownerContact,
    reportTitle: ctx.reportTitle,
    dateLabel: dmy(rec.record_date),
    record: rec,
    warnings: !options.includeWarnings ? [] : [
      ...weatherWarnings("Start", { temp: rec.start_temp_c, rh: rec.start_rh_pct, wind: rec.start_wind_kmh, dt: rec.start_delta_t }, CONFIG.sprayLimits),
      ...weatherWarnings("Finish", { temp: rec.end_temp_c, rh: rec.end_rh_pct, wind: rec.end_wind_kmh, dt: rec.end_delta_t }, CONFIG.sprayLimits),
    ],
    rows: ctx.rows,
    areaHa: ctx.areaHa,
    counts: { points: ctx.features.points.length, lines: ctx.features.lines.length, polygons: ctx.features.polygons.length },
    accuracy: ctx.accuracy,
    thresholdM: CONFIG.accuracyThresholdM,
    receiver: CONFIG.receiver,
    overviewMap: overview && { bytes: overview, type: "png" },
    mapAttribution: CONFIG.basemap.attribution,
    detailMap: detail && { bytes: detail, type: "png" },
    legend,
    photos,
    signature: sigBytes ? { bytes: sigBytes, type: "png" } : null,
    issuedBy: username,
    issuedAt: now.toLocaleString("en-AU", { timeZone: CONFIG.timeZone, dateStyle: "short", timeStyle: "short" }),
    version,
  });

  progress("Attaching the PDF to the record…");
  const name = `Spray Record ${ctx.jobNumber} ${rec.record_date} v${version}.pdf`.replace(/[\\/:*?"<>|]/g, "-");
  const blob = new Blob([pdf as BlobPart], { type: "application/pdf" });
  await ctx.source.addAttachment(id, blob, name);
  return { blob, name, version };
}
