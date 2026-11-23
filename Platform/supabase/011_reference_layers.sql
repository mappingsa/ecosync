-- 011: reference layers - files imported into the map workspace (property
-- boundaries, target areas, fence lines from Shapefile, KML, GPX, GeoJSON or
-- CSV). Shared with the whole organisation; editors and owners add and
-- delete them. Safe to run more than once.

create table if not exists public.reference_layers (
  id            uuid primary key default gen_random_uuid(),
  org_id        uuid not null references public.organisations(id) on delete cascade,
  name          text not null,
  colour        text not null default '#FFD400',
  source_file   text,
  projection    text,
  feature_count integer not null default 0,
  created_by    uuid default auth.uid(),
  created_at    timestamptz not null default now()
);

create table if not exists public.reference_features (
  id        bigserial primary key,
  layer_id  uuid not null references public.reference_layers(id) on delete cascade,
  org_id    uuid not null references public.organisations(id) on delete cascade,
  props     jsonb not null default '{}'::jsonb,
  geom      geometry(Geometry, 4326) not null
);
create index if not exists reference_features_layer on public.reference_features (layer_id);
create index if not exists reference_features_geom on public.reference_features using gist (geom);

alter table public.reference_layers enable row level security;
alter table public.reference_features enable row level security;

do $$
declare t text;
begin
  foreach t in array array['reference_layers', 'reference_features'] loop
    execute format('drop policy if exists %I on public.%I', t || '_read', t);
    execute format('drop policy if exists %I on public.%I', t || '_write', t);
    execute format('create policy %I on public.%I for select using (public.has_role(org_id, ''viewer''))', t || '_read', t);
    execute format('create policy %I on public.%I for all using (public.has_role(org_id, ''editor'')) with check (public.has_role(org_id, ''editor''))', t || '_write', t);
  end loop;
end $$;

-- the shapes as GeoJSON for the map; the reader's own security applies
create or replace view public.reference_features_json with (security_invoker = true) as
select id, layer_id, props, st_asgeojson(geom, 7)::json as geometry
from public.reference_features;
grant select on public.reference_features_json to authenticated;
revoke all on public.reference_features_json from anon;
