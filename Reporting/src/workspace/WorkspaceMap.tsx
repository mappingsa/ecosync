import { useEffect, useRef, useState } from "react";
import maplibregl from "maplibre-gl";
import "maplibre-gl/dist/maplibre-gl.css";
import { CONFIG } from "../config";
import type { WorkspaceLayer } from "../data/platform";
import type { TargetRow } from "../data/types";
import { colourKey } from "../report/colours";
import { methodOf, speciesOf } from "../report/stats";
import { keyOf, type Item } from "./logic";
import { addrKey, midpoints, parseAddr, toGeometry, vertices, type Addr, type Pt, type Shape } from "./edit";

export interface HandleEvent {
  kind: "vertex" | "mid";
  addr: Addr;
  phase: "down" | "move" | "up";
  pt: Pt;
  moved: boolean;
}

export type Basemap = "imagery" | "streets";
export type Bbox = [number, number, number, number];

interface Props {
  items: Item[];
  colours: Map<string, string>;
  targets: TargetRow[];
  visible: Record<WorkspaceLayer, boolean>;
  basemap: Basemap;
  selected: string | null;
  onSelect: (key: string | null) => void;
  measuring: boolean;
  measureCoords: [number, number][];
  onMeasureClick: (p: [number, number]) => void;
  /** Zoom here whenever n changes. */
  fit: { bbox: Bbox; n: number } | null;
  /** The map, once loaded, for the zoom / locate buttons. */
  onMap?: (map: maplibregl.Map) => void;
  /** Cursor position (null when it leaves the map). */
  onPointer?: (p: [number, number] | null) => void;
  /** Centre latitude and zoom after every move, for the status bar's scale. */
  onView?: (v: { lat: number; zoom: number }) => void;

  /** Drawing a new record: taps add points, a double-tap finishes. */
  drawing?: boolean;
  onDrawClick?: (p: Pt) => void;
  onDrawFinish?: () => void;
  /** The shape being drawn or reshaped. */
  draft?: Shape | null;
  /** Reshaping: the draft's points can be dragged; dragging a midpoint adds one. */
  editing?: boolean;
  onHandle?: (e: HandleEvent) => void;
  selectedVertex?: string | null;
  /** Reference layers (imported files) to draw under the spray records. */
  refs?: RefDraw[];
}

export interface RefDraw {
  id: string; // layer id, or "preview-N" while an import is being checked
  colour: string;
  features: import("../data/types").Feature[];
}

/** Map key of a reference feature: "ref:<layer>:<feature>". */
export const refKey = (layerId: string, f: import("../data/types").Feature, i: number) => `ref:${layerId}:${String(f.properties._id ?? i)}`;

const TRACK_COLOUR = "#00E5FF";
const SELECT_COLOUR = "#FFEB3B";
const SOURCES: WorkspaceLayer[] = ["polygons", "lines", "tracks", "points"];
const CLICKABLE = ["ws-points", "ws-lines", "ws-tracks", "ws-poly-fill", "ref-pt", "ref-line", "ref-fill"];
const LAYER_IDS: Record<WorkspaceLayer, string[]> = {
  polygons: ["ws-poly-fill", "ws-poly-edge"],
  lines: ["ws-lines"],
  tracks: ["ws-tracks"],
  points: ["ws-points"],
};
const empty = { type: "FeatureCollection" as const, features: [] as any[] };
const DRAFT = "#ffffff";
const DRAFT_EDGE = "#00c853";

function style(): maplibregl.StyleSpecification {
  return {
    version: 8,
    sources: {
      imagery: { type: "raster", tiles: [CONFIG.basemap.tiles], tileSize: 256, maxzoom: CONFIG.basemap.maxzoom, attribution: CONFIG.basemap.attribution },
      streets: { type: "raster", tiles: ["https://tile.openstreetmap.org/{z}/{x}/{y}.png"], tileSize: 256, maxzoom: 19, attribution: "© OpenStreetMap contributors" },
    },
    layers: [
      { id: "imagery", type: "raster", source: "imagery" },
      { id: "streets", type: "raster", source: "streets", layout: { visibility: "none" } },
    ],
  };
}

