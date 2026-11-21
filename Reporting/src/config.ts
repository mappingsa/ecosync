// Everything that is specific to this deployment lives here, so the rest of the
// app does not care where the data is kept. Moving off ArcGIS Online means a new
// DataSource (src/data) and a new block here - nothing else.

const SERVICES = "https://services2.arcgis.com/2qaBxVgMaOYvQXaU/arcgis/rest/services";

export const CONFIG = {
  portal: "https://www.arcgis.com",
  // Public ID of the "EcoSync Reporting" OAuth credentials (user authentication).
  // Not a secret: it only identifies the app to the ArcGIS sign-in page.
  clientId: "Z5hdM6y1xmyKp2Wp",

  timeZone: "Australia/Adelaide",
  accuracyThresholdM: 3,

  // Job-sheet weather warnings. Delta T window from survey123_spray_form_changes.md
  // (work 2-8, do not spray above 10). Wind limits: set to match your product labels.
  sprayLimits: { deltaTLow: 2, deltaTHigh: 8, deltaTStop: 10, windMinKmh: 3, windMaxKmh: 20 },

  // Printed under Positional data quality on the report.
  receiver: "u-blox F10 dual-band L1/L5, SouthPAN augmented",

  // Photos are shrunk to this long edge before upload (Form 1 used 1280 px).
  photoMaxPx: 1280,

  layers: {
    points: `${SERVICES}/MSA_Weed_Spray_Points/FeatureServer/0`,
    polygons: `${SERVICES}/MSA_Weed_Spray_Polygons/FeatureServer/0`,
    lines: `${SERVICES}/Weed_Spray_Lines/FeatureServer/0`,
    // Survey123 Form 1 - read only, for the job sheet until Daily Records exists.
    form1: `${SERVICES}/service_d66efa637ea6410badeae3eb893e3740/FeatureServer/0`,
  },

  // Hosted tables are found by item title, so no URLs need copying in here.
  tables: {
    jobs: "EcoSync Jobs",
    targets: "EcoSync Targets",
    clients: "EcoSync Clients",
    lists: "EcoSync Lists",
  },

  // Letterhead and report look per trading entity (the Jobs table's "entity"
  // column). FPE follows the FPE documents in Drive: navy and green from the
  // logo, business details top left as on the invoice, the contact strip from
  // the EcoSyncField info sheet as the footer.
  entities: {
    MSA: {
      name: "Mapping Services Australia",
      logo: "/msa_logo.jpg",
      brand: { primary: "#1C2924", accent: "#7A847F", letterhead: [], footerLeft: "Spray recording MSA", footerRight: "" },
    },
    FPE: {
      name: "FieldPoint Environmental",
      logo: "/fpe_logo.png",
      brand: {
        primary: "#01385C",
        accent: "#487020",
        letterhead: [
          "FieldPoint Environmental",
          "JD & TJ Peltz · Box 341 Monash SA 5342",
          "0429 844 652 · jayme@fieldpointenvironmental.com.au",
          "fieldpointenvironmental.com.au · ABN 37 427 191 874",
        ],
        footerLeft: "FieldPoint Environmental · Riverland, South Australia · ABN 37 427 191 874",
        footerRight: "info@fieldpointenvironmental.com.au · 0429 844 652 · fieldpointenvironmental.com.au",
      },
    },
  } as Record<string, { name: string; logo: string; brand: Brand }>,

  basemap: {
    tiles: "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
    attribution: "Imagery: Esri World Imagery",
  },
} as const;

export interface Brand {
  primary: string; // headings, title bar, table headers
  accent: string; // heading underline, footer rule
  letterhead: string[]; // business details, top left; first line bold
  footerLeft: string;
  footerRight: string;
}

export type FeatureLayerKey = "points" | "polygons" | "lines";

// Fallback only: the live list is the "EcoSync Targets" table in ArcGIS.
// One colour per species, used by the map AND the legend, so the two can never
// disagree.
export interface Target {
  species: string;
  method: "Spray" | "Cut and swab";
  colour: string;
}

export const TARGETS: Target[] = [
  { species: "African Boxthorn", method: "Spray", colour: "#E31A1C" },
  { species: "African Boxthorn", method: "Cut and swab", colour: "#63D91E" },
  { species: "Boneseed", method: "Cut and swab", colour: "#E9ED09" },
  { species: "Gazania", method: "Spray", colour: "#D45324" },
  { species: "Noogoora Burr", method: "Spray", colour: "#25E80C" },
  { species: "Golden Dodder", method: "Spray", colour: "#E2ED07" },
  { species: "Buffel Grass", method: "Spray", colour: "#EB0C9D" },
];

// Species not in the list still draw, in these colours, in a stable order.
export const FALLBACK_COLOURS = ["#1F78B4", "#FF7F00", "#6A3D9A", "#B15928", "#A6CEE3", "#FB9A99", "#CAB2D6"];
