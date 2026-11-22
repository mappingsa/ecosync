import { useEffect, useMemo, useState } from "react";
import { CONFIG } from "./config";
import { currentSession, signIn, signOut } from "./data/auth";
import { ArcGISSource } from "./data/arcgis";
import { PlatformSource } from "./data/platform";
import { platformSession, platformSignOut, platformToken, platformUserId, takeRecoveryLink } from "./data/platformAuth";
import type { Attributes, Backend, DataSource, Feature, Job, Lookups, Session } from "./data/types";
import { accuracy, attr, bounds, canonicalJob, dayWindow, forJob, jobsIn, normJob, sameJob, summarise } from "./report/stats";
import { colourKey, colourMap } from "./report/colours";
import { DayMap } from "./components/DayMap";
import { JobSheet } from "./components/JobSheet";
import { issueReport } from "./components/issueReport";
import { NewPassword, SignIn } from "./components/SignIn";

const BACKEND_KEY = "ecosync.backend"; // the data source chosen last on this device
const BACKEND_NAME: Record<Backend, string> = { arcgis: "ArcGIS", platform: "EcoSync database" };

interface Day {
  date: string;
  points: Feature[];
  lines: Feature[];
  polygons: Feature[];
  sheets: Attributes[];
}

const todayLocal = () => new Intl.DateTimeFormat("en-CA", { timeZone: CONFIG.timeZone }).format(new Date());
const fmt = (v: number | null | undefined, dp = 1) => (v == null ? "–" : v.toFixed(dp));

