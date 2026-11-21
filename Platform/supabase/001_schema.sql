-- EcoSync Data Platform - database schema (phase 1)
-- Run once in Supabase: SQL Editor > New query > paste > Run.
--
-- Organisations own everything. Every table carries org_id, and row-level
-- security means a signed-in person only ever sees rows of organisations they
-- belong to, at their role. Domain lists (species, control methods, jobs,
-- clients, list values) are tables, so records point at an id instead of
-- holding free text that can be misspelt.

create extension if not exists postgis;
create extension if not exists pgcrypto;

-- ---------------------------------------------------------------------------
-- Organisations, members, roles
-- ---------------------------------------------------------------------------

create table public.organisations (
  id          uuid primary key default gen_random_uuid(),
  name        text not null,
  created_at  timestamptz not null default now()
);

create type public.member_role as enum ('owner', 'editor', 'viewer');

create table public.members (
  org_id      uuid not null references public.organisations(id) on delete cascade,
  user_id     uuid not null references auth.users(id) on delete cascade,
  role        public.member_role not null,
  -- Viewers only: limit to these jobs. Null = all jobs.
  job_ids     uuid[],
  created_at  timestamptz not null default now(),
  primary key (org_id, user_id)
);

-- Role checks used by every policy. SECURITY DEFINER so they can read
-- members without tripping members' own policies.
create function public.has_role(org uuid, min_role public.member_role)
returns boolean language sql stable security definer set search_path = public as $$
  select exists (
    select 1 from public.members m
    where m.org_id = org and m.user_id = auth.uid()
      and case min_role
            when 'viewer' then true
            when 'editor' then m.role in ('owner', 'editor')
            when 'owner'  then m.role = 'owner'
          end
  );
$$;

create function public.can_see_job(org uuid, job uuid)
returns boolean language sql stable security definer set search_path = public as $$
  select exists (
    select 1 from public.members m
    where m.org_id = org and m.user_id = auth.uid()
      and (m.role <> 'viewer' or m.job_ids is null or job = any (m.job_ids))
  );
$$;

-- Whoever creates an organisation becomes its owner.
create function public.make_creator_owner()
returns trigger language plpgsql security definer set search_path = public as $$
begin
  if auth.uid() is not null then
    insert into public.members (org_id, user_id, role) values (new.id, auth.uid(), 'owner');
  end if;
  return new;
end $$;
create trigger organisations_creator_owner after insert on public.organisations
  for each row execute function public.make_creator_owner();

create function public.touch_updated_at()
returns trigger language plpgsql as $$
begin new.updated_at := now(); return new; end $$;

-- ---------------------------------------------------------------------------
-- Domain lists
-- ---------------------------------------------------------------------------

create table public.clients (
  id          uuid primary key default gen_random_uuid(),
  org_id      uuid not null references public.organisations(id) on delete cascade,
  code        text not null,                -- DEW, EP, AW, RM, SAAL
  name        text not null,
  logo_path   text,
  unique (org_id, code)
);

create table public.control_methods (
  id          uuid primary key default gen_random_uuid(),
  org_id      uuid not null references public.organisations(id) on delete cascade,
  name        text not null,                -- Spot Spray, Foliar spray, Cut and swab...
  applies_to  text not null default 'weed' check (applies_to in ('weed', 'pest', 'both')),
  active      boolean not null default true,
  unique (org_id, name)
);

create table public.species (
  id                uuid primary key default gen_random_uuid(),
  org_id            uuid not null references public.organisations(id) on delete cascade,
  name              text not null,
  kind              text not null check (kind in ('weed', 'pest', 'other')),
  scientific_name   text,
  declared_status   text,
  default_method_id uuid references public.control_methods(id) on delete set null,
  colour            text not null default '#1F78B4' check (colour ~ '^#[0-9A-Fa-f]{6}$'),
  aliases           text[] not null default '{}',   -- other spellings the unit or ArcGIS used
  active            boolean not null default true,
  unique (org_id, name)
);

