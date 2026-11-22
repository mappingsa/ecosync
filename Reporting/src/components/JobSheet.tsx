import { useEffect, useMemo, useState } from "react";
import { CONFIG } from "../config";
import type { AttachmentInfo, Attributes, DataSource, Job } from "../data/types";
import { joinList, splitList, type DailyRecord } from "../data/dailySchema";
import { prefill } from "../report/prefill";
import { deltaT, weatherWarnings } from "../report/weather";
import { dayWindow } from "../report/stats";
import { lookupWeather } from "./weatherLookup";
import { SignaturePad } from "./SignaturePad";
import { jpgName, shrinkImage } from "./images";
import { PDF, SIGNATURE, type IssueOptions } from "./issueReport";

interface Props {
  source: DataSource;
  username: string;
  job: string; // canonical job number
  date: string; // YYYY-MM-DD local
  jobRow: Job | null;
  lists: Map<string, string[]>;
  form1: Attributes | null;
  /** Where the day's work was ([lon, lat]), for the weather lookup. */
  location: [number, number] | null;
  /** Builds the PDF for a saved record and attaches it; provided by the report screen. */
  onIssue?: (rec: DailyRecord, progress: (msg: string) => void, options: IssueOptions) => Promise<{ blob: Blob; name: string; version: number }>;
}

