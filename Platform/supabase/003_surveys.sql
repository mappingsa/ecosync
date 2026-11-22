-- EcoSync Data Platform - surveys and threatened species (run after 001 and 002).
-- SQL Editor > New query > paste all of this > Run. Safe to run again.
--
-- 1. Species kinds: weed, pest, threatened, native, other.
-- 2. Sensitive species (most threatened species): viewers and exports see the
--    location blurred to about 1 km; owners and editors see it exactly.
-- 3. Survey types: each organisation defines its surveys and their questions
--    (tree health, vegetation condition, threatened species...). Answers go in
--    survey_records.data, so a new survey type needs no table change.

alter table public.species drop constraint if exists species_kind_check;
alter table public.species add constraint species_kind_check
  check (kind in ('weed', 'pest', 'threatened', 'native', 'other'));
alter table public.species add column if not exists sensitive boolean not null default false;
alter table public.species add column if not exists conservation_status text;   -- e.g. EPBC Endangered, SA Rare

create table if not exists public.survey_types (
  id          uuid primary key default gen_random_uuid(),
  org_id      uuid not null references public.organisations(id) on delete cascade,
  name        text not null,                 -- Tree health, Vegetation condition, Threatened species
  geometry    text not null default 'point' check (geometry in ('point', 'line', 'polygon', 'any')),
  -- Questions, in order: [{"key":"canopy","label":"Canopy health","type":"choice",
  --   "choices":["Good","Fair","Poor","Dead"],"required":true}, ...]
  -- type: text, number, choice, multichoice, species, date, yes_no, photo
  fields      jsonb not null default '[]',
  active      boolean not null default true,
  unique (org_id, name)
);

-- The survey type now comes from the list; the old free-text column becomes optional.
alter table public.survey_records alter column survey_type drop not null;
alter table public.survey_records add column if not exists survey_type_id uuid references public.survey_types(id) on delete restrict;
alter table public.survey_records add column if not exists species_id uuid references public.species(id) on delete set null;
alter table public.survey_records add column if not exists count integer;
create index if not exists survey_records_type_idx on public.survey_records (org_id, survey_type_id, surveyed_at);
create index if not exists pest_observations_job_idx on public.pest_observations (org_id, job_id, observed_at);

alter table public.survey_types enable row level security;
drop policy if exists survey_types_read on public.survey_types;
drop policy if exists survey_types_write on public.survey_types;
create policy survey_types_read  on public.survey_types for select using (public.has_role(org_id, 'viewer'));
create policy survey_types_write on public.survey_types for all
  using (public.has_role(org_id, 'owner')) with check (public.has_role(org_id, 'owner'));

-- Is this record's species sensitive, and is the person below editor?
-- Then the shared views below blur its location.
create or replace function public.blur_location(org uuid, species uuid)
returns boolean language sql stable security definer set search_path = public as $$
  select coalesce((select s.sensitive from public.species s where s.id = species), false)
     and not public.has_role(org, 'editor');
$$;

-- Views for viewers, exports and the live map feed: same rows (security
-- passes through), sensitive locations snapped to a ~1 km grid.
create or replace view public.survey_records_shared with (security_invoker = true) as
  select r.id, r.org_id, r.job_id, r.survey_type_id, r.species_id, r.count, r.surveyed_at, r.data,
         case when public.blur_location(r.org_id, r.species_id)
              then ST_SnapToGrid(ST_Centroid(r.geom), 0.01) else r.geom end as geom,
         public.blur_location(r.org_id, r.species_id) as location_blurred,
         case when public.blur_location(r.org_id, r.species_id) then null else r.accuracy_m end as accuracy_m
  from public.survey_records r;

create or replace view public.pest_observations_shared with (security_invoker = true) as
  select o.id, o.org_id, o.job_id, o.species_id, o.method_id, o.observed_at, o.count, o.activity, o.notes,
         case when public.blur_location(o.org_id, o.species_id)
              then ST_SnapToGrid(o.geom, 0.01) else o.geom end as geom,
         public.blur_location(o.org_id, o.species_id) as location_blurred
  from public.pest_observations o;

-- Starter survey types for FieldPoint Environmental (edit freely in the app later).
do $$
declare org uuid;
begin
  select id into org from public.organisations where name = 'FieldPoint Environmental';
  if org is null then return; end if;

  insert into public.survey_types (org_id, name, geometry, fields) values
  (org, 'Tree health', 'point', '[
     {"key":"species","label":"Tree species","type":"species"},
     {"key":"canopy","label":"Canopy health","type":"choice","choices":["Healthy","Minor dieback","Moderate dieback","Severe dieback","Dead"],"required":true},
     {"key":"dbh_cm","label":"Trunk diameter at breast height (cm)","type":"number"},
     {"key":"hollows","label":"Hollows present","type":"yes_no"},
     {"key":"threats","label":"Threats","type":"multichoice","choices":["Mistletoe","Insect attack","Fire damage","Grazing","Drought stress","Other"]},
     {"key":"photo","label":"Photo","type":"photo"}]'),
  (org, 'Vegetation condition', 'polygon', '[
     {"key":"veg_type","label":"Vegetation type","type":"text"},
     {"key":"condition","label":"Overall condition","type":"choice","choices":["Excellent","Good","Moderate","Poor","Degraded"],"required":true},
     {"key":"native_cover_pct","label":"Native cover (%)","type":"number"},
     {"key":"weed_cover_pct","label":"Weed cover (%)","type":"number"},
     {"key":"bare_ground_pct","label":"Bare ground (%)","type":"number"},
     {"key":"regeneration","label":"Natural regeneration","type":"choice","choices":["None","Some","Abundant"]},
     {"key":"photo","label":"Photo","type":"photo"}]'),
  (org, 'Threatened species', 'point', '[
     {"key":"species","label":"Species","type":"species","required":true},
     {"key":"count","label":"Number seen","type":"number"},
     {"key":"life_stage","label":"Life stage","type":"choice","choices":["Seedling / juvenile","Mature","Flowering / breeding","Dead"]},
     {"key":"evidence","label":"Evidence","type":"choice","choices":["Seen","Heard","Signs (scats, tracks, diggings)","Specimen"]},
     {"key":"habitat","label":"Habitat notes","type":"text"},
     {"key":"photo","label":"Photo","type":"photo"}]')
  on conflict (org_id, name) do nothing;
end $$;
