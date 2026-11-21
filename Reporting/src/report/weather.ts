// Spray-weather maths. Pure functions, unit-tested in weather.test.ts.

/** Wet-bulb temperature (°C) from air temperature (°C) and RH (%), Stull (2011). */
export function wetBulb(t: number, rh: number): number {
  return (
    t * Math.atan(0.151977 * Math.sqrt(rh + 8.313659)) +
    Math.atan(t + rh) -
    Math.atan(rh - 1.676331) +
    0.00391838 * Math.pow(rh, 1.5) * Math.atan(0.023101 * rh) -
    4.686035
  );
}

/** Delta T = dry bulb - wet bulb, 1 dp. Same formula as weather.js getDeltaT. */
export function deltaT(t: number | null | undefined, rh: number | null | undefined): number | null {
  if (t == null || rh == null || !Number.isFinite(t) || !Number.isFinite(rh) || rh <= 0 || rh > 100) return null;
  return Math.round((t - wetBulb(t, rh)) * 10) / 10;
}

export interface Limits {
  deltaTLow: number; // below this: inversion / drift risk
  deltaTHigh: number; // above this: outside the working window
  deltaTStop: number; // above this: should not be spraying
  windMinKmh: number;
  windMaxKmh: number;
}

/** Warnings for one set of readings. Empty = within limits (or nothing entered). */
export function weatherWarnings(
  when: string,
  r: { temp: number | null; rh: number | null; wind: number | null },
  limits: Limits,
): string[] {
  const out: string[] = [];
  const dt = deltaT(r.temp, r.rh);
  if (dt != null) {
    if (dt > limits.deltaTStop) out.push(`${when}: Delta T ${dt} is above ${limits.deltaTStop} - conditions where spraying should not go ahead.`);
    else if (dt > limits.deltaTHigh) out.push(`${when}: Delta T ${dt} is above the ${limits.deltaTLow}–${limits.deltaTHigh} working window.`);
    else if (dt < limits.deltaTLow) out.push(`${when}: Delta T ${dt} is below ${limits.deltaTLow} - risk of drift in an inversion.`);
  }
  if (r.wind != null && Number.isFinite(r.wind)) {
    if (r.wind > limits.windMaxKmh) out.push(`${when}: wind ${r.wind} km/h is above ${limits.windMaxKmh} km/h.`);
    else if (r.wind < limits.windMinKmh) out.push(`${when}: wind ${r.wind} km/h is below ${limits.windMinKmh} km/h - possible inversion.`);
  }
  return out;
}
