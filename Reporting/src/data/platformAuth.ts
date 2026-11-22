// Sign-in for the EcoSync Data Platform (Supabase Auth), email + password.
// Plain REST calls, no library, like auth.ts for ArcGIS. The session is kept
// in localStorage so a phone stays signed in; the short-lived access token is
// refreshed with the refresh token when it is about to lapse.

import { CONFIG } from "../config";
import type { Session } from "./types";

const KEY = "ecosync.platform";

interface Stored {
  access: string;
  refresh: string;
  expires: number; // epoch ms
  userId: string;
  email: string;
}

export class PlatformAuthError extends Error {}

const authUrl = (path: string) => `${CONFIG.platform.url}/auth/v1/${path}`;

function headers(token?: string): Record<string, string> {
  if (!CONFIG.platform.publishableKey) {
    throw new PlatformAuthError("The EcoSync database key is not set yet (config.ts, platform.publishableKey).");
  }
  const h: Record<string, string> = { apikey: CONFIG.platform.publishableKey, "Content-Type": "application/json" };
  if (token) h.Authorization = `Bearer ${token}`;
  return h;
}

async function post(path: string, body: unknown, token?: string, method = "POST"): Promise<any> {
  const r = await fetch(authUrl(path), { method, headers: headers(token), body: JSON.stringify(body) });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) {
    const msg = j.error_description || j.msg || j.message || `${r.status} ${r.statusText}`;
    throw new PlatformAuthError(msg === "Invalid login credentials" ? "Wrong email or password." : msg);
  }
  return j;
}

function read(): Stored | null {
  try {
    return JSON.parse(localStorage.getItem(KEY) || "null");
  } catch {
    return null;
  }
}

function save(j: any): Stored {
  const s: Stored = {
    access: j.access_token,
    refresh: j.refresh_token,
    expires: j.expires_at ? j.expires_at * 1000 : Date.now() + Number(j.expires_in ?? 3600) * 1000,
    userId: j.user?.id ?? read()?.userId ?? "",
    email: j.user?.email ?? read()?.email ?? "",
  };
  localStorage.setItem(KEY, JSON.stringify(s));
  return s;
}

const toSession = (s: Stored): Session => ({ token: s.access, username: s.email, expires: s.expires, backend: "platform" });

export async function platformSignIn(email: string, password: string): Promise<Session> {
  return toSession(save(await post("token?grant_type=password", { email: email.trim(), password })));
}

/** The signed-in session, if any (the access token may need refreshing). */
export function platformSession(): Session | null {
  const s = read();
  return s?.refresh ? toSession(s) : null;
}

export function platformUserId(): string {
  return read()?.userId ?? "";
}

let refreshing: Promise<Stored> | null = null;

/** A current access token, refreshed if it lapses within a minute. */
export async function platformToken(): Promise<string> {
  const s = read();
  if (!s?.refresh) throw new PlatformAuthError("Signed out of the EcoSync database - sign in again.");
  if (s.expires > Date.now() + 60_000) return s.access;
  refreshing ??= post("token?grant_type=refresh_token", { refresh_token: s.refresh })
    .then(save)
    .catch((e) => {
      localStorage.removeItem(KEY);
      throw new PlatformAuthError(`Signed out of the EcoSync database (${e.message}) - sign in again.`);
    })
    .finally(() => (refreshing = null));
  return (await refreshing).access;
}

export async function platformSignOut(): Promise<void> {
  const s = read();
  localStorage.removeItem(KEY);
  if (s) await post("logout", {}, s.access).catch(() => undefined);
}

/** Emails a link back to this page for choosing a new password. */
export async function sendPasswordReset(email: string): Promise<void> {
  const to = encodeURIComponent(window.location.origin);
  await post(`recover?redirect_to=${to}`, { email: email.trim() });
}

/**
 * Coming back from a password-reset email, the address ends in
 * #access_token=...&type=recovery. Keeps that session and tidies the address.
 * Returns true if the person now needs to choose a new password.
 */
export function takeRecoveryLink(): boolean {
  const h = new URLSearchParams(window.location.hash.replace(/^#/, ""));
  if (!h.get("access_token") || !h.get("refresh_token")) return false;
  save({ access_token: h.get("access_token"), refresh_token: h.get("refresh_token"), expires_in: h.get("expires_in") });
  history.replaceState(null, "", window.location.pathname + window.location.search);
  return h.get("type") === "recovery";
}

export async function setNewPassword(password: string): Promise<Session> {
  const j = await post("user", { password }, await platformToken(), "PUT");
  const s = read()!;
  s.email = j.email ?? s.email;
  s.userId = j.id ?? s.userId;
  localStorage.setItem(KEY, JSON.stringify(s));
  return toSession(s);
}
