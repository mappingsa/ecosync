// Line icons for the app, 24 x 24, drawn with the current text colour.
// Kept in one file and hand-drawn so there's no icon library to install.

const PATHS: Record<string, string> = {
  layers: "M12 3 2.5 8 12 13l9.5-5L12 3Z M2.5 12.5 12 17.5l9.5-5 M2.5 16.5 12 21.5l9.5-5",
  filter: "M3 5h18l-7 8.5V19l-4 2v-7.5L3 5Z",
  info: "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z M12 11v6 M12 7.5v.5",
  ruler: "M3 16.5 16.5 3 21 7.5 7.5 21 3 16.5Z M7 12.5l2 2 M10 9.5l2 2 M13 6.5l2 2",
  pointer: "M5 3l14 7.5-6 1.5-2.5 6L5 3Z",
  table: "M3 5h18v14H3V5Z M3 10h18 M3 14.5h18 M9 5v14",
  download: "M12 4v11 M7 10.5l5 5 5-5 M4 20h16",
  plus: "M12 5v14 M5 12h14",
  minus: "M5 12h14",
  extent: "M4 9V4h5 M15 4h5v5 M20 15v5h-5 M9 20H4v-5",
  locate: "M12 19a7 7 0 1 0 0-14 7 7 0 0 0 0 14Z M12 2v3 M12 19v3 M2 12h3 M19 12h3 M12 14.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5Z",
  sun: "M12 16.5a4.5 4.5 0 1 0 0-9 4.5 4.5 0 0 0 0 9Z M12 2v2 M12 20v2 M4.9 4.9l1.4 1.4 M17.7 17.7l1.4 1.4 M2 12h2 M20 12h2 M4.9 19.1l1.4-1.4 M17.7 6.3l1.4-1.4",
  moon: "M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5Z",
  map: "M9 4 3 6.5v13.5l6-2.5 6 2.5 6-2.5V4l-6 2.5L9 4Z M9 4v13.5 M15 6.5V20",
  report: "M6 2.5h8.5L19 7v14.5H6v-19Z M14 2.5V7h5 M9 12h7 M9 16h7",
  logout: "M14 4h5v16h-5 M10 8l-4 4 4 4 M6 12h10",
  search: "M10.5 17.5a7 7 0 1 0 0-14 7 7 0 0 0 0 14Z M16 16l5 5",
  close: "M6 6l12 12 M18 6 6 18",
  undo: "M9 14 4 9l5-5 M4 9h10.5a5.5 5.5 0 0 1 0 11H11",
  chevronUp: "M6 15l6-6 6 6",
  chevronDown: "M6 9l6 6 6-6",
  calendar: "M4 6h16v15H4V6Z M4 10h16 M8 3v5 M16 3v5",
  check: "M5 12.5 10 17.5 19 7",
  refresh: "M20 11a8 8 0 0 0-14.5-4.5L4 8 M4 4v4h4 M4 13a8 8 0 0 0 14.5 4.5L20 16 M20 20v-4h-4",
  addPoint: "M12 21s-6.5-5.6-6.5-11a6.5 6.5 0 0 1 13 0c0 5.4-6.5 11-6.5 11Z M12 7v6 M9 10h6",
  addLine: "M4 18 9.5 9l5 5L20 5 M4 18h.01 M9.5 9h.01 M14.5 14h.01 M20 5h.01",
  addArea: "M5 7.5 12 3.5l7 5-2.5 10.5h-9L5 7.5Z M5 7.5h.01 M12 3.5h.01 M19 8.5h.01 M16.5 19h.01 M7.5 19h.01",
  edit: "M4 20h4L19.5 8.5a2.1 2.1 0 0 0-4-4L4 16v4Z M14 6l4 4",
  shape: "M5 6.5 12 4l7 4.5-3 10H8L5 6.5Z M12 4v.01 M5 6.5v.01 M19 8.5v.01 M16 18.5v.01 M8 18.5v.01",
  trash: "M4 7h16 M10 11v6 M14 11v6 M6 7l1 13h10l1-13 M9 7V4h6v3",
};

export type IconName = keyof typeof PATHS;

export function Icon({ name, size = 18, title }: { name: IconName; size?: number; title?: string }) {
  return (
    <svg className="icon" width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor"
      strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round" aria-hidden={title ? undefined : true} role={title ? "img" : undefined}>
      {title && <title>{title}</title>}
      <path d={PATHS[name]} />
    </svg>
  );
}

/** The EcoSync mark: a leaf on a location pin. */
export function Logo({ size = 26 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden="true">
      <path d="M16 2C9.4 2 4 7.2 4 13.7 4 22.4 16 30 16 30s12-7.6 12-16.3C28 7.2 22.6 2 16 2Z" fill="var(--accent)" />
      <path d="M10.5 18.5c0-5.5 4-9 11-9.5-.3 6.8-3.9 10.6-9.3 10.6 M12 18l5-5" fill="none" stroke="var(--accent-ink)" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}
