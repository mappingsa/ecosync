import { useEffect, useRef } from "react";
import maplibregl from "maplibre-gl";
import "maplibre-gl/dist/maplibre-gl.css";
import type { Feature, TargetRow } from "../data/types";
import { bounds } from "../report/stats";
import { addDayLayers, baseStyle } from "./mapLayers";

interface Props {
  points: Feature[];
  lines: Feature[];
  polygons: Feature[];
  colours: Map<string, string>;
  targets: TargetRow[];
}

// Each map is drawn from the day's own records only - never a shared web map -
// so a neighbouring job can never appear and nothing needs reframing.
export function DayMap({ points, lines, polygons, colours, targets }: Props) {
  const el = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!el.current) return;
    const map = new maplibregl.Map({ container: el.current, style: baseStyle(), center: [140.6, -34.3], zoom: 8 });
    map.on("load", () => {
      addDayLayers(map, { points, lines, polygons }, colours, targets);
      const b = bounds([...points, ...lines, ...polygons]);
      if (b) map.fitBounds(b, { padding: 40, maxZoom: 18, duration: 0 });
    });
    return () => map.remove();
  }, [points, lines, polygons, colours, targets]);

  return <div ref={el} className="map" />;
}
