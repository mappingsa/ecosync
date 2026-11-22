-- 007: what the reporting app needs to read from the platform.
-- Safe to run more than once.
--
-- 1. report_points / report_lines / report_areas: the spray records with the
--    same field names the ArcGIS layers use (Job_Number, weed_species,
--    control_type, gps_accuracy_m...) and the shape as GeoJSON, so the
--    report, map and PDF code work unchanged. security_invoker = the
--    reader's own row-level security applies, exactly as on the tables.
-- 2. invites: add a person by email with a role; they become a member the
--    moment their user is created (Authentication > Users > Add user).

create index if not exists treatment_points_time on public.treatment_points (treated_at);
create index if not exists treatment_lines_time  on public.treatment_lines  (treated_at);
create index if not exists treatment_areas_time  on public.treatment_areas  (treated_at);

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
             st_asgeojson(t.geom, 7)::json                 as geometry
      from public.%I t
      left join public.jobs j            on j.id = t.job_id
      left join public.species s         on s.id = t.species_id
      left join public.control_methods m on m.id = t.method_id
    $f$, v.view_name, v.table_name);
    execute format('grant select on public.%I to authenticated', v.view_name);
    execute format('revoke all on public.%I from anon', v.view_name);
  end loop;
end $$;

-- People waiting to be added -------------------------------------------------

create table if not exists public.invites (
  org_id     uuid not null references public.organisations(id) on delete cascade,
  email      text not null,
  role       public.member_role not null default 'viewer',
  job_ids    uuid[],                       -- viewers only: limit to these jobs
  created_at timestamptz not null default now(),
  primary key (org_id, email)
);
alter table public.invites enable row level security;
drop policy if exists invites_owner on public.invites;
create policy invites_owner on public.invites for all
  using (public.has_role(org_id, 'owner')) with check (public.has_role(org_id, 'owner'));

create or replace function public.accept_invites()
returns trigger language plpgsql security definer set search_path = public as $$
begin
  insert into public.members (org_id, user_id, role, job_ids)
    select i.org_id, new.id, i.role, i.job_ids from public.invites i
    where lower(i.email) = lower(new.email)
  on conflict (org_id, user_id) do nothing;
  delete from public.invites where lower(email) = lower(new.email);
  return new;
end $$;

drop trigger if exists accept_invites on auth.users;
create trigger accept_invites after insert on auth.users
  for each row execute function public.accept_invites();

-- Make sure the owner is a member (if 002 ran before the user existed).
insert into public.members (org_id, user_id, role)
  select o.id, u.id, 'owner'
  from public.organisations o, auth.users u
  where o.name = 'FieldPoint Environmental'
    and lower(u.email) = 'jayme@fieldpointenvironmental.com.au'
on conflict (org_id, user_id) do nothing;

-- To add Calvin (or anyone): put their email here and run this line, then
-- create their user under Authentication > Users > Add user.
--   insert into public.invites (org_id, email, role)
--     select id, 'calvin@example.com', 'editor' from public.organisations
--     where name = 'FieldPoint Environmental';
