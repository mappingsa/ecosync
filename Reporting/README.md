# EcoSync Reporting

Web app that builds the daily spray report from EcoSyncField data.
Plan: https://claude.ai/artifact/CfD7iWgvoKoF7kyzWS8BqR

## Milestone 1 (this version): read-only report

Sign in with ArcGIS, pick a day and job, and see:

- the day's points, lines and polygons on imagery, coloured by target
- counts per target and control method, line length, polygon area (ha)
- positional accuracy: average, worst, % within 3 m (same rules as End of Day)
- the matching Survey123 Form 1 record, read only
- a warning for records with no job number, and for a job with no Form 1 record

Not yet: treated area from the track (still End of Day), Jobs and Daily Records
tables, the PDF.

## Milestone 2: lookup tables

Four hosted tables in ArcGIS Online, found by title (no URLs to copy):

| Table | What it holds |
| --- | --- |
| EcoSync Jobs | One row per job: job number (CODE-number, e.g. EP-C489.2), client code, entity (MSA or FPE, picks the letterhead), title, address, owner contact, defaults, active |
| EcoSync Clients | Client code to full name (DEW, EP, AW, RM, SAAL) |
| EcoSync Targets | Species, default control method, map and legend colour |
| EcoSync Lists | Operators, equipment and chemicals for the job sheet (milestone 3) |

Starting copies are in `tables\`. To publish each: ArcGIS Online, Content, New item,
upload the CSV, choose **Add CSV and create a hosted table** (no location), and keep
the title exactly as the file name. Then click **Reload tables** in the app.

New job? Add a row to EcoSync Jobs in ArcGIS (open the table, Data tab), using the
job number exactly as set on the unit. The app warns about any job it cannot find.

## Milestone 3: job sheet

Below the map, the **Job sheet** replaces filling in Form 1: times, operators,
equipment, chemicals, total sprayed, weather at start and finish (Delta T is
calculated, warnings outside the limits in `src/config.ts`), observations, flag
issues, photos (shrunk to 1280 px) and signature.

- First use: click **Create the Daily Records table**. It makes the hosted table
  **EcoSync Daily Records** in your ArcGIS content, attachments on.
- One sheet per job per day. A new sheet fills in from that day's Form 1 record
  if there is one, else from the job's defaults in EcoSync Jobs.
- Ticked options come from the EcoSync Lists table (operator / equipment / chemical).

## Milestone 4: PDF report

**Issue report (PDF)** under the job sheet saves the sheet, then builds an A4
spray record in the layout of the old Word template: letterhead logo by entity,
job and site, logistics, weather with Delta T and any warnings, observations,
litres, treatments table and area, positional data quality, overview and detail
maps (drawn from the day's own records), legend from the Targets colours, photos
and signature. It is attached to the Daily Record as `Spray Record <job> <date> vN.pdf`
and downloaded. Re-issuing adds a new version; earlier ones are kept. Saving the
sheet after issuing marks it draft again until re-issued.

## Run it on this PC

1. Install **Node.js LTS** from https://nodejs.org (once).
2. Double-click **Start Reporting.bat**. The first run installs packages (a minute or two).
3. The browser opens https://localhost:5173. It warns the certificate is not
   trusted: that is the app's own development certificate. Choose
   Advanced → Continue to localhost.
4. Sign in with ArcGIS.

## Check it against End of Day

Pick a few past days you already have reports for. Counts and the three accuracy
figures should match the Form 1 record's acc_* fields. Note any that differ.

## Phones: publish to Cloudflare Pages

The app runs on phones from https://ecosync-reporting.pages.dev (already a
registered sign-in address in the ArcGIS "EcoSync Reporting" credentials).

First time:
1. Create a free Cloudflare account at https://dash.cloudflare.com/sign-up.
2. Double-click **Build for publishing.bat**. It builds the site into `dist` and
   opens the folder and the Cloudflare dashboard.
3. In Cloudflare: Workers & Pages, Create, Pages, **Upload assets**. Project name
   `ecosync-reporting`. Drag in everything inside `dist`. Deploy.
4. If Cloudflare gives a different address (name taken), add that address to the
   redirect URLs of the EcoSync Reporting credentials in ArcGIS Online.

Each update after that: run Build for publishing.bat, then in the project click
**Create deployment** and upload `dist` again.

The site only holds the app's code: the data stays in ArcGIS and everyone signs
in with their own ArcGIS login. The phone needs signal (or Starlink Wi-Fi) while
saving; working fully offline is what the Expo app is for later.

## Where things are

| File | What it does |
| --- | --- |
| src/config.ts | Layer URLs, client ID, time zone, Targets colours |
| src/data/arcgis.ts | The only file that talks to ArcGIS (swap this to leave ArcGIS) |
| src/data/auth.ts | ArcGIS sign-in (OAuth PKCE) |
| src/report/stats.ts | Day window, job matching, accuracy, area, length, summary |
| src/report/stats.test.ts | Tests for the above: `npm test` |
| src/components/DayMap.tsx | The map (MapLibre) |
| public/msa_logo.jpg | MSA letterhead logo |
| public/fpe_logo.png | FPE letterhead logo (copy "FieldPoint Environmental transparent logo.png" here) |
| tables/ | Starting CSVs for the four lookup tables |
