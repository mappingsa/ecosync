// The issued spray record as an A4 portrait PDF, following the layout of the
// Survey123 "Spray Record Form" template. Pure: takes everything as bytes and
// numbers, returns PDF bytes - so it runs in the browser and in tests alike.

import { PDFDocument, StandardFonts, rgb, type PDFFont, type PDFImage, type PDFPage } from "pdf-lib";
import type { DailyRecord } from "../data/dailySchema";
import { splitList } from "../data/dailySchema";
import type { Accuracy, SummaryRow } from "./stats";

export interface ImageBytes {
  bytes: Uint8Array;
  type: "png" | "jpg";
}

export interface LegendItem {
  label: string;
  colour: string; // #RRGGBB
  shape: "point" | "line" | "polygon";
}

export interface Brand {
  primary: string;
  accent: string;
  letterhead: string[];
  footerLeft: string;
  footerRight: string;
}

export const PLAIN_BRAND: Brand = { primary: "#1C2924", accent: "#7A847F", letterhead: [], footerLeft: "", footerRight: "" };

export interface ReportInput {
  entityName: string;
  brand?: Brand;
  logo: ImageBytes | null;
  jobNumber: string;
  client: string;
  address: string;
  ownerContact: string;
  reportTitle: string;
  dateLabel: string; // "21/11/2026"
  record: DailyRecord;
  warnings: string[];
  rows: SummaryRow[];
  areaHa: number;
  counts: { points: number; lines: number; polygons: number };
  accuracy: Accuracy;
  thresholdM: number;
  receiver: string;
  overviewMap: ImageBytes | null;
  mapAttribution?: string;
  detailMap: ImageBytes | null;
  legend: LegendItem[];
  photos: (ImageBytes & { name: string })[];
  signature: ImageBytes | null;
  issuedBy: string;
  issuedAt: string; // display text
  version: number;
}

// --- Text that the standard PDF fonts can draw --------------------------------

/** Standard PDF fonts only cover Western European characters: fold the rest. */
export function pdfSafe(s: string): string {
  return String(s ?? "")
    .normalize("NFD")
    .replace(/[̀-ͯ]/g, "") // combining marks: Wiluṟara -> Wilurara
    .replace(/[‘’]/g, "'")
    .replace(/[“”]/g, '"')
    .replace(/—/g, "-")
    .replace(/[^\x09\x0a\x0d\x20-\x7e\xa0-\xff–•…]/g, "?");
}

const hex = (c: string) => {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(c.trim());
  return m ? rgb(parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255) : rgb(0.5, 0.5, 0.5);
};

const INK = rgb(0.11, 0.16, 0.14);
const QUIET = rgb(0.38, 0.42, 0.4);
const RULE = rgb(0.75, 0.78, 0.76);
const WARN = rgb(0.6, 0.33, 0);
const W = 595.28, H = 841.89, M = 50, CW = W - 2 * M;