export default function App() {
  const [session, setSession] = useState<Session | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [date, setDate] = useState(todayLocal());
  const [day, setDay] = useState<Day | null>(null);
  const [job, setJob] = useState("");
  const [busy, setBusy] = useState(false);
  const [lookups, setLookups] = useState<Lookups | null>(null);
  const [needPassword, setNeedPassword] = useState(false);
  const backend: Backend = session?.backend ?? "arcgis";
  // One data source per sign-in, so it can remember table locations between screens.
  const source = useMemo<DataSource | null>(() => {
    if (!session) return null;
    return session.backend === "platform" ? new PlatformSource(platformToken, platformUserId()) : new ArcGISSource(session.token);
  }, [session]);

  useEffect(() => {
    if (takeRecoveryLink()) {
      setNeedPassword(true); // came from a password-reset email
      return;
    }
    const pref = localStorage.getItem(BACKEND_KEY);
    currentSession().then(
      (arc) => setSession(pref === "platform" ? platformSession() ?? arc : arc),
      (e) => {
        setError(String(e.message ?? e));
        if (pref === "platform") setSession(platformSession());
      },
    );
  }, []);

  const choose = (s: Session) => {
    localStorage.setItem(BACKEND_KEY, s.backend ?? "arcgis");
    setLookups(null);
    setDay(null);
    setError(null);
    setSession(s);
  };

  // Jobs, Targets, Clients and Lists: read once per sign-in, and again on "Reload tables".
  const loadLookups = () => {
    if (!source) return;
    source.lookups().then(setLookups, (e) => setError(`Lookup tables: ${e.message ?? e}`));
  };
  useEffect(loadLookups, [source]);

  async function load() {
    if (!source) return;
    setBusy(true);
    setError(null);
    try {
      const src = source;
      const [t0, t1] = dayWindow(date, CONFIG.timeZone);
      const [points, lines, polygons, sheets] = await Promise.all([
        src.dayFeatures("points", t0, t1),
        src.dayFeatures("lines", t0, t1),
        src.dayFeatures("polygons", t0, t1),
        src.jobSheets(t0, t1),
      ]);
      setDay({ date, points, lines, polygons, sheets });
      const { jobs } = jobsIn([...points, ...lines, ...polygons], lookups?.jobs);
      setJob(jobs[0] ?? "");
    } catch (e: any) {
      setError(e.message ?? String(e));
    } finally {
      setBusy(false);
    }
  }

  const all = useMemo(() => (day ? [...day.points, ...day.lines, ...day.polygons] : []), [day]);
  const { jobs, blank } = useMemo(() => jobsIn(all, lookups?.jobs), [all, lookups]);

  const view = useMemo(() => {
    if (!day || !job) return null;
    const known = lookups?.jobs ?? [];
    const points = forJob(day.points, job, known);
    const lines = forJob(day.lines, job, known);
    const polygons = forJob(day.polygons, job, known);
    const targets = lookups?.targets ?? [];
    const rows = summarise(points, lines, polygons, targets);
    const colours = colourMap(rows, targets);
    const jobRow: Job | null = known.find((j) => normJob(j.job_number) === normJob(canonicalJob(job, known))) ?? null;
    const acc = accuracy([...points, ...polygons], CONFIG.accuracyThresholdM);
    const sheet = day.sheets.find((s) => sameJob(attr(s, "job_number"), job, known)) ?? null;
    const areaHa = rows.reduce((s, r) => s + r.areaM2, 0) / 10000;
    const entity = jobRow ? CONFIG.entities[jobRow.entity.trim().toUpperCase()] ?? null : null;
    const logo = logoFor(jobRow);
    const client = jobRow ? lookups?.clients.get(jobRow.client_code.trim().toUpperCase()) || jobRow.client_code : "";
    return { points, lines, polygons, rows, colours, acc, sheet, areaHa, jobRow, entity, client, targets, logo };
  }, [day, job, lookups]);

  if (needPassword) {
    return <NewPassword onDone={(s) => { setNeedPassword(false); choose(s); }} />;
  }

  if (!session) {
    return (
      <SignIn
        error={error}
        onArcGIS={() => {
          localStorage.setItem(BACKEND_KEY, "arcgis");
          signIn().catch((e) => setError(String(e)));
        }}
        onPlatform={choose}
      />
    );
  }

  const signOutNow = () => {
    if (backend === "platform") platformSignOut();
    else signOut();
    setSession(null);
    setLookups(null);
    setDay(null);
  };

  return (
    <main>
      <header>
        <h1>EcoSync Reporting</h1>
        <span className="who">
          <span className={`backend ${backend}`}>{BACKEND_NAME[backend]}</span>{" "}
          {session.username} ·{" "}
          <button className="link" onClick={signOutNow}>Sign out</button>
        </span>
      </header>

      <section className="controls">
        <label>
          Day <input type="date" value={date} onChange={(e) => setDate(e.target.value)} />
        </label>
        <button onClick={load} disabled={busy}>{busy ? "Loading…" : "Load day"}</button>
        {day && (
          <label>
            Job{" "}
            <select value={job} onChange={(e) => setJob(e.target.value)}>
              {jobs.length === 0 && <option value="">No records this day</option>}
              {jobs.map((j) => <option key={j} value={j}>{j}</option>)}
            </select>
          </label>
        )}
      </section>

      {error && <p className="error">{error}</p>}

      {lookups && lookups.missing.length > 0 && (
        <p className="warn">
          Not found in {BACKEND_NAME[backend]}: {lookups.missing.join(", ")}. Using built-in defaults until they are published.{" "}
          <button className="link" onClick={loadLookups}>Reload tables</button>
        </p>
      )}

      {day && blank > 0 && (
        <p className="warn">
          {blank} record{blank === 1 ? "" : "s"} on {day.date} have no job number and are not on any report.
        </p>
      )}

      {day && view && (
        <>
          <JobPanel job={job} row={view.jobRow} lookups={lookups} onReload={loadLookups} />
          <section className="cards">
            <Card label="Points" value={String(view.points.length)} />
            <Card label="Lines" value={String(view.lines.length)} />
            <Card label="Polygons" value={String(view.polygons.length)} />
            <Card label="Area treated (polygons)" value={`${fmt(view.areaHa, 2)} ha`} />
            <Card label="Average accuracy" value={`${fmt(view.acc.average)} m`} />
            <Card label="Worst recorded" value={`${fmt(view.acc.worst)} m`} />
            <Card label={`Within ${CONFIG.accuracyThresholdM} m`} value={view.acc.withinPct == null ? "–" : `${view.acc.withinPct} %`} />
          </section>

          <DayMap points={view.points} lines={view.lines} polygons={view.polygons} colours={view.colours} targets={lookups?.targets ?? []} />

          <h2>Treatments</h2>
          <table>
            <thead>
              <tr><th>Target</th><th>Method</th><th>Points</th><th>Lines</th><th>Length (m)</th><th>Polygons</th><th>Area (ha)</th></tr>
            </thead>
            <tbody>
              {view.rows.map((r) => (
                <tr key={r.species + r.method}>
                  <td><span className="swatch" style={{ background: view.colours.get(colourKey(r.species, r.method)) }} />{r.species}</td>
                  <td>{r.method}</td>
                  <td>{r.points}</td>
                  <td>{r.lines}</td>
                  <td>{r.lengthM ? r.lengthM.toFixed(0) : "–"}</td>
                  <td>{r.polygons}</td>
                  <td>{r.areaM2 ? (r.areaM2 / 10000).toFixed(3) : "–"}</td>
                </tr>
              ))}
            </tbody>
          </table>

          <h2>Job sheet</h2>
          <JobSheet
            key={`${job}|${day.date}`}
            source={source!}
            username={session.username}
            job={view.jobRow?.job_number ?? job}
            date={day.date}
            jobRow={view.jobRow}
            lists={lookups?.lists ?? new Map()}
            form1={view.sheet}
            location={(() => {
              const b = bounds([...view.points, ...view.lines, ...view.polygons]);
              return b ? [(b[0] + b[2]) / 2, (b[1] + b[3]) / 2] : null;
            })()}
            onIssue={(rec, progress, options) => {
              // The letterhead chosen on the job sheet wins over the job's default.
              const code = (rec.entity || view.jobRow?.entity || "").trim().toUpperCase();
              const ent = CONFIG.entities[code] ?? view.entity;
              const sameAsJob = code === (view.jobRow?.entity ?? "").trim().toUpperCase();
              return issueReport(
                {
                  source: source!,
                  entityName: ent?.name ?? code,
                  brand: ent?.brand,
                  logoUrl: sameAsJob ? view.logo : ent?.logo ?? null,
                  jobNumber: view.jobRow?.job_number ?? job,
                  client: view.client,
                  address: view.jobRow?.property_address ?? "",
                  ownerContact: view.jobRow?.owner_contact ?? "",
                  reportTitle: view.jobRow?.report_title ?? "",
                  features: { points: view.points, lines: view.lines, polygons: view.polygons },
                  rows: view.rows,
                  areaHa: view.areaHa,
                  accuracy: view.acc,
                  colours: view.colours,
                  targets: view.targets,
                },
                rec,
                session.username,
                progress,
                options,
              );
            }}
          />

          {backend === "arcgis" && <details className="form1">
            <summary>Survey123 Form 1 record (read only)</summary>
            {view.sheet ? <SheetTable sheet={view.sheet} /> : (
              <p className="warn">
                No Form 1 record for “{job}” on {day.date}.
                {day.sheets.length > 0 && <> That day's Form 1 job numbers: {day.sheets.map((s) => `[${attr(s, "job_number") ?? ""}]`).join(", ")}</>}
              </p>
            )}
          </details>}
        </>
      )}
    </main>
  );
}

function JobPanel({ job, row, lookups, onReload }: { job: string; row: Job | null; lookups: Lookups | null; onReload: () => void }) {
  if (!lookups) return null;
  if (!row) {
    return (
      <p className="warn">
        “{job}” is not in the EcoSync Jobs table, so the report has no client, letterhead or address for it.
        Add a row with this job number exactly, then <button className="link" onClick={onReload}>reload tables</button>.
      </p>
    );
  }
  const entity = CONFIG.entities[row.entity.toUpperCase()];
  const logo = logoFor(row);
  const client = lookups.clients.get(row.client_code.toUpperCase()) || row.client_code || "–";
  return (
    <section className="jobpanel">
      {logo ? <img src={logo} alt={entity?.name ?? row.entity} className="logo" /> : <span className="warn">Entity “{row.entity}” has no letterhead set</span>}
      <dl>
        <dt>Job</dt><dd>{row.job_number}{row.report_title && ` · ${row.report_title}`}</dd>
        <dt>Client</dt><dd>{client}</dd>
        <dt>Property</dt><dd>{row.property_address || "–"}</dd>
        <dt>Owner / contact</dt><dd>{row.owner_contact || "–"}</dd>
        <dt>Report type</dt><dd>{row.report_type || "Spray record"}</dd>
      </dl>
    </section>
  );
}

/** Letterhead logo: the job's own logo if it has one, else its entity's (MSA / FPE). */
function logoFor(row: Job | null): string | null {
  if (!row) return null;
  const own = row.logo.trim();
  if (own) return /^https?:\/\//i.test(own) ? own : `/logos/${own}`;
  return CONFIG.entities[row.entity.trim().toUpperCase()]?.logo ?? null;
}

function Card({ label, value }: { label: string; value: string }) {
  return (
    <div className="card">
      <div className="value">{value}</div>
      <div className="label">{label}</div>
    </div>
  );
}

const SYSTEM = /^(objectid|globalid|creationdate|creator|editdate|editor|shape__.*)$/i;
const looksLikeDate = (k: string, v: unknown) => typeof v === "number" && v > 1e12 && /date|time/i.test(k);

function SheetTable({ sheet }: { sheet: Attributes }) {
  const rows = Object.entries(sheet).filter(([k, v]) => !SYSTEM.test(k) && v !== null && v !== "");
  return (
    <table className="sheet">
      <tbody>
        {rows.map(([k, v]) => (
          <tr key={k}>
            <th>{k}</th>
            <td>{looksLikeDate(k, v) ? new Date(v as number).toLocaleString("en-AU", { timeZone: CONFIG.timeZone }) : String(v)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
