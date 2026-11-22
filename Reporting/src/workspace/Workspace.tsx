import { useEffect, useMemo, useRef, useState } from "react";
import maplibregl from "maplibre-gl";
import { CONFIG } from "../config";
import type { Domains, EditableLayer, PlatformSource, Role, WorkspaceLayer } from "../data/platform";
import type { Lookups } from "../data/types";
import { colourKey, colourMap } from "../report/colours";
import { bounds, dayWindow, methodOf, speciesOf } from "../report/stats";
import { Icon, type IconName } from "../ui/Icon";
import {
  applyFilters, facets, fmtArea, fmtCoord, fmtLength, fmtScale, jobOf, keyOf, LAYER_NAMES, measure, NO_FILTERS, parseLatLon,
  scaleDenominator, sortRows, toCsv, toGeoJson, toRow, totals,
  type Facet, type Filters, type Item, type SortKey,
} from "./logic";
import { WorkspaceMap, type Basemap, type Bbox, type HandleEvent } from "./WorkspaceMap";
import {
  addrKey, deleteVertex, fromDrawing, fromGeometry, fromLocalInput, insertVertex, isValid, minPoints, moveVertex, parseAddr,
  toEwkt, toLocalInput, type Addr, type Pt, type Shape,
} from "./edit";
import { EditForm, type FormValues } from "./EditForm";

interface FormState {
  mode: "new" | "edit";
  layer: EditableLayer;
  id?: string;
  v: FormValues;
}
const ONE: Record<EditableLayer, string> = { points: "point", lines: "line", polygons: "area" };
const LAST_KEY = "ecosync.lastUsed"; // job / species / method last saved, as defaults for the next new record

interface Props {
  source: PlatformSource;
  lookups: Lookups | null;
}

type PanelTab = "layers" | "filters" | "details";

const LAYERS: WorkspaceLayer[] = ["points", "lines", "polygons", "tracks"];
const LAYER_ICON: Record<WorkspaceLayer, string> = { points: "dot", lines: "line", polygons: "area", tracks: "track" };
const TABLE_PAGE = 200;
const today = () => new Intl.DateTimeFormat("en-CA", { timeZone: CONFIG.timeZone }).format(new Date());
const daysAgo = (n: number) => {
  const d = new Date(`${today()}T12:00:00Z`);
  d.setUTCDate(d.getUTCDate() - n);
  return d.toISOString().slice(0, 10);
};
const when = (ms: number) => (ms ? new Date(ms).toLocaleString("en-AU", { timeZone: CONFIG.timeZone, dateStyle: "short", timeStyle: "short" }) : "–");
const shortDate = (d: string) => new Date(`${d}T12:00:00Z`).toLocaleDateString("en-AU", { day: "numeric", month: "short", year: "numeric", timeZone: "UTC" });
const sizeText = (layer: WorkspaceLayer, v: number | null) => (v == null ? "–" : layer === "polygons" ? fmtArea(v) : fmtLength(v));
const QUICK: [string, number | null][] = [["7 days", 7], ["30 days", 30], ["Year", 365], ["Everything", null]];