/** The workspace map: every filtered record, coloured by species and method. */
export function WorkspaceMap(p: Props) {
  const el = useRef<HTMLDivElement>(null);
  const mapRef = useRef<maplibregl.Map | null>(null);
  const [ready, setReady] = useState(false);
  const cb = useRef(p);
  cb.current = p; // event handlers always see the latest props

  useEffect(() => {
    if (!el.current) return;
    const map = new maplibregl.Map({ container: el.current, style: style(), center: [137.5, -33.5], zoom: 6, doubleClickZoom: false });
    mapRef.current = map;
    map.addControl(new maplibregl.ScaleControl({ unit: "metric" }), "bottom-right");
    const view = () => cb.current.onView?.({ lat: map.getCenter().lat, zoom: map.getZoom() });
    map.on("moveend", view);
    map.on("mouseout", () => cb.current.onPointer?.(null));
    map.on("load", () => {
      for (const s of SOURCES) map.addSource(`ws-${s}`, { type: "geojson", data: empty });
      map.addSource("ws-measure", { type: "geojson", data: empty });
      // Reference layers, under everything else
      map.addSource("ref", { type: "geojson", data: empty });
      map.addLayer({ id: "ref-fill", type: "fill", source: "ref", filter: ["in", ["geometry-type"], ["literal", ["Polygon", "MultiPolygon"]]], paint: { "fill-color": ["get", "_colour"], "fill-opacity": ["case", ["get", "_preview"], 0.25, 0.12] } });
      map.addLayer({ id: "ref-line", type: "line", source: "ref", filter: ["!=", ["geometry-type"], "Point"], paint: { "line-color": ["get", "_colour"], "line-width": ["case", ["get", "_preview"], 3, 2] } });
      map.addLayer({ id: "ref-pt", type: "circle", source: "ref", filter: ["==", ["geometry-type"], "Point"], paint: { "circle-radius": 5, "circle-color": ["get", "_colour"], "circle-stroke-color": "#fff", "circle-stroke-width": 1.5 } });
      map.addLayer({ id: "ws-poly-fill", type: "fill", source: "ws-polygons", paint: { "fill-color": ["get", "_colour"], "fill-opacity": 0.35 } });
      map.addLayer({ id: "ws-poly-edge", type: "line", source: "ws-polygons", paint: { "line-color": ["get", "_colour"], "line-width": 2 } });
      map.addLayer({ id: "ws-tracks", type: "line", source: "ws-tracks", paint: { "line-color": TRACK_COLOUR, "line-width": 2, "line-dasharray": [2, 2] } });
      map.addLayer({ id: "ws-lines", type: "line", source: "ws-lines", paint: { "line-color": ["get", "_colour"], "line-width": 3 } });
      map.addLayer({
        id: "ws-points", type: "circle", source: "ws-points",
        paint: {
          "circle-color": ["get", "_colour"], "circle-stroke-color": "#000", "circle-stroke-width": 1,
          "circle-radius": ["interpolate", ["linear"], ["zoom"], 8, 2.5, 14, 4.5, 18, 7],
        },
      });
      // The selected record, drawn over everything
      const none = ["==", ["get", "_key"], ""] as any;
      map.addLayer({ id: "sel-poly", type: "line", source: "ws-polygons", filter: none, paint: { "line-color": SELECT_COLOUR, "line-width": 4 } });
      map.addLayer({ id: "sel-line", type: "line", source: "ws-lines", filter: none, paint: { "line-color": SELECT_COLOUR, "line-width": 6 } });
      map.addLayer({ id: "sel-track", type: "line", source: "ws-tracks", filter: none, paint: { "line-color": SELECT_COLOUR, "line-width": 4 } });
      map.addLayer({ id: "sel-point", type: "circle", source: "ws-points", filter: none, paint: { "circle-radius": 10, "circle-color": "rgba(0,0,0,0)", "circle-stroke-color": SELECT_COLOUR, "circle-stroke-width": 3 } });
      map.addLayer({ id: "sel-ref", type: "line", source: "ref", filter: none, paint: { "line-color": SELECT_COLOUR, "line-width": 4 } });
      map.addLayer({ id: "sel-ref-pt", type: "circle", source: "ref", filter: none, paint: { "circle-radius": 10, "circle-color": "rgba(0,0,0,0)", "circle-stroke-color": SELECT_COLOUR, "circle-stroke-width": 3 } });
      // The measuring line
      map.addLayer({ id: "m-fill", type: "fill", source: "ws-measure", filter: ["==", ["geometry-type"], "Polygon"], paint: { "fill-color": "#fff", "fill-opacity": 0.2 } });
      map.addLayer({ id: "m-line", type: "line", source: "ws-measure", filter: ["==", ["geometry-type"], "LineString"], paint: { "line-color": "#fff", "line-width": 2.5 } });
      map.addLayer({ id: "m-pts", type: "circle", source: "ws-measure", filter: ["==", ["geometry-type"], "Point"], paint: { "circle-radius": 4, "circle-color": "#fff", "circle-stroke-color": "#000", "circle-stroke-width": 1 } });
      // The shape being drawn or reshaped, with its handles
      map.addSource("d-shape", { type: "geojson", data: empty });
      map.addSource("d-handles", { type: "geojson", data: empty });
      map.addSource("d-rubber", { type: "geojson", data: empty });
      map.addLayer({ id: "d-fill", type: "fill", source: "d-shape", filter: ["==", ["geometry-type"], "Polygon"], paint: { "fill-color": DRAFT_EDGE, "fill-opacity": 0.2 } });
      map.addLayer({ id: "d-line", type: "line", source: "d-shape", filter: ["!=", ["geometry-type"], "Point"], paint: { "line-color": DRAFT_EDGE, "line-width": 2.5 } });
      map.addLayer({ id: "d-rubber", type: "line", source: "d-rubber", paint: { "line-color": DRAFT_EDGE, "line-width": 1.5, "line-dasharray": [2, 2] } });
      map.addLayer({
        id: "d-mid", type: "circle", source: "d-handles", filter: ["==", ["get", "kind"], "mid"],
        paint: { "circle-radius": 4.5, "circle-color": DRAFT_EDGE, "circle-opacity": 0.55, "circle-stroke-color": DRAFT, "circle-stroke-width": 1 },
      });
      map.addLayer({
        id: "d-vert", type: "circle", source: "d-handles", filter: ["==", ["get", "kind"], "vertex"],
        paint: {
          "circle-radius": ["case", ["get", "sel"], 8, 6],
          "circle-color": ["case", ["get", "sel"], "#ff5252", DRAFT],
          "circle-stroke-color": DRAFT_EDGE, "circle-stroke-width": 2.5,
        },
      });
      setReady(true);
      view();
      cb.current.onMap?.(map);
    });
    // Dragging a handle (mouse or finger)
    let drag: { kind: "vertex" | "mid"; addr: Addr } | null = null;
    let moved = false;
    let last: Pt = [0, 0];
    const begin = (kind: "vertex" | "mid") => (e: any) => {
      if (!cb.current.editing) return;
      const f = e.features?.[0];
      if (!f) return;
      e.preventDefault();
      map.dragPan.disable();
      drag = { kind, addr: parseAddr(String(f.properties.a)) };
      moved = false;
      last = [e.lngLat.lng, e.lngLat.lat];
      cb.current.onHandle?.({ ...drag, phase: "down", pt: last, moved });
    };
    const during = (e: any) => {
      if (!drag) return;
      moved = true;
      last = [e.lngLat.lng, e.lngLat.lat];
      cb.current.onHandle?.({ ...drag, phase: "move", pt: last, moved });
    };
    const end = () => {
      if (!drag) return;
      cb.current.onHandle?.({ ...drag, phase: "up", pt: last, moved });
      drag = null;
      map.dragPan.enable();
    };
    for (const [layer, kind] of [["d-vert", "vertex"], ["d-mid", "mid"]] as const) {
      map.on("mousedown", layer, begin(kind));
      map.on("touchstart", layer, begin(kind));
    }
    map.on("mousemove", during);
    map.on("touchmove", during);
    map.on("mouseup", end);
    map.on("touchend", end);
    map.on("dblclick", (e) => {
      if (!cb.current.drawing) return;
      e.preventDefault();
      cb.current.onDrawFinish?.();
    });

    map.on("click", (e) => {
      const c = cb.current;
      if (c.measuring) return c.onMeasureClick([e.lngLat.lng, e.lngLat.lat]);
      if (c.drawing) return c.onDrawClick?.([e.lngLat.lng, e.lngLat.lat]);
      if (c.editing) return;
      const pad = 6;
      const hits = map.queryRenderedFeatures(
        [[e.point.x - pad, e.point.y - pad], [e.point.x + pad, e.point.y + pad]],
        { layers: CLICKABLE.filter((id) => map.getLayer(id)) },
      );
      c.onSelect(hits.length ? String(hits[0].properties?._key ?? "") || null : null);
    });
    map.on("mousemove", (e) => {
      const c = cb.current;
      c.onPointer?.([e.lngLat.lng, e.lngLat.lat]);
      // rubber band from the last drawn point to the cursor
      const src = map.getSource("d-rubber") as maplibregl.GeoJSONSource | undefined;
      const pts = c.draft?.parts[0]?.[0] ?? [];
      if (src) {
        src.setData(c.drawing && c.draft && c.draft.layer !== "points" && pts.length
          ? { type: "Feature", properties: {}, geometry: { type: "LineString", coordinates: [pts[pts.length - 1], [e.lngLat.lng, e.lngLat.lat], ...(c.draft.layer === "polygons" && pts.length > 1 ? [pts[0]] : [])] } } as any
          : empty);
      }
      if (c.measuring || c.drawing) return void (map.getCanvas().style.cursor = "crosshair");
      if (c.editing) {
        const onHandle = map.queryRenderedFeatures(e.point, { layers: ["d-vert", "d-mid"] }).length > 0;
        return void (map.getCanvas().style.cursor = onHandle ? "move" : "");
      }
      const hit = map.queryRenderedFeatures(e.point, { layers: CLICKABLE.filter((id) => map.getLayer(id)) }).length > 0;
      map.getCanvas().style.cursor = hit ? "pointer" : "";
    });
    return () => {
      map.remove();
      mapRef.current = null;
    };
  }, []);

  // Records
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map) return;
    for (const s of SOURCES) {
      const features = p.items.filter((i) => i.layer === s && i.f.geometry).map((i) => ({
        type: "Feature",
        geometry: i.f.geometry,
        properties: {
          _key: keyOf(i),
          _colour: s === "tracks" ? TRACK_COLOUR
            : p.colours.get(colourKey(speciesOf(i.f.properties), methodOf(i.f.properties, p.targets))) ?? "#888",
        },
      }));
      (map.getSource(`ws-${s}`) as maplibregl.GeoJSONSource).setData({ type: "FeatureCollection", features } as any);
    }
  }, [ready, p.items, p.colours, p.targets]);

  // Layer switches and basemap
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map) return;
    for (const [layer, ids] of Object.entries(LAYER_IDS) as [WorkspaceLayer, string[]][]) {
      for (const id of ids) map.setLayoutProperty(id, "visibility", p.visible[layer] ? "visible" : "none");
    }
    map.setLayoutProperty("imagery", "visibility", p.basemap === "imagery" ? "visible" : "none");
    map.setLayoutProperty("streets", "visibility", p.basemap === "streets" ? "visible" : "none");
  }, [ready, p.visible, p.basemap]);

  // Selection
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map) return;
    const f = ["==", ["get", "_key"], p.selected ?? ""] as any;
    for (const id of ["sel-poly", "sel-line", "sel-track", "sel-point"]) map.setFilter(id, f);
    map.setFilter("sel-ref", ["all", f, ["!=", ["geometry-type"], "Point"]] as any);
    map.setFilter("sel-ref-pt", ["all", f, ["==", ["geometry-type"], "Point"]] as any);
  }, [ready, p.selected]);

  // Measuring line
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map) return;
    const c = p.measureCoords;
    const features: any[] = c.map((pt) => ({ type: "Feature", properties: {}, geometry: { type: "Point", coordinates: pt } }));
    if (c.length >= 2) features.push({ type: "Feature", properties: {}, geometry: { type: "LineString", coordinates: c } });
    if (c.length >= 3) features.push({ type: "Feature", properties: {}, geometry: { type: "Polygon", coordinates: [[...c, c[0]]] } });
    (map.getSource("ws-measure") as maplibregl.GeoJSONSource).setData({ type: "FeatureCollection", features } as any);
  }, [ready, p.measureCoords]);

  // Reference layers
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map) return;
    const features: any[] = [];
    for (const r of p.refs ?? []) {
      r.features.forEach((f, i) => {
        if (f.geometry) features.push({ type: "Feature", geometry: f.geometry, properties: { _key: refKey(r.id, f, i), _colour: r.colour, _preview: r.id.startsWith("preview") } });
      });
    }
    (map.getSource("ref") as maplibregl.GeoJSONSource).setData({ type: "FeatureCollection", features } as any);
  }, [ready, p.refs]);

  // The shape being drawn or reshaped
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map) return;
    const s = p.draft;
    const ok = s && s.parts[0]?.[0]?.length;
    let shape: any = empty;
    if (ok) {
      const ring = s!.parts[0][0];
      // while a line or area has too few points to be one, show what there is
      const g = s!.layer === "polygons" && ring.length < 3 ? { type: "LineString", coordinates: ring }
        : s!.layer === "lines" && ring.length < 2 ? null
        : toGeometry(s!);
      shape = g ? { type: "Feature", properties: {}, geometry: g } : empty;
    }
    (map.getSource("d-shape") as maplibregl.GeoJSONSource).setData(shape);
    const handles: any[] = [];
    if (ok) {
      for (const h of vertices(s!)) {
        handles.push({ type: "Feature", properties: { kind: "vertex", a: addrKey(h.addr), sel: addrKey(h.addr) === p.selectedVertex }, geometry: { type: "Point", coordinates: h.pt } });
      }
      if (p.editing && s!.parts[0][0].length >= 2) {
        for (const h of midpoints(s!)) handles.push({ type: "Feature", properties: { kind: "mid", a: addrKey(h.addr) }, geometry: { type: "Point", coordinates: h.pt } });
      }
    }
    (map.getSource("d-handles") as maplibregl.GeoJSONSource).setData({ type: "FeatureCollection", features: handles } as any);
    if (!p.drawing) (map.getSource("d-rubber") as maplibregl.GeoJSONSource).setData(empty);
  }, [ready, p.draft, p.editing, p.drawing, p.selectedVertex]);

  // Zoom to
  useEffect(() => {
    const map = mapRef.current;
    if (!ready || !map || !p.fit) return;
    const [w, s, e, n] = p.fit.bbox;
    if (w === e && s === n) map.flyTo({ center: [w, s], zoom: Math.max(map.getZoom(), 17) });
    else map.fitBounds(p.fit.bbox, { padding: 60, maxZoom: 18 });
  }, [ready, p.fit?.n]);

  return <div ref={el} className="ws-map" />;
}
