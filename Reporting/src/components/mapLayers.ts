import maplibregl from "maplibre-gl";
import { CONFIG } from "../config";
import type { Feature, TargetRow } from "../data/types";
import { methodOf, speciesOf } from "../report/stats";
import { colourKey } from "../report/colours";

export function baseStyle(): maplibregl.StyleSpecification {
  return {
    version: 8,
    sources: {
      imagery: { type: "raster", tiles: [CONFIG.basemap.tiles], tileSize: 256, maxzoom: CONFIG.basemap.maxzoom, attribution: CONFIG.basemap.attribution },
    },
    layers: [{ id: "imagery", type: "raster", source: "imagery" }],
  };
}

/** The day's records on top of the imagery, each coloured by species + method. */
export function addDayLayers(
  map: maplibregl.Map,
  d: { points: Feature[]; lines: Feature[]; polygons: Feature[] },
  colours: Map<string, string>,
  targets: TargetRow[],
) {
  const tag = (fs: Feature[]) => ({
    type: "FeatureCollection" as const,
    features: fs.filter((f) => f.geometry).map((f) => ({
      ...f,
      properties: { ...f.properties, _colour: colours.get(colourKey(speciesOf(f.properties), methodOf(f.properties, targets))) ?? "#888" },
    })),
  });
  map.addSource("polygons", { type: "geojson", data: tag(d.polygons) as any });
  map.addSource("lines", { type: "geojson", data: tag(d.lines) as any });
  map.addSource("points", { type: "geojson", data: tag(d.points) as any });
  map.addLayer({ id: "poly-fill", type: "fill", source: "polygons", paint: { "fill-color": ["get", "_colour"], "fill-opacity": 0.35 } });
  map.addLayer({ id: "poly-edge", type: "line", source: "polygons", paint: { "line-color": ["get", "_colour"], "line-width": 2 } });
  map.addLayer({ id: "lines", type: "line", source: "lines", paint: { "line-color": ["get", "_colour"], "line-width": 3 } });
  map.addLayer({
    id: "points", type: "circle", source: "points",
    paint: { "circle-color": ["get", "_colour"], "circle-radius": 5, "circle-stroke-color": "#000", "circle-stroke-width": 1 },
  });
}

/**
 * Draws a map off-screen and returns it as PNG bytes for the PDF - 1200 x 560
 * px, the 600 x 280 shape the old report maps used. Falls back to no imagery if
 * the imagery tiles cannot be copied into an image.
 */
export async function renderMapPng(
  d: { points: Feature[]; lines: Feature[]; polygons: Feature[] },
  colours: Map<string, string>,
  targets: TargetRow[],
  bbox: [number, number, number, number],
  withImagery = true,
): Promise<Uint8Array> {
  const div = document.createElement("div");
  div.style.cssText = "position:absolute;left:-10000px;top:0;width:1200px;height:560px;";
  document.body.appendChild(div);
  const style = baseStyle();
  if (!withImagery) style.layers = [{ id: "bg", type: "background", paint: { "background-color": "#e9ece8" } }];
  const map = new maplibregl.Map({
    container: div,
    style,
    interactive: false,
    attributionControl: false,
    pixelRatio: 1,
    canvasContextAttributes: { preserveDrawingBuffer: true },
    preserveDrawingBuffer: true, // older MapLibre name for the same setting
    fadeDuration: 0,
  } as any);
  try {
    await new Promise<void>((ok) => map.once("load", () => ok()));
    addDayLayers(map, d, colours, targets);
    map.fitBounds(bbox, { padding: 50, maxZoom: 18, duration: 0 });
    await new Promise<void>((ok) => map.once("idle", () => ok()));
    const blob = await new Promise<Blob | null>((ok) => map.getCanvas().toBlob(ok, "image/png"));
    if (!blob) throw new Error("empty map image");
    return new Uint8Array(await blob.arrayBuffer());
  } catch (e) {
    if (withImagery) return renderMapPng(d, colours, targets, bbox, false);
    throw e;
  } finally {
    map.remove();
    div.remove();
  }
}