-- Optional colour per species AND method (sprayed Boxthorn red, cut-and-swab green).
create table public.species_method_colours (
  species_id  uuid not null references public.species(id) on delete cascade,
  method_id   uuid not null references public.control_methods(id) on delete cascade,
  org_id      uuid not null references public.organisations(id) on delete cascade,
  colour      text not null check (colour ~ '^#[0-9A-Fa-f]{6}$'),
  primary key (species_id, method_id)
);

create table public.jobs (
  id                uuid primary key default gen_random_uuid(),
  org_id            uuid not null references public.organisations(id) on delete cascade,
  job_number        text not null,          -- CLIENT-number, e.g. EP-C489.2
  client_id         uuid references public.clients(id) on delete set null,
  letterhead        text,                   -- MSA or FPE
  title             text,
  property_address  text,
  owner_contact     text,
  default_equipment text,
  default_chemicals text,
  aliases           text[] not null default '{}',   -- old names: 'DEW NP Weed Control'
  active            boolean not null default true,
  starts_on         date,
  ends_on           date,
  notes             text,
  unique (org_id, job_number)
);

create table public.list_values (
  id          uuid primary key default gen_random_uuid(),
  org_id      uuid not null references public.organisations(id) on delete cascade,
  list        text not null check (list in ('operator', 'equipment', 'chemical')),
  value       text not null,
  active      boolean not null default true,
  unique (org_id, list, value)
);

create table public.units (
  id            uuid primary key default gen_random_uuid(),
  org_id        uuid not null references public.organisations(id) on delete cascade,
  serial        text not null unique,
  name          text,
  firmware      text,
  last_sync_at  timestamptz,
  key_hash      text                        -- sync key, stored hashed (phase 4)
);

-- ---------------------------------------------------------------------------
-- Layers
-- ---------------------------------------------------------------------------
-- source / source_ref: where a record came from ('arcgis' + layer:objectid,
-- 'unit', 'app', 'import'). Unique per organisation, so the ArcGIS importer
-- can run every hour without making duplicates.

create table public.treatment_points (
  id            uuid primary key default gen_random_uuid(),
  org_id        uuid not null references public.organisations(id) on delete cascade,
  job_id        uuid references public.jobs(id) on delete set null,
  species_id    uuid references public.species(id) on delete set null,
  method_id     uuid references public.control_methods(id) on delete set null,
  unit_id       uuid references public.units(id) on delete set null,
  treated_at    timestamptz not null,
  accuracy_m    numeric,
  run_id        text,
  capture_mode  text,
  quantity      numeric,
  notes         text,
  source        text not null default 'app',
  source_ref    text,
  geom          geometry(Point, 4326) not null,
  created_by    uuid default auth.uid(),
  created_at    timestamptz not null default now(),
  updated_at    timestamptz not null default now(),
  unique (org_id, source, source_ref)
);

create table public.treatment_lines (like public.treatment_points including defaults including constraints);
alter table public.treatment_lines add primary key (id), add unique (org_id, source, source_ref),
  alter column geom type geometry(MultiLineString, 4326);

create table public.treatment_areas (like public.treatment_points including defaults including constraints);
alter table public.treatment_areas add primary key (id), add unique (org_id, source, source_ref),
  alter column geom type geometry(MultiPolygon, 4326);

create table public.tracks (
  id          uuid primary key default gen_random_uuid(),
  org_id      uuid not null references public.organisations(id) on delete cascade,
  job_id      uuid references public.jobs(id) on delete set null,
  unit_id     uuid references public.units(id) on delete set null,
  started_at  timestamptz not null,
  ended_at    timestamptz,
  source      text not null default 'unit',
  source_ref  text,
  geom        geometry(MultiLineString, 4326) not null,
  created_at  timestamptz not null default now(),
  unique (org_id, source, source_ref)
);

create table public.pest_observations (
  id            uuid primary key default gen_random_uuid(),
  org_id        uuid not null references public.organisations(id) on delete cascade,
  job_id        uuid references public.jobs(id) on delete set null,
  species_id    uuid references public.species(id) on delete set null,
  method_id     uuid references public.control_methods(id) on delete set null,  -- bait, trap, observation
  observed_at   timestamptz not null,
  count         integer,
  activity      text,
  accuracy_m    numeric,
  notes         text,
  source        text not null default 'app',
  source_ref    text,
  geom          geometry(Point, 4326) not null,
  created_by    uuid default auth.uid(),
  created_at    timestamptz not null default now(),
  updated_at    timestamptz not null default now(),
  unique (org_id, source, source_ref)
);

