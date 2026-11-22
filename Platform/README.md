# EcoSync Data Platform

Plan: https://claude.ai/artifact/SofCNk1CTRkswFdtMriWDb

## Phase 1: database (Supabase)

`supabase/001_schema.sql` creates everything: organisations and roles
(owner, editor, viewer, with job-limited viewers), the domain lists (clients,
control methods, species with colours, species+method colours, jobs with
aliases, list values, units), the layers (treatment points, lines, areas,
tracks, pest observations, survey records), job sheets and attachments, the
"needs a home" list for unmatched imports, row-level security on every table,
and a private `attachments` file bucket.

`supabase/002_seed_fpe.sql` adds FieldPoint Environmental as the first
organisation, makes you its owner, and loads the clients, methods, species,
jobs (with their old names as aliases) and operator/equipment/chemical lists
from the ArcGIS EcoSync tables.

`supabase/003_surveys.sql` adds survey support: species kinds (weed, pest,
threatened, native, other), a **sensitive** flag that blurs a species'
locations to about 1 km for viewers and exports, a **survey types** list where
each survey's questions are defined (starts with Tree health, Vegetation
condition, Threatened species), and the shared views the map feed and exports
use. Run it after 001 and 002; safe to run again.

### Set up (once)

1. Supabase dashboard > New project. Name `ecosync`, region
   **Asia-Pacific (Sydney)**, a strong database password (keep it in your
   password manager). Wait for it to finish.
2. Authentication > Users > **Add user** > your email
   (jayme@fieldpointenvironmental.com.au) with a password.
3. SQL Editor > New query > paste all of `001_schema.sql` > **Run**.
   Should finish with "Success. No rows returned".
4. New query > paste all of `002_seed_fpe.sql` > **Run**. The message shows
   the organisation id.
5. Table Editor: check species (6), jobs (4), list_values (29).
6. New query > paste all of `003_surveys.sql` > **Run**. Check survey_types (3).

Both scripts were tested against Postgres 16 (PostGIS and Supabase auth and
storage stubbed): schema builds, the seed runs twice without duplicates, and
the security rules let an owner see everything, a job-limited viewer see only
that job, and an outsider see nothing.

## ArcGIS importer (`importer\`)

Copies the four ArcGIS spray layers (points, lines, polygons, track) into the
platform every hour while the two run side by side. Species, methods and job
numbers are matched to the lists, including old names (aliases); anything
unmatched is listed in `needs_a_home` in Supabase and in `import_log.txt`.
Records are keyed by their ArcGIS GlobalID, so re-running never duplicates.
ArcGIS stays the source of truth: edit there, not in the platform, until the
switch-over.

1. Copy `importer\.env.example` to `importer\.env` and fill in the Supabase
   URL and service key (Project Settings > API).
2. Double-click **Import now.bat** with `--full` the first time to bring in
   all history: in a command window, `"Import now.bat" --full`. Check the
   summary and the needs_a_home list; fix unmatched values by adding aliases
   to jobs or species, then run again.
3. Double-click **Set up hourly import.bat** to run it every hour.

ArcGIS sign-in comes from `EcoSync\tools\spray_stats_config.json`, the file
End of Day uses. `python -m unittest test_mapping` runs the tests.

### Keys

Project Settings > API shows the project URL, the **anon** key (safe in the
web app) and the **service_role** key. The service_role key bypasses all
security: never put it in the app, email it or paste it into chat. The
ArcGIS importer (next step) keeps it in a local file that git ignores.