function download(name: string, text: string, type: string) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = Object.assign(document.createElement("a"), { href: url, download: name });
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** The field GIS workspace: every record in a date range, filtered, mapped and listed; editors can draw and edit. */
export function Workspace({ source, lookups }: Props) {
  const targets = lookups?.targets ?? [];
  const [from, setFrom] = useState(daysAgo(30));
  const [to, setTo] = useState(today());
  const [items, setItems] = useState<Item[] | null>(null);
  const [loaded, setLoaded] = useState<{ from: string; to: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [toast, setToast] = useState<string | null>(null);
  const [filters, setFilters] = useState<Filters>(NO_FILTERS);
  const [visible, setVisible] = useState<Record<WorkspaceLayer, boolean>>({ points: true, lines: true, polygons: true, tracks: true });
  const [basemap, setBasemap] = useState<Basemap>("imagery");
  const [selected, setSelected] = useState<string | null>(null);
  const [sort, setSort] = useState<{ key: SortKey; asc: boolean }>({ key: "time", asc: false });
  const [limit, setLimit] = useState(TABLE_PAGE);
  const [measuring, setMeasuring] = useState(false);
  const [mpts, setMpts] = useState<[number, number][]>([]);
  const [fit, setFit] = useState<{ bbox: Bbox; n: number } | null>(null);
  const [panel, setPanel] = useState<PanelTab>("layers");
  const [sheetOpen, setSheetOpen] = useState(false); // phones: the panel is a bottom sheet
  const [tableOpen, setTableOpen] = useState(false);
  const [menu, setMenu] = useState(false);
  const [pointer, setPointer] = useState<[number, number] | null>(null);
  const [view, setView] = useState<{ lat: number; zoom: number } | null>(null);
  const [query, setQuery] = useState("");
  const mapRef = useRef<maplibregl.Map | null>(null);
  const pin = useRef<maplibregl.Marker | null>(null);
  const [toastOk, setToastOk] = useState(false);

  // --- editing state ---
  const [role, setRole] = useState<Role | null>(null);
  const [domains, setDomains] = useState<Domains | null>(null);
  const [drawLayer, setDrawLayer] = useState<EditableLayer | null>(null); // set while drawing
  const [reshaping, setReshaping] = useState(false);
  const [draft, setDraft] = useState<Shape | null>(null);
  const [history, setHistory] = useState<Shape[]>([]);
  const [selVertex, setSelVertex] = useState<string | null>(null);
  const [form, setForm] = useState<FormState | null>(null);
  const [saving, setSaving] = useState(false);
  const dragAddr = useRef<Addr | null>(null);
  const canEdit = role === "owner" || role === "editor";

  useEffect(() => {
    source.myRole().then((r) => {
      setRole(r);
      if (r !== "viewer") source.domains().then(setDomains, () => undefined);
    }, () => undefined);
  }, [source]);

  const zoomTo = (b: Bbox | null) => {
    if (b) setFit((f) => ({ bbox: b, n: (f?.n ?? 0) + 1 }));
  };
  const flash = (msg: string, ok = false) => {
    setToast(msg);
    setToastOk(ok);
    setTimeout(() => setToast((t) => (t === msg ? null : t)), ok ? 3000 : 6000);
  };

  async function load(f = from, t = to) {
    if (f > t) return flash("The start date is after the end date.");
    setBusy(true);
    setToast(null);
    try {
      const t0 = dayWindow(f, CONFIG.timeZone)[0];
      const t1 = dayWindow(t, CONFIG.timeZone)[1];
      const got = await Promise.all(LAYERS.map((l) => source.rangeFeatures(l, t0, t1)));
      const all = LAYERS.flatMap((layer, i) => got[i].map((ft): Item => ({ layer, f: ft })));
      setItems(all);
      setLoaded({ from: f, to: t });
      setSelected(null);
      setLimit(TABLE_PAGE);
      if (all.length === 0) flash(`No records between ${shortDate(f)} and ${shortDate(t)}.`);
      zoomTo(bounds(all.map((i) => i.f)));
    } catch (e: any) {
      flash(e.message ?? String(e));
    } finally {
      setBusy(false);
    }
  }

  const quick = (days: number | null) => {
    const f = days == null ? "2015-01-01" : daysAgo(days);
    const t = today();
    setFrom(f);
    setTo(t);
    load(f, t);
  };

  const fac = useMemo(() => facets(items ?? [], targets), [items, targets]);
  const filtered = useMemo(() => applyFilters(items ?? [], filters, targets), [items, filters, targets]);
  const shown = useMemo(() => filtered.filter((i) => visible[i.layer]), [filtered, visible]);
  const onMap = useMemo(() => (reshaping && selected ? shown.filter((i) => keyOf(i) !== selected) : shown), [shown, reshaping, selected]);
  const colours = useMemo(() => {
    const pairs = new Map<string, { species: string; method: string }>();
    for (const i of items ?? []) {
      if (i.layer === "tracks") continue;
      const species = speciesOf(i.f.properties), method = methodOf(i.f.properties, targets);
      pairs.set(colourKey(species, method), { species, method });
    }
    return colourMap([...pairs.values()], targets);
  }, [items, targets]);
  const rows = useMemo(() => sortRows(shown.map((i) => toRow(i, targets)), sort.key, sort.asc), [shown, targets, sort]);
  const byKey = useMemo(() => new Map(shown.map((i) => [keyOf(i), i])), [shown]);
  const tot = useMemo(() => totals(shown), [shown]);
  const counts = useMemo(() => {
    const c: Record<WorkspaceLayer, number> = { points: 0, lines: 0, polygons: 0, tracks: 0 };
    for (const i of filtered) c[i.layer]++;
    return c;
  }, [filtered]);
  const legend = useMemo(() => {
    const m = new Map<string, { species: string; method: string; n: number }>();
    for (const r of rows) {
      if (r.layer === "tracks") continue;
      const k = colourKey(r.species, r.method);
      const e = m.get(k) ?? { species: r.species, method: r.method, n: 0 };
      e.n++;
      m.set(k, e);
    }
    return [...m.entries()].sort((a, b) => a[0].localeCompare(b[0]));
  }, [rows]);
  const nFilters = filters.jobs.length + filters.species.length + filters.methods.length;
  const m = measure(mpts);
  const sel = selected ? byKey.get(selected) ?? null : null;
  const selRow = sel ? toRow(sel, targets) : null;

  const pick = (key: string | null, zoom = false) => {
    setSelected(key);
    const it = key ? byKey.get(key) : null;
    if (key) setPanel("details");
    if (zoom && it) zoomTo(bounds([it.f]));
  };

  function search(e: { preventDefault(): void }) {
    e.preventDefault();
    const q = query.trim();
    if (!q) return;
    const ll = parseLatLon(q);
    if (ll) {
      dropPin(ll, "searched");
      return;
    }
    const job = fac.jobs.map(([j]) => j).find((j) => j.toLowerCase() === q.toLowerCase())
      ?? fac.jobs.map(([j]) => j).find((j) => j.toLowerCase().includes(q.toLowerCase()));
    if (!job) return flash(items ? `No job matching “${q}” in the loaded dates, and it isn't a lat / lon.` : "Load some dates first, or type a lat / lon.");
    setFilters({ ...NO_FILTERS, jobs: [job] });
    zoomTo(bounds((items ?? []).filter((i) => jobOf(i) === job).map((i) => i.f)));
    setPanel("filters");
  }

  function dropPin(ll: [number, number], kind: "searched" | "you") {
    const map = mapRef.current;
    if (!map) return;
    pin.current?.remove();
    const el = document.createElement("div");
    el.className = kind === "you" ? "pin-you" : "pin-search";
    pin.current = new maplibregl.Marker({ element: el }).setLngLat(ll).addTo(map);
    map.flyTo({ center: ll, zoom: Math.max(map.getZoom(), 16) });
  }

  function locate() {
    if (!navigator.geolocation) return flash("This device can't share its location.");
    navigator.geolocation.getCurrentPosition(
      (p) => dropPin([p.coords.longitude, p.coords.latitude], "you"),
      (err) => flash(err.code === 1 ? "Location is turned off for this site. Allow it in the browser to use Locate." : "Couldn't get your location."),
      { enableHighAccuracy: true, timeout: 15000 },
    );
  }

  // --- drawing and editing ---------------------------------------------------------

  const busyEditing = drawLayer != null || reshaping || form != null;
  const pushDraft = (next: Shape | null) => {
    if (draft) setHistory((h) => [...h.slice(-49), draft]);
    setDraft(next);
  };
  const undo = () => {
    if (!history.length) return void (drawLayer && setDraft(null));
    setDraft(history[history.length - 1]);
    setHistory((h) => h.slice(0, -1));
    setSelVertex(null);
  };
  const stopEditing = () => {
    setDrawLayer(null);
    setReshaping(false);
    setDraft(null);
    setHistory([]);
    setSelVertex(null);
    setForm(null);
  };

  function lastUsed(): Partial<FormValues> {
    try { return JSON.parse(localStorage.getItem(LAST_KEY) || "{}"); } catch { return {}; }
  }

  function startDraw(layer: EditableLayer) {
    stopEditing();
    setMeasuring(false);
    setMpts([]);
    setSelected(null);
    setDrawLayer(layer);
  }

  function drawClick(pt: Pt) {
    if (!drawLayer) return;
    const ring = draft?.parts[0][0] ?? [];
    const prev = ring[ring.length - 1];
    if (prev && Math.abs(prev[0] - pt[0]) < 1e-9 && Math.abs(prev[1] - pt[1]) < 1e-9) return; // the 2nd tap of a double-tap
    const next = fromDrawing(drawLayer, [...ring, pt]);
    pushDraft(next);
    if (drawLayer === "points") finishDraw(next);
  }

  function finishDraw(shape = draft) {
    if (!drawLayer) return;
    if (!shape || !isValid(shape)) return flash(`An ${ONE[drawLayer]} needs at least ${minPoints(drawLayer)} points.`);
    const last = lastUsed();
    const has = <T extends { id: string; active: boolean }>(xs: T[] | undefined, id?: string) => !!id && !!xs?.some((x) => x.id === id && x.active);
    setForm({
      mode: "new",
      layer: drawLayer,
      v: {
        // start from what was used last, if it is still on the lists
        job_id: has(domains?.jobs, last.job_id) ? last.job_id! : "",
        species_id: has(domains?.species, last.species_id) ? last.species_id! : "",
        method_id: has(domains?.methods, last.method_id) ? last.method_id! : "",
        when: toLocalInput(Date.now(), CONFIG.timeZone),
        notes: "",
      },
    });
    setDrawLayer(null);
    setPanel("details");
    setSheetOpen(true);
  }

  /** Put a saved record (re-read from the database) into the loaded set and select it. */
  async function refresh(layer: EditableLayer, id: string) {
    const f = await source.featureById(layer, id);
    const key = `${layer}:${id}`;
    setItems((cur) => {
      const rest = (cur ?? []).filter((i) => keyOf(i) !== key);
      return f ? [...rest, { layer, f }] : rest;
    });
    setSelected(f ? key : null);
  }

  async function saveForm() {
    if (!form) return;
    const t = fromLocalInput(form.v.when, CONFIG.timeZone);
    if (Number.isNaN(t)) return flash("Check the date and time.");
    const values = {
      job_id: form.v.job_id || null,
      species_id: form.v.species_id || null,
      method_id: form.v.method_id || null,
      treated_at: new Date(t).toISOString(),
      notes: form.v.notes.trim() || null,
    };
    setSaving(true);
    try {
      let id = form.id;
      if (form.mode === "new") {
        if (!draft || !isValid(draft)) throw new Error("The shape is missing; draw it again.");
        id = await source.createFeature(form.layer, { ...values, geom: toEwkt(draft) });
      } else {
        await source.updateFeature(form.layer, id!, values);
      }
      try {
        localStorage.setItem(LAST_KEY, JSON.stringify({ job_id: form.v.job_id, species_id: form.v.species_id, method_id: form.v.method_id }));
      } catch { /* private mode */ }
      await refresh(form.layer, id!);
      flash(form.mode === "new" ? `New ${ONE[form.layer]} saved.` : "Changes saved.", true);
      stopEditing();
    } catch (e: any) {
      flash(e.message ?? String(e));
    } finally {
      setSaving(false);
    }
  }

  function editDetails() {
    if (!sel || sel.layer === "tracks") return;
    const p = sel.f.properties;
    setForm({
      mode: "edit",
      layer: sel.layer,
      id: String(p.id),
      v: {
        job_id: String(p.job_id ?? ""),
        species_id: String(p.species_id ?? ""),
        method_id: String(p.method_id ?? ""),
        when: toLocalInput(Number(p.treatment_time ?? Date.now()), CONFIG.timeZone),
        notes: String(p.notes ?? ""),
      },
    });
  }

  function editShape() {
    if (!sel || sel.layer === "tracks" || !sel.f.geometry) return;
    setMeasuring(false);
    setDraft(fromGeometry(sel.f.geometry, sel.layer));
    setHistory([]);
    setSelVertex(null);
    setReshaping(true);
  }

  async function saveShape() {
    if (!sel || sel.layer === "tracks" || !draft) return;
    if (!isValid(draft)) return flash("The shape needs more points.");
    setSaving(true);
    try {
      await source.updateFeature(sel.layer, String(sel.f.properties.id), { geom: toEwkt(draft) });
      await refresh(sel.layer, String(sel.f.properties.id));
      flash("Shape saved.", true);
      stopEditing();
    } catch (e: any) {
      flash(e.message ?? String(e));
    } finally {
      setSaving(false);
    }
  }

  async function remove() {
    if (!sel || sel.layer === "tracks") return;
    if (!confirm(`Delete this ${ONE[sel.layer]}? It will no longer appear on the map or in reports.`)) return;
    const key = keyOf(sel);
    try {
      await source.deleteFeature(sel.layer, String(sel.f.properties.id));
      setItems((cur) => (cur ?? []).filter((i) => keyOf(i) !== key));
      setSelected(null);
      flash("Deleted.", true);
    } catch (e: any) {
      flash(e.message ?? String(e));
    }
  }

  function onHandle(e: HandleEvent) {
    if (!draft) return;
    if (e.phase === "down") {
      setHistory((h) => [...h.slice(-49), draft]);
      if (e.kind === "mid") {
        setDraft(insertVertex(draft, e.addr, e.pt));
        dragAddr.current = { ...e.addr, i: e.addr.i + 1 };
      } else {
        dragAddr.current = e.addr;
      }
    } else if (e.phase === "move" && dragAddr.current) {
      setDraft((d) => (d ? moveVertex(d, dragAddr.current!, e.pt) : d));
    } else if (e.phase === "up") {
      if (!e.moved && e.kind === "vertex") {
        setHistory((h) => h.slice(0, -1)); // a tap, not an edit
        const k = addrKey(e.addr);
        setSelVertex((cur) => (cur === k ? null : k));
      } else {
        setSelVertex(null);
      }
      dragAddr.current = null;
    }
  }

  function deletePoint() {
    if (!draft || !selVertex) return;
    const next = deleteVertex(draft, parseAddr(selVertex));
    if (!next) return flash(`A ${ONE[draft.layer]} can't have fewer than ${minPoints(draft.layer)} points.`);
    pushDraft(next);
    setSelVertex(null);
  }

  // Keyboard: Esc cancels, Enter finishes drawing, Delete removes the chosen point
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const t = e.target as HTMLElement;
      if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT")) return;
      if (e.key === "Escape") {
        if (drawLayer || reshaping) stopEditing();
        else if (measuring) { setMeasuring(false); setMpts([]); }
      } else if (e.key === "Enter" && drawLayer) finishDraw();
      else if ((e.key === "Delete" || e.key === "Backspace") && reshaping && selVertex) deletePoint();
      else if (e.key === "z" && (e.ctrlKey || e.metaKey) && (drawLayer || reshaping)) { e.preventDefault(); undo(); }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  });

  const draftSize = draft && draft.layer !== "points" ? measure(draft.parts[0][0]) : null;

  const head = (key: SortKey, label: string) => (
    <th onClick={() => setSort((s) => ({ key, asc: s.key === key ? !s.asc : key !== "time" }))} className="sortable">
      {label}<span className="sort">{sort.key === key ? (sort.asc ? "▲" : "▼") : ""}</span>
    </th>
  );
  const fileBase = loaded ? `ecosync_${loaded.from}_to_${loaded.to}` : "ecosync";
  const tabBtn = (t: PanelTab, icon: IconName, label: string, badge?: number) => (
    <button className={panel === t ? "on" : ""} onClick={() => { setPanel(t); setSheetOpen(true); }}>
      <Icon name={icon} size={17} />{label}{badge ? <span className="badge">{badge}</span> : null}
    </button>
  );

  return (
    <div className={`ws ${tableOpen && items ? "with-table" : ""}`}>
      <aside className={`ws-panel ${sheetOpen ? "open" : ""}`}>
        <button className="sheet-handle" onClick={() => setSheetOpen(!sheetOpen)} aria-label={sheetOpen ? "Close panel" : "Open panel"}><span /></button>
        <form className="ws-search" onSubmit={search}>
          <Icon name="search" size={16} />
          <input value={query} onChange={(e) => setQuery(e.target.value)} placeholder="Search job, or lat, lon" list="ws-jobs" enterKeyHint="search" />
          <datalist id="ws-jobs">{fac.jobs.map(([j]) => <option key={j} value={j} />)}</datalist>
          {query && <button type="button" className="icon-btn sm" onClick={() => setQuery("")} aria-label="Clear search"><Icon name="close" size={14} /></button>}
        </form>
        <nav className="ptabs">
          {tabBtn("layers", "layers", "Layers")}
          {tabBtn("filters", "filter", "Filters", nFilters)}
          {tabBtn("details", "info", "Details")}
        </nav>

        <div className="pbody">
          {panel === "layers" && (
            <>
              <section className="pcard">
                <h3><Icon name="calendar" size={15} />Dates</h3>
                <div className="date-row">
                  <label><span>From</span><input type="date" value={from} onChange={(e) => setFrom(e.target.value)} /></label>
                  <label><span>To</span><input type="date" value={to} onChange={(e) => setTo(e.target.value)} /></label>
                </div>
                <div className="chips">
                  {QUICK.map(([label, d]) => <button key={label} className="chip" onClick={() => quick(d)} disabled={busy}>{label}</button>)}
                </div>
                <button className="primary wide" onClick={() => load()} disabled={busy}>
                  {busy ? <><span className="spinner" />Loading…</> : <><Icon name="refresh" size={16} />Load records</>}
                </button>
                {loaded && items && <p className="muted">{shortDate(loaded.from)} – {shortDate(loaded.to)} · {items.length.toLocaleString()} records</p>}
              </section>

              <section className="pcard">
                <h3><Icon name="layers" size={15} />Layers</h3>
                <ul className="layer-list">
                  {LAYERS.map((l) => (
                    <li key={l}>
                      <label className="toggle-row">
                        <input type="checkbox" checked={visible[l]} onChange={(e) => setVisible({ ...visible, [l]: e.target.checked })} />
                        <span className={`lsym ${LAYER_ICON[l]}`} />
                        <span className="lname">{LAYER_NAMES[l]}</span>
                        <span className="count">{counts[l].toLocaleString()}</span>
                      </label>
                    </li>
                  ))}
                </ul>
                {legend.length > 0 && (
                  <ul className="legend">
                    {legend.map(([k, l]) => (
                      <li key={k}><span className="swatch" style={{ background: colours.get(k) }} /><span className="lname">{l.species}<em>{l.method}</em></span><span className="count">{l.n.toLocaleString()}</span></li>
                    ))}
                  </ul>
                )}
              </section>

              <section className="pcard">
                <h3><Icon name="map" size={15} />Basemap</h3>
                <div className="seg small">
                  <button className={basemap === "imagery" ? "on" : ""} onClick={() => setBasemap("imagery")}>Imagery</button>
                  <button className={basemap === "streets" ? "on" : ""} onClick={() => setBasemap("streets")}>Streets</button>
                </div>
              </section>
            </>
          )}

          {panel === "filters" && (
            <>
              {!items && <p className="empty">Load some dates first; the jobs, species and methods in them appear here.</p>}
              {items && nFilters > 0 && (
                <button className="ghost wide" onClick={() => setFilters(NO_FILTERS)}><Icon name="close" size={14} />Clear {nFilters} filter{nFilters === 1 ? "" : "s"}</button>
              )}
              <FacetBox title="Job" facet={fac.jobs} picked={filters.jobs} onChange={(jobs) => setFilters({ ...filters, jobs })} />
              <FacetBox title="Species" facet={fac.species} picked={filters.species} onChange={(species) => setFilters({ ...filters, species })} />
              <FacetBox title="Method" facet={fac.methods} picked={filters.methods} onChange={(methods) => setFilters({ ...filters, methods })} />
            </>
          )}

          {panel === "details" && (
            <>
              {form && (
                <EditForm
                  title={form.mode === "new" ? `New ${ONE[form.layer]}` : `Edit ${ONE[form.layer]}`}
                  layer={form.layer}
                  values={form.v}
                  domains={domains}
                  saving={saving}
                  onChange={(v) => setForm({ ...form, v })}
                  onSave={saveForm}
                  onCancel={stopEditing}
                />
              )}
              {form ? null : selRow ? (
                <section className="pcard selected">
                  <h3><Icon name="pointer" size={15} />Selected {LAYER_NAMES[selRow.layer].toLowerCase().replace(/s$/, "")}</h3>
                  <dl className="kv">
                    <dt>Job</dt><dd>{selRow.job}</dd>
                    {selRow.layer !== "tracks" && <><dt>Species</dt><dd><span className="swatch" style={{ background: colours.get(colourKey(selRow.species, selRow.method)) }} />{selRow.species}</dd><dt>Method</dt><dd>{selRow.method}</dd></>}
                    <dt>{selRow.layer === "tracks" ? "Started" : "Time"}</dt><dd>{when(selRow.time)}</dd>
                    {selRow.accuracy != null && <><dt>Accuracy</dt><dd>{selRow.accuracy.toFixed(2)} m</dd></>}
                    {selRow.size != null && <><dt>{selRow.layer === "polygons" ? "Area" : "Length"}</dt><dd>{sizeText(selRow.layer, selRow.size)}</dd></>}
                  </dl>
                  {sel?.f.properties.notes ? <p className="notes">{String(sel.f.properties.notes)}</p> : null}
                  <div className="btn-row">
                    <button className="ghost" onClick={() => pick(selected, true)}><Icon name="extent" size={15} />Zoom to</button>
                    <button className="ghost" onClick={() => setSelected(null)} disabled={reshaping}><Icon name="close" size={15} />Clear</button>
                  </div>
                  {canEdit && selRow.layer !== "tracks" && !reshaping && (
                    <div className="btn-row edit-row">
                      <button className="ghost" onClick={editDetails}><Icon name="edit" size={15} />Details</button>
                      <button className="ghost" onClick={editShape}><Icon name="shape" size={15} />Shape</button>
                      <button className="ghost danger" onClick={remove}><Icon name="trash" size={15} />Delete</button>
                    </div>
                  )}
                  {sel?.f.properties.edited_at ? <p className="muted small">Edited here · the ArcGIS import no longer changes it</p> : null}
                </section>
              ) : (
                <p className="empty">{items ? "Tap a record on the map or in the table to see its details." : "Load some dates to start."}</p>
              )}
              {items && (
                <section className="pcard">
                  <h3><Icon name="info" size={15} />Showing</h3>
                  <div className="stats">
                    <div><b>{tot.points.toLocaleString()}</b><span>points</span></div>
                    <div><b>{fmtArea(tot.areaM2)}</b><span>{tot.areas} area{tot.areas === 1 ? "" : "s"}</span></div>
                    <div><b>{fmtLength(tot.lengthM)}</b><span>{tot.lines} line{tot.lines === 1 ? "" : "s"}</span></div>
                    <div><b>{fmtLength(tot.trackM)}</b><span>{tot.tracks} track run{tot.tracks === 1 ? "" : "s"}</span></div>
                  </div>
                </section>
              )}
            </>
          )}
        </div>
      </aside>

      <div className="ws-stage">
        <WorkspaceMap
          items={onMap}
          colours={colours}
          targets={targets}
          visible={visible}
          basemap={basemap}
          selected={selected}
          onSelect={(k) => { if (!busyEditing) pick(k); }}
          measuring={measuring}
          measureCoords={mpts}
          onMeasureClick={(pt) => setMpts((c) => [...c, pt])}
          drawing={drawLayer != null}
          onDrawClick={drawClick}
          onDrawFinish={() => finishDraw()}
          draft={draft}
          editing={reshaping}
          onHandle={onHandle}
          selectedVertex={selVertex}
          fit={fit}
          onMap={(mp) => (mapRef.current = mp)}
          onPointer={setPointer}
          onView={setView}
        />

        <div className="toolbar" role="toolbar" aria-label="Map tools">
          <ToolBtn icon="pointer" label="Select" on={!measuring && !drawLayer && !reshaping} disabled={busyEditing} onClick={() => { setMeasuring(false); setMpts([]); }} />
          <ToolBtn icon="ruler" label="Measure" on={measuring} disabled={busyEditing} onClick={() => { setMeasuring(!measuring); setMpts([]); }} />
          {canEdit && (
            <>
              <span className="tb-sep" />
              <ToolBtn icon="addPoint" label="Point" on={drawLayer === "points"} disabled={reshaping || !!form} onClick={() => (drawLayer === "points" ? stopEditing() : startDraw("points"))} />
              <ToolBtn icon="addLine" label="Line" on={drawLayer === "lines"} disabled={reshaping || !!form} onClick={() => (drawLayer === "lines" ? stopEditing() : startDraw("lines"))} />
              <ToolBtn icon="addArea" label="Area" on={drawLayer === "polygons"} disabled={reshaping || !!form} onClick={() => (drawLayer === "polygons" ? stopEditing() : startDraw("polygons"))} />
            </>
          )}
          <span className="tb-sep" />
          <ToolBtn icon="table" label="Table" on={tableOpen} disabled={!items} onClick={() => setTableOpen(!tableOpen)} />
          <div className="tb-menu">
            <ToolBtn icon="download" label="Download" on={menu} disabled={!items || shown.length === 0} onClick={() => setMenu(!menu)} />
            {menu && (
              <div className="popover" onMouseLeave={() => setMenu(false)}>
                <p>Download the {shown.length.toLocaleString()} records showing</p>
                <button onClick={() => { download(`${fileBase}.csv`, toCsv(shown, targets, CONFIG.timeZone), "text/csv"); setMenu(false); }}>CSV <span>spreadsheet</span></button>
                <button onClick={() => { download(`${fileBase}.geojson`, toGeoJson(shown, targets, CONFIG.timeZone), "application/geo+json"); setMenu(false); }}>GeoJSON <span>QGIS, RustGIS, ArcGIS</span></button>
              </div>
            )}
          </div>
        </div>

        <div className="navctl">
          <button onClick={() => mapRef.current?.zoomIn()} title="Zoom in" aria-label="Zoom in"><Icon name="plus" /></button>
          <button onClick={() => mapRef.current?.zoomOut()} title="Zoom out" aria-label="Zoom out"><Icon name="minus" /></button>
          <button onClick={() => zoomTo(bounds(shown.map((i) => i.f)))} disabled={!shown.length} title="Zoom to what's showing" aria-label="Zoom to what's showing"><Icon name="extent" /></button>
          <button onClick={locate} title="Where am I" aria-label="Where am I"><Icon name="locate" /></button>
        </div>

        {measuring && (
          <div className="float-card measure-card">
            <Icon name="ruler" size={16} />
            {mpts.length < 2
              ? <span>Tap the map to measure</span>
              : <span><b>{fmtLength(m.lengthM)}</b>{m.areaM2 != null && <> · <b>{fmtArea(m.areaM2)}</b></>}</span>}
            <button className="icon-btn sm" disabled={!mpts.length} onClick={() => setMpts(mpts.slice(0, -1))} title="Undo last point" aria-label="Undo last point"><Icon name="undo" size={15} /></button>
            <button className="icon-btn sm" onClick={() => { setMeasuring(false); setMpts([]); }} title="Finish" aria-label="Finish measuring"><Icon name="check" size={15} /></button>
          </div>
        )}

        {drawLayer && (
          <div className="float-card edit-card">
            <Icon name={drawLayer === "points" ? "addPoint" : drawLayer === "lines" ? "addLine" : "addArea"} size={16} />
            <span>
              {drawLayer === "points" ? "Tap the map to place the point"
                : !draft ? `Tap the map to start the ${ONE[drawLayer]}`
                : <>{draft.parts[0][0].length} point{draft.parts[0][0].length === 1 ? "" : "s"}
                    {draftSize && draftSize.lengthM > 0 && drawLayer === "lines" && <> · <b>{fmtLength(draftSize.lengthM)}</b></>}
                    {draftSize?.areaM2 != null && drawLayer === "polygons" && <> · <b>{fmtArea(draftSize.areaM2)}</b></>}
                    <span className="hint-inline"> · double-tap to finish</span></>}
            </span>
            {drawLayer !== "points" && <button className="icon-btn sm" disabled={!draft} onClick={undo} title="Undo (Ctrl+Z)" aria-label="Undo"><Icon name="undo" size={15} /></button>}
            {drawLayer !== "points" && <button className="pill primary" disabled={!draft || !isValid(draft)} onClick={() => finishDraw()}>Finish</button>}
            <button className="icon-btn sm" onClick={stopEditing} title="Cancel (Esc)" aria-label="Cancel drawing"><Icon name="close" size={15} /></button>
          </div>
        )}

        {reshaping && draft && (
          <div className="float-card edit-card">
            <Icon name="shape" size={16} />
            <span>{draft.layer === "points" ? "Drag the point to move it" : selVertex ? "Point selected" : "Drag points · drag a small dot to add one · tap a point to select it"}</span>
            {selVertex && <button className="pill danger" onClick={deletePoint}><Icon name="trash" size={14} />Delete point</button>}
            <button className="icon-btn sm" disabled={!history.length} onClick={undo} title="Undo (Ctrl+Z)" aria-label="Undo"><Icon name="undo" size={15} /></button>
            <button className="pill primary" disabled={saving || !isValid(draft)} onClick={saveShape}>{saving ? <span className="spinner" /> : null}Save shape</button>
            <button className="icon-btn sm" onClick={stopEditing} title="Cancel (Esc)" aria-label="Cancel reshaping"><Icon name="close" size={15} /></button>
          </div>
        )}

        {busy && <div className="float-card loading"><span className="spinner" />Loading records…</div>}
        {toast && !busy && <div className={`float-card toast ${toastOk ? "good" : ""}`} onClick={() => setToast(null)}>{toastOk && <Icon name="check" size={16} />}{toast}</div>}

        {!items && !busy && !busyEditing && (
          <div className="welcome">
            <h2>Your field records</h2>
            <p>Spray points, areas and tracks from EcoSyncField. Choose how far back to look.</p>
            <div className="chips">
              {QUICK.map(([label, d]) => <button key={label} className="chip lg" onClick={() => quick(d)}>{label}</button>)}
            </div>
          </div>
        )}

        {items && tableOpen && (
          <div className="drawer">
            <div className="drawer-head">
              <Icon name="table" size={15} />
              <b>Attribute table</b>
              <span className="muted">{rows.length.toLocaleString()} records</span>
              <button className="icon-btn sm" onClick={() => setTableOpen(false)} aria-label="Close table"><Icon name="close" size={15} /></button>
            </div>
            <div className="drawer-body">
              <table className="grid">
                <thead>
                  <tr>{head("layer", "Layer")}{head("job", "Job")}{head("species", "Species")}{head("method", "Method")}{head("time", "Time")}{head("accuracy", "Accuracy")}{head("size", "Length / area")}</tr>
                </thead>
                <tbody>
                  {rows.slice(0, limit).map((r) => (
                    <tr key={r.key} className={r.key === selected ? "sel" : ""} onClick={() => pick(r.key, true)}>
                      <td>{LAYER_NAMES[r.layer]}</td>
                      <td>{r.job}</td>
                      <td>{r.layer !== "tracks" && <span className="swatch" style={{ background: colours.get(colourKey(r.species, r.method)) }} />}{r.species}</td>
                      <td>{r.method}</td>
                      <td>{when(r.time)}</td>
                      <td className="num">{r.accuracy == null ? "–" : `${r.accuracy.toFixed(2)} m`}</td>
                      <td className="num">{sizeText(r.layer, r.size)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
              {rows.length > limit && (
                <button className="ghost wide" onClick={() => setLimit(limit + TABLE_PAGE * 5)}>
                  Showing {limit.toLocaleString()} of {rows.length.toLocaleString()} · show more
                </button>
              )}
            </div>
          </div>
        )}
      </div>

      <footer className="statusbar">
        <span className="mono">{pointer ? fmtCoord(pointer) : "–"}</span>
        <span>{view ? `Zoom ${view.zoom.toFixed(1)}` : ""}</span>
        <span>{view ? fmtScale(scaleDenominator(view.lat, view.zoom)) : ""}</span>
        <span className="grow" />
        <span>{items ? `${shown.length.toLocaleString()} of ${items.length.toLocaleString()} records showing` : "No records loaded"}</span>
        <span>{selected ? "1 selected" : ""}</span>
        <span className="src"><i />EcoSync database · Sydney</span>
      </footer>
    </div>
  );
}

function ToolBtn({ icon, label, on, disabled, onClick }: { icon: IconName; label: string; on?: boolean; disabled?: boolean; onClick: () => void }) {
  return (
    <button className={`tool ${on ? "on" : ""}`} onClick={onClick} disabled={disabled} title={label} aria-label={label} aria-pressed={on}>
      <Icon name={icon} size={19} />
      <span>{label}</span>
    </button>
  );
}

function FacetBox({ title, facet, picked, onChange }: { title: string; facet: Facet; picked: string[]; onChange: (v: string[]) => void }) {
  const [q, setQ] = useState("");
  if (facet.length === 0) return null;
  const set = new Set(picked);
  const toggle = (v: string) => onChange(set.has(v) ? picked.filter((x) => x !== v) : [...picked, v]);
  const list = q ? facet.filter(([v]) => v.toLowerCase().includes(q.toLowerCase())) : facet;
  return (
    <section className="pcard">
      <h3>
        {title}
        <span className="h-note">{picked.length ? `${picked.length} of ${facet.length}` : "all"}</span>
        {picked.length > 0 && <button className="link small" onClick={() => onChange([])}>clear</button>}
      </h3>
      {facet.length > 8 && <input className="facet-search" value={q} onChange={(e) => setQ(e.target.value)} placeholder={`Find ${title.toLowerCase()}`} />}
      <ul className="facet">
        {list.map(([v, n]) => (
          <li key={v}>
            <label className="toggle-row">
              <input type="checkbox" checked={set.has(v)} onChange={() => toggle(v)} />
              <span className="lname">{v}</span>
              <span className="count">{n.toLocaleString()}</span>
            </label>
          </li>
        ))}
      </ul>
    </section>
  );
}
