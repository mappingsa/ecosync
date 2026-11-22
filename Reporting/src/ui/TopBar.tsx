import type { Backend } from "../data/types";
import { Icon, Logo } from "./Icon";
import type { Theme } from "./theme";

export type Tab = "report" | "map";

interface Props {
  tab: Tab;
  onTab: ((t: Tab) => void) | null; // null: only the report is available
  theme: Theme;
  onTheme: () => void;
  backend: Backend;
  user: string;
  onSignOut: () => void;
}

const SOURCE: Record<Backend, string> = { arcgis: "ArcGIS", platform: "EcoSync database" };

export function TopBar({ tab, onTab, theme, onTheme, backend, user, onSignOut }: Props) {
  return (
    <header className="topbar">
      <div className="brand">
        <Logo />
        <span className="brand-name">EcoSync</span>
        <span className="brand-sub">Field data</span>
      </div>

      {onTab && (
        <nav className="seg" aria-label="Views">
          <button className={tab === "report" ? "on" : ""} onClick={() => onTab("report")}><Icon name="report" size={16} />Report</button>
          <button className={tab === "map" ? "on" : ""} onClick={() => onTab("map")}><Icon name="map" size={16} />Map</button>
        </nav>
      )}

      <div className="topbar-right">
        <span className={`source ${backend}`} title={`Data from ${SOURCE[backend]}`}><i />{SOURCE[backend]}</span>
        <span className="user" title={user}>{user}</span>
        <button className="icon-btn" onClick={onTheme} title={theme === "dark" ? "Light mode" : "Dark mode"} aria-label="Switch light or dark mode">
          <Icon name={theme === "dark" ? "sun" : "moon"} />
        </button>
        <button className="icon-btn" onClick={onSignOut} title="Sign out" aria-label="Sign out"><Icon name="logout" /></button>
      </div>
    </header>
  );
}