class Layout {
  page!: PDFPage;
  y = 0;
  primary = INK;
  accent = RULE;
  constructor(public doc: PDFDocument, public font: PDFFont, public bold: PDFFont) {
    this.newPage();
  }
  newPage() {
    this.page = this.doc.addPage([W, H]);
    this.y = H - M;
  }
  need(h: number) {
    if (this.y - h < M + 30) this.newPage();
  }
  wrap(text: string, size: number, width: number, font = this.font): string[] {
    const out: string[] = [];
    for (const para of pdfSafe(text).split(/\r?\n/)) {
      let line = "";
      for (const word of para.split(/\s+/)) {
        const t = line ? `${line} ${word}` : word;
        if (font.widthOfTextAtSize(t, size) <= width || !line) line = t;
        else { out.push(line); line = word; }
      }
      out.push(line);
    }
    return out;
  }
  text(s: string, x: number, size = 10, font = this.font, color = INK) {
    this.page.drawText(pdfSafe(s), { x, y: this.y, size, font, color });
  }
  /** Wrapped paragraph at x, advancing y. */
  para(s: string, x = M, width = CW, size = 10, font = this.font, color = INK) {
    for (const line of this.wrap(s, size, width, font)) {
      this.need(size + 4);
      this.text(line, x, size, font, color);
      this.y -= size + 4;
    }
  }
  /** Section heading; `keep` = height that must fit with it, so short blocks never split. */
  section(title: string, keep = 48) {
    this.need(keep);
    this.y -= 14;
    this.text(title.toUpperCase(), M, 11, this.bold, this.primary);
    this.y -= 6;
    this.page.drawLine({ start: { x: M, y: this.y }, end: { x: M + CW, y: this.y }, thickness: 0.6, color: RULE });
    this.page.drawLine({ start: { x: M, y: this.y }, end: { x: M + 56, y: this.y }, thickness: 2, color: this.accent });
    this.y -= 16;
  }
  /** "Label:   value" rows with a fixed label column; values wrap. */
  field(label: string, value: string, labelW = 130) {
    const lines = this.wrap(value || "–", 10, CW - labelW);
    this.need(lines.length * 14);
    this.text(`${label}:`, M, 10, this.bold);
    for (const l of lines) {
      this.text(l, M + labelW, 10);
      this.y -= 14;
    }
  }
  table(headers: string[], rows: string[][], widths: number[], align: ("l" | "r")[] = []) {
    const rowH = 16;
    const draw = (cells: string[], font: PDFFont, color = INK) => {
      let x = M;
      cells.forEach((c, i) => {
        const s = pdfSafe(c);
        const w = font.widthOfTextAtSize(s, 9.5);
        const tx = align[i] === "r" ? x + widths[i] - w - 4 : x + 4;
        this.page.drawText(s, { x: tx, y: this.y, size: 9.5, font, color });
        x += widths[i];
      });
    };
    const tw = widths.reduce((a, b) => a + b, 0);
    this.need(rowH * 2);
    this.page.drawRectangle({ x: M, y: this.y - 5, width: tw, height: rowH, color: this.primary });
    draw(headers, this.bold, rgb(1, 1, 1));
    this.y -= rowH;
    rows.forEach((r, i) => {
      this.need(rowH);
      if (i % 2 === 1) this.page.drawRectangle({ x: M, y: this.y - 5, width: tw, height: rowH, color: rgb(0.95, 0.96, 0.95) });
      draw(r, this.font);
      this.y -= rowH;
    });
    this.page.drawLine({ start: { x: M, y: this.y + 11 }, end: { x: M + tw, y: this.y + 11 }, thickness: 0.5, color: RULE });
    this.y -= 2;
  }
  /** Image scaled to width (capped at maxH), centred; advances y. */
  image(img: PDFImage, width: number, maxH: number, x?: number) {
    const s = Math.min(width / img.width, maxH / img.height);
    const w = img.width * s, h = img.height * s;
    this.need(h + 6);
    this.page.drawImage(img, { x: x ?? M + (CW - w) / 2, y: this.y - h, width: w, height: h });
    this.page.drawRectangle({ x: x ?? M + (CW - w) / 2, y: this.y - h, width: w, height: h, borderColor: RULE, borderWidth: 0.5 });
    this.y -= h + 8;
  }
}

const n = (v: number | null | undefined, unit = "", dp?: number) =>
  v == null || !Number.isFinite(v) ? "–" : `${dp == null ? v : v.toFixed(dp)}${unit}`;

