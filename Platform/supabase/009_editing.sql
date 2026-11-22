-- 009: editing spray records in the map workspace. Safe to run more than once.
--
-- 1. edited_at / edited_by / deleted_at on points, lines and areas.
-- 2. A guard: once a person edits or deletes a record in the platform, the
--    hourly ArcGIS import (which signs in with the service key) can no longer
--    change it. New ArcGIS records still come in as usual.
-- 3. Deleting in the app only hides a record (deleted_at), so the import can't
--    bring it back and it can be restored if needed.
-- 4. The report_* views skip deleted records and now also give the ids the
--    edit form needs.

do $$
declare t text;
begin
  foreach t in array array['treatment_points', 'treatment_lines', 'treatment_areas'] loop
    execute format('alter table public.%I add column if not exists edited_at timestamptz', t);
    execute format('alter table public.%I add column if not exists edited_by uuid', t);
    execute format('alter table public.%I add column if not exists deleted_at timestamptz', t);
  end loop;
end $$;

create or replace function public.guard_edited_records()
returns trigger language plpgsql as $$
begin
  if auth.uid() is not null then
    -- a person (the app): stamp the edit
    new.edited_at := now();
    new.edited_by := auth.uid();
  elsif coalesce(auth.role(), '') = 'service_role'
        and (old.edited_at is not null or old.deleted_at is not null) then
    -- the import, on a record someone has edited or deleted: keep it as it is
    return old;
  end if;
  return new;
end $$;

do $$
declare t text;
begin
  foreach t in array array['treatment_points', 'treatment_lines', 'treatment_areas'] loop
    execute format('drop trigger if exists %I on public.%I', t || '_guard', t);
    execute format('create trigger %I before update on public.%I for each row execute function public.guard_edited_records()',
                   t || '_guard', t);
  end loop;
end $$;

-- Views: same columns as 007 first (so they can be replaced in place), then the
-- extra ones; deleted records left out.
do $$
declare
  v record;
begin
  for v in select * from (values
      ('report_points', 'treatment_points'),
      ('report_lines',  'treatment_lines'),
      ('report_areas',  'treatment_areas')) as t(view_name, table_name)
  loop
    execute format($f$
      create or replace view public.%I with (security_invoker = true) as
      select t.id, t.org_id, t.job_id, t.treated_at,
             j.job_number                                  as "Job_Number",
             s.name                                        as weed_species,
             m.name                                        as control_type,
             (extract(epoch from t.treated_at) * 1000)::bigint as treatment_time,
             t.accuracy_m                                  as gps_accuracy_m,
             t.run_id, t.capture_mode,
             st_asgeojson(t.geom, 7)::json                 as geometry,
             t.species_id, t.method_id, t.notes, t.quantity, t.source, t.edited_at
      from public.%I t
      left join public.jobs j            on j.id = t.job_id
      left join public.species s         on s.id = t.species_id
      left join public.control_methods m on m.id = t.method_id
      where t.deleted_at is null
    $f$, v.view_name, v.table_name);
  end loop;
end $$;
