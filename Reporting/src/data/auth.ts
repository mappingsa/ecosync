// ArcGIS Online sign-in, OAuth 2.0 authorization code with PKCE - the flow
// meant for browser apps: no client secret, and the token never appears in the
// address bar. Written against the plain REST endpoints rather than an Esri
// library, so it can be replaced wholesale when sign-in moves elsewhere.

import { CONFIG } from "../config";
import type { Session } from "./types";

const SESSION_KEY = "ecosync.session";
const VERIFIER_KEY = "ecosync.pkce";

const redirectUri = () => window.location.origin; // must match a registered redirect URL exactly
const oauth = (path: string) => `${CONFIG.portal}/sharing/rest/oauth2/${path}`;

function base64url(bytes: Uint8Array): string {
  let s = "";
  bytes.forEach((b) => (s += String.fromCharCode(b)));
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export async function signIn(): Promise<void> {
  const verifier = base64url(crypto.getRandomValues(new Uint8Array(32)));
  const challenge = base64url(
    new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier))),
  );
  sessionStorage.setItem(VERIFIER_KEY, verifier);
  const q = new URLSearchParams({
    client_id: CONFIG.clientId,
    response_type: "code",
    redirect_uri: redirectUri(),
    code_challenge: challenge,
    code_challenge_method: "S256",
  });
  window.location.assign(`${oauth("authorize")}?${q}`);
}

export function signOut(): void {
  sessionStorage.removeItem(SESSION_KEY);
}

function stored(): Session | null {
  try {
    const s = JSON.parse(sessionStorage.getItem(SESSION_KEY) || "null") as Session | null;
    // A minute of slack so a query does not start on a token about to lapse.
    return s && s.expires > Date.now() + 60_000 ? s : null;
  } catch {
    return null;
  }
}

/** Finishes a sign-in if we have just come back from ArcGIS, else returns the saved session. */
export async function currentSession(): Promise<Session | null> {
  const params = new URLSearchParams(window.location.search);
  const code = params.get("code");
  const error = params.get("error_description") || params.get("error");
  if (error) {
    window.history.replaceState(null, "", window.location.pathname);
    throw new Error(`ArcGIS sign-in: ${error}`);
  }
  if (!code) return stored();

  const verifier = sessionStorage.getItem(VERIFIER_KEY) || "";
  sessionStorage.removeItem(VERIFIER_KEY);
  window.history.replaceState(null, "", window.location.pathname);

  const r = await fetch(oauth("token"), {
    method: "POST",
    body: new URLSearchParams({
      client_id: CONFIG.clientId,
      grant_type: "authorization_code",
      code,
      redirect_uri: redirectUri(),
      code_verifier: verifier,
    }),
  });
  const j = await r.json();
  if (j.error) throw new Error(`ArcGIS sign-in: ${j.error_description || j.error.message || j.error}`);
  const session: Session = {
    token: j.access_token,
    username: j.username,
    expires: Date.now() + Number(j.expires_in) * 1000,
  };
  sessionStorage.setItem(SESSION_KEY, JSON.stringify(session));
  return session;
}