export function JobSheet({ source, username, job, date, jobRow, lists, form1, location, onIssue }: Props) {
  const [ready, setReady] = useState<boolean | null>(null); // null = checking
  const [rec, setRec] = useState<DailyRecord | null>(null);
  const [atts, setAtts] = useState<AttachmentInfo[]>([]);
  const [newPhotos, setNewPhotos] = useState<File[]>([]);
  const [signature, setSignature] = useState<Blob | null>(null);
  const [busy, setBusy] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [saved, setSaved] = useState("");
  const [printWarnings, setPrintWarnings] = useState(false);
  // Delta T typed in by hand (true) or calculated from temperature and humidity (false).
  const [dtManual, setDtManual] = useState({ start: false, end: false });
  // Readings that came from the lookup (so a new time may replace them) vs typed by hand.
  const [fromLookup, setFromLookup] = useState({ start: false, end: false });
  const [lookedUpFor, setLookedUpFor] = useState({ start: "", end: "" });
  const [weatherMsg, setWeatherMsg] = useState("");

  const load = async () => {
    setError(null);
    setSaved("");
    setNewPhotos([]);
    setSignature(null);
    try {
      const ok = await source.hasDailyRecords();
      setReady(ok);
      if (!ok) return;
      const existing = await source.dailyRecord(job, date);
      if (existing && !existing.entity) existing.entity = (jobRow?.entity ?? "").trim().toUpperCase();
      const r = existing ?? prefill(job, date, CONFIG.timeZone, form1, jobRow);
      setRec(r);
      // A saved Delta T that differs from the calculation was typed in: keep it.
      const differs = (saved: number | null, t: number | null, h: number | null) =>
        saved != null && deltaT(t, h) != null && Math.abs(saved - deltaT(t, h)!) > 0.05;
      setDtManual({
        start: differs(r.start_delta_t, r.start_temp_c, r.start_rh_pct) || (r.start_delta_t != null && deltaT(r.start_temp_c, r.start_rh_pct) == null),
        end: differs(r.end_delta_t, r.end_temp_c, r.end_rh_pct) || (r.end_delta_t != null && deltaT(r.end_temp_c, r.end_rh_pct) == null),
      });
      const empty = (t: number | null, h: number | null, w: number | null) => t == null && h == null && w == null;
      setFromLookup({
        start: empty(r.start_temp_c, r.start_rh_pct, r.start_wind_kmh) || /lookup/i.test(r.weather_source),
        end: empty(r.end_temp_c, r.end_rh_pct, r.end_wind_kmh) || /lookup/i.test(r.weather_source),
      });
      setLookedUpFor({ start: r.start_time, end: r.end_time }); // loading a sheet never triggers a lookup
      setAtts(existing?.objectId != null ? await source.attachments(existing.objectId) : []);
    } catch (e: any) {
      setError(e.message ?? String(e));
    }
  };
  useEffect(() => { load(); }, [job, date]); // eslint-disable-line react-hooks/exhaustive-deps

  const setup = async () => {
    setBusy("Creating the EcoSync Daily Records table…");
    setError(null);
    try {
      await source.setupDailyRecords(username);
      await load();
    } catch (e: any) {
      setError(`Setup failed: ${e.message ?? e}`);
    } finally {
      setBusy("");
    }
  };

  const startDT = rec ? (dtManual.start ? rec.start_delta_t : deltaT(rec.start_temp_c, rec.start_rh_pct)) : null;
  const endDT = rec ? (dtManual.end ? rec.end_delta_t : deltaT(rec.end_temp_c, rec.end_rh_pct)) : null;
  const warnings = useMemo(() => {
    if (!rec) return [];
    return [
      ...weatherWarnings("Start", { temp: rec.start_temp_c, rh: rec.start_rh_pct, wind: rec.start_wind_kmh, dt: startDT }, CONFIG.sprayLimits),
      ...weatherWarnings("Finish", { temp: rec.end_temp_c, rh: rec.end_rh_pct, wind: rec.end_wind_kmh, dt: endDT }, CONFIG.sprayLimits),
    ];
  }, [rec, startDT, endDT]);

  /** Fill start or finish readings from the weather lookup for that time. */
  const lookup = async (side: "start" | "end", time: string) => {
    if (!rec || !/^\d{2}:\d{2}$/.test(time)) return;
    if (!location) {
      setWeatherMsg("No mapped records for this job today, so there is no location for the weather lookup.");
      return;
    }
    const [h, m] = time.split(":").map(Number);
    const unix = (dayWindow(date, CONFIG.timeZone)[0] + (h * 60 + m) * 60_000) / 1000;
    setWeatherMsg(`Looking up the weather at ${time}…`);
    try {
      const w = await lookupWeather(location[1], location[0], unix);
      setRec((cur) => cur && {
        ...cur,
        [`${side}_temp_c`]: w.temp_c,
        [`${side}_rh_pct`]: w.rh_pct,
        [`${side}_wind_kmh`]: w.wind_kmh,
        [`${side}_wind_dir`]: w.wind_dir,
        weather_source: !cur.weather_source || /lookup|form 1/i.test(cur.weather_source)
          ? "Weather lookup (OpenWeather) - check against meter"
          : cur.weather_source,
      });
      setDtManual((d) => ({ ...d, [side]: false }));
      setFromLookup((f) => ({ ...f, [side]: true }));
      setLookedUpFor((l) => ({ ...l, [side]: time }));
      setSaved("");
      setWeatherMsg(`${side === "start" ? "Start" : "Finish"} weather filled in for ${time}. Edit any value if your meter read differently.`);
    } catch (e: any) {
      setWeatherMsg(e.message ?? String(e));
    }
  };

  // Setting a start or finish time fills that side's weather - unless the
  // readings there were typed in by hand. A short pause lets the time picker settle.
  useEffect(() => {
    if (!rec) return;
    const timers: number[] = [];
    for (const side of ["start", "end"] as const) {
      const t = side === "start" ? rec.start_time : rec.end_time;
      if (/^\d{2}:\d{2}$/.test(t) && t !== lookedUpFor[side] && fromLookup[side]) {
        timers.push(window.setTimeout(() => lookup(side, t), 900));
      }
    }
    return () => timers.forEach(clearTimeout);
  }, [rec?.start_time, rec?.end_time]); // eslint-disable-line react-hooks/exhaustive-deps

  if (ready === null) return <p className="hint">Checking for the Daily Records table…</p>;
  if (!ready) {
    return (
      <div className="setup">
        <p>
          The job sheet is saved to a new hosted table, <b>EcoSync Daily Records</b>, in your ArcGIS content
          (one row per job per day, with photos and signature as attachments). It only needs creating once.
        </p>
        <button onClick={setup} disabled={!!busy}>{busy || "Create the Daily Records table"}</button>
        {error && <p className="error">{error}</p>}
      </div>
    );
  }
  if (!rec) return error ? <p className="error">{error}</p> : null;

  const set = <K extends keyof DailyRecord>(k: K, v: DailyRecord[K]) => {
    setRec({ ...rec, [k]: v });
    setSaved("");
  };
  const numIn = (k: keyof DailyRecord) => (e: React.ChangeEvent<HTMLInputElement>) =>
    set(k, (e.target.value === "" ? null : Number(e.target.value)) as any);
  /** A reading typed by hand: the lookup will no longer overwrite that side. */
  const reading = (side: "start" | "end", k: keyof DailyRecord) => (e: React.ChangeEvent<HTMLInputElement>) => {
    setFromLookup((f) => ({ ...f, [side]: false }));
    numIn(k)(e);
  };
  const typedDT = (side: "start" | "end") => (e: React.ChangeEvent<HTMLInputElement>) => {
    const v = e.target.value === "" ? null : Number(e.target.value);
    setDtManual((d) => ({ ...d, [side]: v != null }));
    set(side === "start" ? "start_delta_t" : "end_delta_t", v);
  };

  /** Saves the sheet (and any new photos / signature); returns the saved record. */
  const persist = async (status: string): Promise<DailyRecord> => {
    setBusy("Saving…");
    const toSave: DailyRecord = {
      ...rec,
      status,
      start_delta_t: startDT,
      end_delta_t: endDT,
      updated_by: username,
      updated_at: new Date().toISOString(),
    };
    const id = await source.saveDailyRecord(toSave);
    for (const [i, f] of newPhotos.entries()) {
      setBusy(`Uploading photo ${i + 1} of ${newPhotos.length}…`);
      await source.addAttachment(id, await shrinkImage(f, CONFIG.photoMaxPx), jpgName(f.name));
    }
    if (signature) {
      setBusy("Saving signature…");
      await source.deleteAttachments(id, atts.filter((a) => SIGNATURE.test(a.name)).map((a) => a.id));
      await source.addAttachment(id, signature, "signature.png");
    }
    const done = { ...toSave, objectId: id };
    setRec(done);
    setAtts(await source.attachments(id));
    setNewPhotos([]);
    setSignature(null);
    return done;
  };

  const stamp = () => new Date().toLocaleTimeString("en-AU", { timeZone: CONFIG.timeZone, hour: "2-digit", minute: "2-digit" });

  const save = async () => {
    setError(null);
    try {
      // Any change after issuing makes it a draft again until re-issued.
      await persist("draft");
      setSaved(`Saved ${stamp()}`);
    } catch (e: any) {
      setError(e.message ?? String(e));
    } finally {
      setBusy("");
    }
  };

  const issue = async () => {
    if (!onIssue) return;
    setError(null);
    try {
      const saved = await persist("draft");
      const { blob, name, version } = await onIssue(saved, setBusy, { includeWarnings: printWarnings });
      const issued = { ...saved, status: "issued" };
      await source.saveDailyRecord(issued); // status only - photos and signature were saved above
      setRec(issued);
      setAtts(await source.attachments(saved.objectId!));
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = name;
      a.click();
      setSaved(`Issued version ${version} at ${stamp()}`);
    } catch (e: any) {
      setError(e.message ?? String(e));
    } finally {
      setBusy("");
    }
  };

  const removePhoto = async (a: AttachmentInfo) => {
    if (rec.objectId == null || !confirm(`Delete ${a.name}?`)) return;
    try {
      await source.deleteAttachments(rec.objectId, [a.id]);
      setAtts(atts.filter((x) => x.id !== a.id));
    } catch (e: any) {
      setError(e.message ?? String(e));
    }
  };

  const photos = atts.filter((a) => !SIGNATURE.test(a.name) && !PDF.test(a.name));
  const pdfs = atts.filter((a) => PDF.test(a.name));
  const sig = atts.find((a) => SIGNATURE.test(a.name));

  return (
    <form className="jobsheet" onSubmit={(e) => { e.preventDefault(); save(); }}>
      <p className="hint">
        {rec.objectId == null
          ? form1 ? "New job sheet, filled in from today's Form 1 record. Check it, then save." : "New job sheet. Fill it in, then save."
          : `Saved job sheet${rec.updated_by ? ` · last saved by ${rec.updated_by}` : ""}.`}
      </p>

      <fieldset>
        <legend>Report letterhead</legend>
        <div className="letterhead">
          {Object.entries(CONFIG.entities).map(([code, e]) => (
            <label key={code} className={`lh ${rec.entity === code ? "on" : ""}`}>
              <input type="radio" name="entity" checked={rec.entity === code} onChange={() => set("entity", code)} />
              <img src={e.logo} alt={e.name} />
              <span>{e.name}</span>
            </label>
          ))}
        </div>
      </fieldset>

      <fieldset>
        <legend>Operations</legend>
        <div className="grid2">
          <label>Start time <input type="time" value={rec.start_time} onChange={(e) => set("start_time", e.target.value)} /></label>
          <label>End time <input type="time" value={rec.end_time} onChange={(e) => set("end_time", e.target.value)} /></label>
        </div>
        <CheckList title="Operators" options={lists.get("operator") ?? []} value={rec.operators} onChange={(v) => set("operators", v)} />
        <CheckList title="Equipment" options={lists.get("equipment") ?? []} value={rec.equipment} onChange={(v) => set("equipment", v)} />
        <CheckList title="Chemicals" options={lists.get("chemical") ?? []} value={rec.chemicals} onChange={(v) => set("chemicals", v)} />
        <label>
          Total sprayed out for the day (L)
          <input type="number" step="0.1" min="0" value={rec.tank_total_l ?? ""} onChange={numIn("tank_total_l")} />
        </label>
      </fieldset>

      <fieldset>
        <legend>Weather</legend>
        <table className="weather">
          <thead><tr><th /><th>At start</th><th>At finish</th></tr></thead>
          <tbody>
            <tr><th>Air temperature (°C)</th>
              <td><input type="number" step="0.1" value={rec.start_temp_c ?? ""} onChange={reading("start", "start_temp_c")} /></td>
              <td><input type="number" step="0.1" value={rec.end_temp_c ?? ""} onChange={reading("end", "end_temp_c")} /></td></tr>
            <tr><th>Relative humidity (%)</th>
              <td><input type="number" step="1" min="0" max="100" value={rec.start_rh_pct ?? ""} onChange={reading("start", "start_rh_pct")} /></td>
              <td><input type="number" step="1" min="0" max="100" value={rec.end_rh_pct ?? ""} onChange={reading("end", "end_rh_pct")} /></td></tr>
            <tr><th>Wind speed (km/h)</th>
              <td><input type="number" step="0.1" min="0" value={rec.start_wind_kmh ?? ""} onChange={reading("start", "start_wind_kmh")} /></td>
              <td><input type="number" step="0.1" min="0" value={rec.end_wind_kmh ?? ""} onChange={reading("end", "end_wind_kmh")} /></td></tr>
            <tr><th>Wind direction</th>
              <td><WindDir value={rec.start_wind_dir} onChange={(v) => { setFromLookup((f) => ({ ...f, start: false })); set("start_wind_dir", v); }} /></td>
              <td><WindDir value={rec.end_wind_dir} onChange={(v) => { setFromLookup((f) => ({ ...f, end: false })); set("end_wind_dir", v); }} /></td></tr>
            <tr><th>Delta T</th>
              {(["start", "end"] as const).map((side) => (
                <td key={side}>
                  <input type="number" step="0.1" className={dtManual[side] ? "typed" : "calc"}
                    value={(side === "start" ? startDT : endDT) ?? ""} onChange={typedDT(side)} />
                  <div className="dtnote">
                    {dtManual[side]
                      ? <button type="button" className="link" onClick={() => setDtManual((d) => ({ ...d, [side]: false }))}>use calculated</button>
                      : "calculated"}
                  </div>
                </td>
              ))}</tr>
          </tbody>
        </table>
        <div className="lookuprow">
          <button type="button" className="secondary" disabled={!rec.start_time} onClick={() => lookup("start", rec.start_time)}>
            Look up start weather{rec.start_time ? ` (${rec.start_time})` : ""}
          </button>
          <button type="button" className="secondary" disabled={!rec.end_time} onClick={() => lookup("end", rec.end_time)}>
            Look up finish weather{rec.end_time ? ` (${rec.end_time})` : ""}
          </button>
        </div>
        {weatherMsg && <p className="hint">{weatherMsg}</p>}
        <label>
          Source of readings
          <select value={rec.weather_source} onChange={(e) => set("weather_source", e.target.value)}>
            {!["", "Handheld meter", "Weather lookup", "Handheld meter and weather lookup", "Weather lookup (OpenWeather) - check against meter"].includes(rec.weather_source) && (
              <option value={rec.weather_source}>{rec.weather_source}</option>
            )}
            <option value="">–</option>
            <option>Handheld meter</option>
            <option>Weather lookup</option>
            <option>Handheld meter and weather lookup</option>
            <option>Weather lookup (OpenWeather) - check against meter</option>
          </select>
        </label>
        {warnings.length > 0 && (
          <>
            <ul className="warn">{warnings.map((w) => <li key={w}>{w}</li>)}</ul>
            <label className="check">
              <input type="checkbox" checked={printWarnings} onChange={(e) => setPrintWarnings(e.target.checked)} />{" "}
              Print these warnings on the report (the readings and Delta T always print)
            </label>
          </>
        )}
      </fieldset>

      <fieldset>
        <legend>Field observations</legend>
        <textarea rows={4} value={rec.observations} onChange={(e) => set("observations", e.target.value)} />
        <label>
          Flag issues (fence, erosion, access, vandalism…)
          <textarea rows={2} value={rec.flag_issues} onChange={(e) => set("flag_issues", e.target.value)} />
        </label>
      </fieldset>

      <fieldset>
        <legend>Photos</legend>
        <div className="thumbs">
          {photos.map((a) => (
            <figure key={a.id}>
              {rec.objectId != null && a.contentType.startsWith("image/") && (
                <img src={source.attachmentUrl(rec.objectId, a.id)} alt={a.name} />
              )}
              <figcaption>{a.name} <button type="button" className="link" onClick={() => removePhoto(a)}>Delete</button></figcaption>
            </figure>
          ))}
          {newPhotos.map((f, i) => (
            <figure key={`new-${i}`} className="pending">
              <img src={URL.createObjectURL(f)} alt={f.name} />
              <figcaption>{f.name} (not saved) <button type="button" className="link" onClick={() => setNewPhotos(newPhotos.filter((_, j) => j !== i))}>Remove</button></figcaption>
            </figure>
          ))}
        </div>
        <input type="file" accept="image/*" multiple aria-label="Add photos (camera or gallery)" onChange={(e) => { setNewPhotos([...newPhotos, ...Array.from(e.target.files ?? [])]); e.target.value = ""; }} />
      </fieldset>

      <fieldset>
        <legend>Operator signature</legend>
        {sig && rec.objectId != null && !signature && (
          <img className="sig" src={source.attachmentUrl(rec.objectId, sig.id)} alt="Saved signature" />
        )}
        <SignaturePad onChange={setSignature} />
      </fieldset>

      {error && <p className="error">{error}</p>}
      <div className="saverow">
        <button type="submit" disabled={!!busy}>{busy || "Save job sheet"}</button>
        {onIssue && (
          <button type="button" className="secondary" disabled={!!busy} onClick={issue}>
            {pdfs.length ? "Re-issue report (PDF)" : "Issue report (PDF)"}
          </button>
        )}
        {saved && <span className="ok">{saved}</span>}
        {rec.status === "issued" && !saved && <span className="ok">Issued</span>}
      </div>
      {pdfs.length > 0 && rec.objectId != null && (
        <div className="issued">
          Issued reports:{" "}
          {pdfs.map((a, i) => (
            <span key={a.id}>{i > 0 && " · "}<a href={source.attachmentUrl(rec.objectId!, a.id)} target="_blank" rel="noreferrer">{a.name}</a></span>
          ))}
        </div>
      )}
    </form>
  );
}