export async function buildReportPdf(r: ReportInput): Promise<Uint8Array> {
  const doc = await PDFDocument.create();
  doc.setTitle(pdfSafe(`Spray Record ${r.jobNumber} ${r.dateLabel}`));
  doc.setAuthor(pdfSafe(r.entityName));
  doc.setCreator("EcoSync Reporting");
  const font = await doc.embedFont(StandardFonts.Helvetica);
  const bold = await doc.embedFont(StandardFonts.HelveticaBold);
  const embed = async (i: ImageBytes | null) => (i ? (i.type === "png" ? doc.embedPng(i.bytes) : doc.embedJpg(i.bytes)) : null);
  const brand = r.brand ?? PLAIN_BRAND;
  const L = new Layout(doc, font, bold);
  L.primary = hex(brand.primary);
  L.accent = hex(brand.accent);
  const rec = r.record;

  // Letterhead: business details top left, logo top right (as on the invoice)
  const logo = await embed(r.logo);
  let logoH = 0;
  if (logo) {
    const s = Math.min(150 / logo.width, 70 / logo.height);
    logoH = logo.height * s;
    L.page.drawImage(logo, { x: M + CW - logo.width * s, y: H - M - logoH + 10, width: logo.width * s, height: logoH });
  }
  const top = L.y;
  brand.letterhead.forEach((line, i) => {
    L.text(line, M, i === 0 ? 11 : 8.5, i === 0 ? bold : font, i === 0 ? L.primary : QUIET);
    L.y -= i === 0 ? 14 : 11;
  });
  L.y = Math.min(L.y, top - logoH + 6) - 10;

  // Title bar in the entity colour
  L.page.drawRectangle({ x: M, y: L.y - 26, width: CW, height: 34, color: L.primary });
  L.y -= 14;
  L.text("SPRAY RECORD FORM", M + 12, 15, bold, rgb(1, 1, 1));
  const sub = "Record Keeping Form for Chemical Use";
  L.page.drawText(sub, { x: M + CW - 12 - font.widthOfTextAtSize(sub, 9.5), y: L.y + 1, size: 9.5, font, color: rgb(1, 1, 1) });
  L.y -= 22;

  L.section("Job & site details");
  L.field("Job Number", r.jobNumber + (r.reportTitle ? ` - ${r.reportTitle}` : ""));
  L.field("Employer / Client", r.client);
  L.field("Property Address", r.address);
  if (r.ownerContact) L.field("Owner / Contact", r.ownerContact);

  L.section("Operational logistics");
  L.field("Date of Application", r.dateLabel);
  L.field("Start Time", rec.start_time);
  L.field("End Time", rec.end_time);
  L.field("Spray Operator", splitList(rec.operators).join(", "));
  L.field("Equipment Type", splitList(rec.equipment).join(", "));
  L.field("Chemical Product", splitList(rec.chemicals).join(", "));

  L.section("Weather verification compliance", 140);
  L.table(
    ["", "At start", "At finish"],
    [
      ["Air Temperature", n(rec.start_temp_c, " °C"), n(rec.end_temp_c, " °C")],
      ["Relative Humidity", n(rec.start_rh_pct, "%"), n(rec.end_rh_pct, "%")],
      ["Wind speed", n(rec.start_wind_kmh, " km/h"), n(rec.end_wind_kmh, " km/h")],
      ["Wind direction", rec.start_wind_dir || "–", rec.end_wind_dir || "–"],
      ["Delta T", n(rec.start_delta_t), n(rec.end_delta_t)],
    ],
    [170, 160, 160],
  );
  if (rec.weather_source) L.para(`Source of readings: ${rec.weather_source}`, M, CW, 9, font, QUIET);
  for (const w of r.warnings) L.para(`! ${w}`, M, CW, 9.5, bold, WARN);

  L.section("Field observations & site comments");
  L.para(rec.observations || "–");
  if (rec.flag_issues) {
    L.y -= 4;
    L.para("Flag issues:", M, CW, 10, bold);
    L.para(rec.flag_issues);
  }

  L.section("Total litres of chemical sprayed out for the day");
  L.para(rec.tank_total_l == null ? "–" : `${rec.tank_total_l} L`);

  L.section("Treatments", 60 + Math.min(r.rows.length, 6) * 16);
  L.table(
    ["Target", "Method", "Points", "Lines (m)", "Polygons", "Area (ha)"],
    r.rows.map((x) => [
      x.species, x.method, String(x.points),
      x.lines ? `${x.lines} (${x.lengthM.toFixed(0)})` : "0",
      String(x.polygons),
      x.areaM2 ? (x.areaM2 / 10000).toFixed(3) : "–",
    ]),
    [140, 105, 55, 75, 60, 60],
    ["l", "l", "r", "r", "r", "r"],
  );
  L.para(`Area treated (from polygons): ${r.areaHa.toFixed(2)} ha`, M, CW, 10, bold);

  L.section("Positional data quality", 130);
  L.field("Records captured", `${r.counts.points} points, ${r.counts.lines} lines, ${r.counts.polygons} polygons`, 140);
  L.field("Average accuracy", n(r.accuracy.average, " m", 1), 140);
  L.field("Worst recorded", n(r.accuracy.worst, " m", 1), 140);
  L.field(`Within ${r.thresholdM} m`, r.accuracy.withinPct == null ? "–" : `${r.accuracy.withinPct}%`, 140);
  if (r.receiver) L.field("Receiver", r.receiver, 140);
  L.para("Every individual record carries its own accuracy value in the layer.", M, CW, 9, font, QUIET);

  L.section("Control data map");
  const overview = await embed(r.overviewMap);
  const detail = await embed(r.detailMap);
  // Caption and map move to a new page together, never apart.
  const mapBlock = (caption: string, img: PDFImage) => {
    const s = Math.min(CW / img.width, 280 / img.height);
    L.need(img.height * s + 26);
    L.para(caption, M, CW, 9, font, QUIET);
    L.image(img, CW, 280);
  };
  if (overview) mapBlock("Overview - the full extent of the day's work", overview);
  if (detail) mapBlock("Detail - the main working area", detail);
  if (!overview && !detail) L.para("No mapped records for this job on this day.", M, CW, 10, font, QUIET);
  else if (r.mapAttribution) L.para(r.mapAttribution, M, CW, 7.5, font, QUIET);
  if (r.legend.length) {
    L.need(20 + r.legend.length * 15);
    L.y -= 4;
    L.text("LEGEND", M, 9.5, bold, QUIET);
    L.y -= 15;
    for (const item of r.legend) {
      L.need(15);
      const c = hex(item.colour), cy = L.y + 3.5;
      if (item.shape === "point") L.page.drawCircle({ x: M + 6, y: cy, size: 4, color: c, borderColor: rgb(0, 0, 0), borderWidth: 0.5 });
      else if (item.shape === "line") L.page.drawLine({ start: { x: M, y: cy }, end: { x: M + 12, y: cy }, thickness: 2.5, color: c });
      else L.page.drawRectangle({ x: M + 1, y: cy - 4.5, width: 10, height: 9, color: c, opacity: 0.45, borderColor: c, borderWidth: 1.2 });
      L.text(item.label, M + 20, 9.5);
      L.y -= 15;
    }
  }

  if (r.photos.length) {
    L.section("Site photos");
    const colW = (CW - 12) / 2;
    for (let i = 0; i < r.photos.length; i += 2) {
      const pair = await Promise.all(r.photos.slice(i, i + 2).map((p) => embed(p)));
      const hs = pair.map((img) => (img ? Math.min(colW / img.width, 200 / img.height) * img.height : 0));
      const rowH = Math.max(...hs);
      L.need(rowH + 22);
      pair.forEach((img, j) => {
        if (!img) return;
        const s = Math.min(colW / img.width, 200 / img.height);
        const x = M + j * (colW + 12);
        L.page.drawImage(img, { x, y: L.y - img.height * s, width: img.width * s, height: img.height * s });
        L.page.drawText(pdfSafe(r.photos[i + j].name), { x, y: L.y - rowH - 11, size: 8, font, color: QUIET });
      });
      L.y -= rowH + 22;
    }
  }

  // Declaration and signature
  L.need(140);
  L.y -= 20;
  L.text("This is an accurate record of chemical use.", M, 10.5, bold);
  L.y -= 64; // room for the signature above its line
  const sig = await embed(r.signature);
  L.text("Operator signature:", M, 10);
  if (sig) {
    const s = Math.min(180 / sig.width, 55 / sig.height);
    L.page.drawImage(sig, { x: M + 110, y: L.y - 12, width: sig.width * s, height: sig.height * s });
  }
  L.text(`Date: ${r.dateLabel}`, M + CW - 120, 10);
  L.page.drawLine({ start: { x: M + 108, y: L.y - 14 }, end: { x: M + 300, y: L.y - 14 }, thickness: 0.5, color: RULE });

  // Footer on every page: brand strip, then the record's own line
  const pages = doc.getPages();
  pages.forEach((p, i) => {
    const y0 = M - 8;
    p.drawLine({ start: { x: M, y: y0 }, end: { x: M + CW, y: y0 }, thickness: 1.2, color: L.accent });
    const bl = pdfSafe(brand.footerLeft || r.entityName);
    const br = pdfSafe(brand.footerRight);
    const pageNo = `Page ${i + 1} of ${pages.length}`;
    p.drawText(bl, { x: M, y: y0 - 10, size: 7, font: bold, color: L.primary });
    p.drawText(pageNo, { x: W - M - bold.widthOfTextAtSize(pageNo, 7), y: y0 - 10, size: 7, font: bold, color: L.primary });
    if (br) p.drawText(br, { x: M, y: y0 - 19, size: 7, font, color: QUIET });
    const rec = pdfSafe(`Spray record ${r.jobNumber} · ${r.dateLabel}`);
    p.drawText(rec, { x: M, y: y0 - (br ? 28 : 19), size: 7, font, color: QUIET });
  });

  return doc.save();
}
