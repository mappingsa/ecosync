import { FALLBACK_COLOURS, TARGETS } from "../config";
import type { TargetRow } from "../data/types";

export const colourKey = (species: string, method: string) => `${species}|${method}`.toLowerCase();

const valid = (c: string | undefined) => (c && /^#[0-9a-f]{6}$/i.test(c.trim()) ? c.trim() : undefined);

/**
 * One colour per species AND control method (Boxthorn sprayed = red, Boxthorn
 * cut and swab = green), used by both the map and the legend. Looks for a
 * Targets row with that species and method, then any row for the species,
 * then the config list, then a fallback palette.
 */
export function colourMap(pairs: { species: string; method: string }[], targets: TargetRow[] = []): Map<string, string> {
  const out = new Map<string, string>();
  let next = 0;
  const lc = (s: string) => s.trim().toLowerCase();
  const sorted = [...pairs].sort((a, b) => colourKey(a.species, a.method).localeCompare(colourKey(b.species, b.method)));
  for (const { species, method } of sorted) {
    const key = colourKey(species, method);
    if (out.has(key)) continue;
    // control_method may list several methods: "Spot Spray; Foliar spray"
    const exact = targets.find((t) => lc(t.species) === lc(species) && t.control_method.split(";").some((m) => lc(m) === lc(method)));
    const anyRow = targets.find((t) => lc(t.species) === lc(species));
    const fromConfig = TARGETS.find((t) => lc(t.species) === lc(species) && lc(t.method) === lc(method))
      ?? TARGETS.find((t) => lc(t.species) === lc(species));
    out.set(key, valid(exact?.colour) ?? valid(anyRow?.colour) ?? fromConfig?.colour ?? FALLBACK_COLOURS[next++ % FALLBACK_COLOURS.length]);
  }
  return out;
}
