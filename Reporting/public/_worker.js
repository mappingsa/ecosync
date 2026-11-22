// Cloudflare Pages "advanced mode" worker: serves the app, plus one small
// endpoint, /api/weather, that looks up past weather from OpenWeather's One Call
// 3.0 "timemachine". The API key lives only in Cloudflare (Settings >
// Variables and Secrets > OWM_API_KEY), never in the app.

const ALLOWED_ORIGINS = ["https://ecosync-reporting.pages.dev", "https://localhost:5173"];

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/api/weather") return weather(request, url, env);
    return env.ASSETS.fetch(request);
  },
};

async function weather(request, url, env) {
  const origin = request.headers.get("Origin") || "";
  const cors = ALLOWED_ORIGINS.includes(origin) ? { "Access-Control-Allow-Origin": origin, Vary: "Origin" } : {};
  const reply = (status, body) =>
    new Response(JSON.stringify(body), {
      status,
      headers: {
        "Content-Type": "application/json",
        "Cache-Control": status === 200 ? "public, max-age=86400" : "no-store",
        ...cors,
      },
    });

  if (!env.OWM_API_KEY) return reply(500, { error: "Weather lookup is not set up yet: add OWM_API_KEY in Cloudflare." });

  const lat = Number(url.searchParams.get("lat"));
  const lon = Number(url.searchParams.get("lon"));
  const dt = Number(url.searchParams.get("dt")); // unix seconds
  // Only Australian locations and the last 60 days, so the key cannot be used for anything else.
  if (!(lat >= -45 && lat <= -9 && lon >= 112 && lon <= 155)) return reply(400, { error: "Location is outside Australia." });
  const now = Date.now() / 1000;
  if (!(dt > now - 60 * 86400 && dt < now + 3600)) return reply(400, { error: "Time must be within the last 60 days." });

  const q =
    "https://api.openweathermap.org/data/3.0/onecall/timemachine" +
    `?lat=${lat.toFixed(4)}&lon=${lon.toFixed(4)}&dt=${Math.round(dt)}&units=metric&appid=${env.OWM_API_KEY}`;
  const r = await fetch(q);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) return reply(502, { error: `OpenWeather: ${j.message || r.status}` });
  const d = (j.data || [])[0];
  if (!d) return reply(502, { error: "OpenWeather returned no data for that time." });
  return reply(200, {
    temp_c: d.temp,
    rh_pct: d.humidity,
    wind_kmh: d.wind_speed * 3.6, // metric units give m/s
    wind_deg: d.wind_deg,
    at: d.dt,
    source: "OpenWeather",
  });
}