create table public.survey_records (
  id            uuid primary key default gen_random_uuid(),
  org_id        uuid not null references public.organisations(id) on delete cascade,
  job_id        uuid references public.jobs(id) on delete set null,
  survey_type   text not null,              -- tree_health, vegetation, ...
  surveyed_at   timestamptz not null,
  data          jsonb not null default '{}',
  accuracy_m    numeric,
  source        text not null default 'app',
  source_ref    text,
  geom          geometry(Geometry, 4326) not null,
  created_by    uuid default auth.uid(),
  created_at    timestamptz not null default now(),
  updated_at    timestamptz not null default now(),
  unique (org_id, source, source_ref)
);

-- Job sheets: same fields as the reporting app's Daily Records.
create table public.daily_records (
  id              uuid primary key default gen_random_uuid(),
  org_id          uuid not null references public.organisations(id) on delete cascade,
  job_id          uuid not null references public.jobs(id) on delete restrict,
  record_date     date not null,
  letterhead      text,
  start_time      text,
  end_time        text,
  operators       text[] not null default '{}',
  equipment       text[] not null default '{}',
  chemicals       text[] not null default '{}',
  tank_total_l    numeric,
  start_temp_c    numeric, start_rh_pct numeric, start_wind_kmh numeric, start_wind_dir text, start_delta_t numeric,
  end_temp_c      numeric, end_rh_pct numeric, end_wind_kmh numeric, end_wind_dir text, end_delta_t numeric,
  weather_source  text,
  observations    text,
  flag_issues     text,
  status          text not null default 'draft' check (status in ('draft', 'issued')),
  source          text not null default 'app',
  source_ref      text,
  updated_by      uuid default auth.uid(),
  created_at      timestamptz not null default now(),
  updated_at      timestamptz not null default now(),
  unique (org_id, job_id, record_date)
);

create table public.attachments (
  id              uuid primary key default gen_random_uuid(),
  org_id          uuid not null references public.organisations(id) on delete cascade,
  daily_record_id uuid not null references public.daily_records(id) on delete cascade,
  kind            text not null check (kind in ('photo', 'signature', 'report')),
  storage_path    text not null,            -- in the 'attachments' bucket: <org_id>/<record_id>/<file>
  name            text not null,
  content_type    text,
  size_bytes      bigint,
  created_at      timestamptz not null default now()
);

-- Values the importer could not match to a domain list: fix once, not daily.
create table public.needs_a_home (
  id          bigserial primary key,
  org_id      uuid not null references public.organisations(id) on delete cascade,
  source      text not null,                -- e.g. arcgis:MSA_Weed_Spray_Points
  field       text not null,                -- job_number, weed_species, control_type
  value       text not null,
  seen        integer not null default 1,
  first_seen  timestamptz not null default now(),
  last_seen   timestamptz not null default now(),
  unique (org_id, source, field, value)
);

-- Spatial and lookup indexes
create index on public.treatment_points using gist (geom);
create index on public.treatment_lines  using gist (geom);
create index on public.treatment_areas  using gist (geom);
create index on public.tracks           using gist (geom);
create index on public.pest_observations using gist (geom);
create index on public.survey_records   using gist (geom);
create index on public.treatment_points (org_id, job_id, treated_at);
create index on public.treatment_lines  (org_id, job_id, treated_at);
create index on public.treatment_areas  (org_id, job_id, treated_at);
create index on public.tracks           (org_id, job_id, started_at);
create index on public.attachments      (daily_record_id);

do $$
declare t text;
begin
  foreach t in array array['treatment_points', 'treatment_lines', 'treatment_areas',
                           'pest_observations', 'survey_records', 'daily_records'] loop
    execute format('create trigger %I before update on public.%I for each row execute function public.touch_updated_at()',
                   t || '_touch', t);
  end loop;
