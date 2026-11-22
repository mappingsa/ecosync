import { useState, type FormEvent } from "react";
import { platformSignIn, sendPasswordReset, setNewPassword } from "../data/platformAuth";
import type { Session } from "../data/types";

interface Props {
  onArcGIS: () => void;
  onPlatform: (s: Session) => void;
  error: string | null;
}

/** Sign in to ArcGIS (as now) or to the EcoSync database (trial). */
export function SignIn({ onArcGIS, onPlatform, error }: Props) {
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);

  async function submit(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    setErr(null);
    try {
      onPlatform(await platformSignIn(email, password));
    } catch (x: any) {
      setErr(x.message ?? String(x));
    } finally {
      setBusy(false);
    }
  }

  async function forgot() {
    if (!email.trim()) return setErr("Type your email first, then press Forgot password.");
    setErr(null);
    try {
      await sendPasswordReset(email);
      setMsg(`Email sent to ${email.trim()}. Open the link in it on this device to choose a new password.`);
    } catch (x: any) {
      setErr(x.message ?? String(x));
    }
  }

  return (
    <main className="signin">
      <h1>EcoSync Reporting</h1>
      {error && <p className="error">{error}</p>}
      <button onClick={onArcGIS}>Sign in with ArcGIS</button>

      <form className="platform-signin" onSubmit={submit}>
        <h2>EcoSync database (trial)</h2>
        <label>Email <input type="email" autoComplete="username" value={email} onChange={(e) => setEmail(e.target.value)} required /></label>
        <label>Password <input type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} required /></label>
        <button type="submit" disabled={busy}>{busy ? "Signing in…" : "Sign in"}</button>{" "}
        <button type="button" className="link" onClick={forgot}>Forgot password?</button>
        {err && <p className="error">{err}</p>}
        {msg && <p className="ok">{msg}</p>}
      </form>
    </main>
  );
}

/** After a password-reset link: choose the new password. */
export function NewPassword({ onDone }: { onDone: (s: Session) => void }) {
  const [password, setPassword] = useState("");
  const [again, setAgain] = useState("");
  const [err, setErr] = useState<string | null>(null);

  async function submit(e: FormEvent) {
    e.preventDefault();
    if (password.length < 8) return setErr("Use at least 8 characters.");
    if (password !== again) return setErr("The two passwords don't match.");
    try {
      onDone(await setNewPassword(password));
    } catch (x: any) {
      setErr(x.message ?? String(x));
    }
  }

  return (
    <main className="signin">
      <h1>EcoSync Reporting</h1>
      <form className="platform-signin" onSubmit={submit}>
        <h2>Choose a new password</h2>
        <label>New password <input type="password" autoComplete="new-password" value={password} onChange={(e) => setPassword(e.target.value)} required /></label>
        <label>Again <input type="password" autoComplete="new-password" value={again} onChange={(e) => setAgain(e.target.value)} required /></label>
        <button type="submit">Save password</button>
        {err && <p className="error">{err}</p>}
      </form>
    </main>
  );
}