function CheckList({ title, options, value, onChange }: { title: string; options: string[]; value: string; onChange: (v: string) => void }) {
  const chosen = splitList(value);
  // Keep any saved value that is no longer in the list, so nothing silently disappears.
  const all = [...options, ...chosen.filter((c) => !options.some((o) => o.toLowerCase() === c.toLowerCase()))];
  const isOn = (o: string) => chosen.some((c) => c.toLowerCase() === o.toLowerCase());
  const toggle = (o: string) => onChange(joinList(isOn(o) ? chosen.filter((c) => c.toLowerCase() !== o.toLowerCase()) : [...chosen, o]));
  return (
    <div className="checklist">
      <div className="cl-title">{title}</div>
      {all.length === 0 && <span className="hint">Add values to the EcoSync Lists table.</span>}
      {all.map((o) => (
        <label key={o} className="check">
          <input type="checkbox" checked={isOn(o)} onChange={() => toggle(o)} /> {o}
        </label>
      ))}
    </div>
  );
}

const DIRS = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW", "Calm", "Variable"];

function WindDir({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  return (
    <select value={value} onChange={(e) => onChange(e.target.value)}>
      <option value="">–</option>
      {value && !DIRS.includes(value) && <option value={value}>{value}</option>}
      {DIRS.map((d) => <option key={d}>{d}</option>)}
    </select>
  );
}
