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

Both scripts were tested against Postgres 16 (PostGIS and Supabase auth and
storage stubbed): schema builds, the seed runs twice without duplicates, and
the security rules let an owner see everything, a job-limited viewer see only
that job, and an outsider see nothing.

### Keys

Project Settings > API shows the project URL, the **anon** key (safe in the
web app) and the **service_role** key. The service_role key bypasses all
security: never put it in the app, email it or paste it into chat. The
ArcGIS importer (next step) keeps it in a local file that git ignores.