end $$;

-- ---------------------------------------------------------------------------
-- Row-level security
-- ---------------------------------------------------------------------------

alter table public.organisations enable row level security;
create policy org_read   on public.organisations for select using (public.has_role(id, 'viewer'));
create policy org_create on public.organisations for insert with check (auth.uid() is not null);
create policy org_update on public.organisations for update using (public.has_role(id, 'owner'));

alter table public.members enable row level security;
create policy members_read  on public.members for select using (public.has_role(org_id, 'viewer'));
create policy members_write on public.members for all
  using (public.has_role(org_id, 'owner')) with check (public.has_role(org_id, 'owner'));

-- Domain lists: everyone in the organisation reads; owners change species,
-- methods, clients and units; editors may also add jobs and list values.
do $$
declare t text;
begin
  foreach t in array array['clients', 'control_methods', 'species', 'species_method_colours', 'units',
                           'jobs', 'list_values'] loop
    execute format('alter table public.%I enable row level security', t);
    if t = 'jobs' then
      -- job-limited viewers see only their own jobs in the list
      execute 'create policy jobs_read on public.jobs for select using (public.can_see_job(org_id, id))';
    else
      execute format('create policy %I on public.%I for select using (public.has_role(org_id, ''viewer''))', t || '_read', t);
    end if;
  end loop;
  foreach t in array array['clients', 'control_methods', 'species', 'species_method_colours', 'units'] loop
    execute format('create policy %I on public.%I for all using (public.has_role(org_id, ''owner'')) with check (public.has_role(org_id, ''owner''))',
                   t || '_write', t);
  end loop;
  foreach t in array array['jobs', 'list_values'] loop
    execute format('create policy %I on public.%I for all using (public.has_role(org_id, ''editor'')) with check (public.has_role(org_id, ''editor''))',
                   t || '_write', t);
  end loop;
end $$;

-- Layers and job sheets: viewers see only their jobs; editors write.
do $$
declare t text;
begin
  foreach t in array array['treatment_points', 'treatment_lines', 'treatment_areas', 'tracks',
                           'pest_observations', 'survey_records', 'daily_records'] loop
    execute format('alter table public.%I enable row level security', t);
    execute format('create policy %I on public.%I for select using (public.can_see_job(org_id, job_id))', t || '_read', t);
    execute format('create policy %I on public.%I for all using (public.has_role(org_id, ''editor'')) with check (public.has_role(org_id, ''editor''))',
                   t || '_write', t);
  end loop;
end $$;

alter table public.attachments enable row level security;
create policy attachments_read on public.attachments for select using (
  exists (select 1 from public.daily_records d where d.id = daily_record_id and public.can_see_job(d.org_id, d.job_id)));
create policy attachments_write on public.attachments for all
  using (public.has_role(org_id, 'editor')) with check (public.has_role(org_id, 'editor'));

alter table public.needs_a_home enable row level security;
create policy needs_a_home_read  on public.needs_a_home for select using (public.has_role(org_id, 'editor'));
create policy needs_a_home_write on public.needs_a_home for all
  using (public.has_role(org_id, 'editor')) with check (public.has_role(org_id, 'editor'));

-- ---------------------------------------------------------------------------
-- File storage: photos, signatures, issued PDFs
-- ---------------------------------------------------------------------------
-- Private bucket; files live under <org_id>/..., and the folder name decides
-- who may read or write them.

insert into storage.buckets (id, name, public) values ('attachments', 'attachments', false)
  on conflict (id) do nothing;

create policy attachments_files_read on storage.objects for select using (
  bucket_id = 'attachments' and public.has_role(((storage.foldername(name))[1])::uuid, 'viewer'));
create policy attachments_files_write on storage.objects for insert with check (
  bucket_id = 'attachments' and public.has_role(((storage.foldername(name))[1])::uuid, 'editor'));
create policy attachments_files_delete on storage.objects for delete using (
  bucket_id = 'attachments' and public.has_role(((storage.foldername(name))[1])::uuid, 'editor'));
