import type { Domains, EditableLayer } from "../data/platform";
import { Icon } from "../ui/Icon";

export interface FormValues {
  job_id: string; // "" = none
  species_id: string;
  method_id: string;
  when: string; // "YYYY-MM-DDTHH:mm", local
  notes: string;
}

interface Props {
  title: string;
  layer: EditableLayer;
  values: FormValues;
  domains: Domains | null;
  saving: boolean;
  onChange: (v: FormValues) => void;
  onSave: () => void;
  onCancel: () => void;
}

/** Job, species, method, time and notes for one record, from the organisation's lists. */
export function EditForm({ title, values: v, domains, saving, onChange, onSave, onCancel }: Props) {
  const set = (patch: Partial<FormValues>) => onChange({ ...v, ...patch });
  // inactive list entries only show when the record already uses them
  const keep = <T extends { id: string; active: boolean }>(xs: T[] | undefined, cur: string) => (xs ?? []).filter((x) => x.active || x.id === cur);
  const pickSpecies = (id: string) => {
    const sp = domains?.species.find((s) => s.id === id);
    // a species with a usual method fills the method in, unless one is already chosen
    set({ species_id: id, method_id: v.method_id || sp?.default_method_id || "" });
  };

  return (
    <form className="pcard editform" onSubmit={(e) => { e.preventDefault(); onSave(); }}>
      <h3><Icon name="edit" size={15} />{title}</h3>
      {!domains && <p className="muted">Loading the lists…</p>}
      <label>
        <span>Job</span>
        <select value={v.job_id} onChange={(e) => set({ job_id: e.target.value })}>
          <option value="">(no job)</option>
          {keep(domains?.jobs, v.job_id).map((j) => <option key={j.id} value={j.id}>{j.job_number}</option>)}
        </select>
      </label>
      {!v.job_id && <p className="field-note">Records with no job don't appear on any report.</p>}
      <label>
        <span>Species</span>
        <select value={v.species_id} onChange={(e) => pickSpecies(e.target.value)}>
          <option value="">(none)</option>
          {keep(domains?.species, v.species_id).map((s) => <option key={s.id} value={s.id}>{s.name}</option>)}
        </select>
      </label>
      <label>
        <span>Method</span>
        <select value={v.method_id} onChange={(e) => set({ method_id: e.target.value })}>
          <option value="">(none)</option>
          {keep(domains?.methods, v.method_id).map((m) => <option key={m.id} value={m.id}>{m.name}</option>)}
        </select>
      </label>
      <label>
        <span>Date and time</span>
        <input type="datetime-local" value={v.when} onChange={(e) => set({ when: e.target.value })} required />
      </label>
      <label>
        <span>Notes</span>
        <textarea rows={2} value={v.notes} onChange={(e) => set({ notes: e.target.value })} />
      </label>
      <div className="btn-row">
        <button type="submit" className="primary" disabled={saving || !v.when}>
          {saving ? <><span className="spinner" />Saving…</> : <><Icon name="check" size={15} />Save</>}
        </button>
        <button type="button" className="ghost" onClick={onCancel} disabled={saving}><Icon name="close" size={15} />Cancel</button>
      </div>
    </form>
  );
}
