-- 010: run the ArcGIS import in the cloud, every hour, instead of on the PC.
-- Run this AFTER the arcgis-import Edge Function is deployed. Safe to re-run.
--
-- 1. import_runs: one row per run, so you (and the app) can see it worked.
-- 2. A shared secret, made here and kept in Supabase Vault: only pg_cron
--    knows it, so nobody else can start an import.
-- 3. pg_cron calls the function at 7 minutes past every hour (pg_net does the call).
-- 4. run_arcgis_import_now(): start one straight away, from here or the app.

create extension if not exists pg_cron with schema pg_catalog;
create extension if not exists pg_net with schema extensions;
grant usage on schema cron to postgres;

-- 1. The run log -------------------------------------------------------------
create table if not exists public.import_runs (
  id          bigserial primary key,
  org_id      uuid not null references public.organisations(id) on delete cascade,
  started_at  timestamptz not null,
  finished_at timestamptz not null default now(),
  ok          boolean not null,
  summary     text,
  details     jsonb
);
create index if not exists import_runs_recent on public.import_runs (org_id, started_at desc);
alter table public.import_runs enable row level security;
drop policy if exists import_runs_read on public.import_runs;
create policy import_runs_read on public.import_runs for select using (public.has_role(org_id, 'viewer'));

-- 2. The shared secret ---------------------------------------------------------
do $$
begin
  if not exists (select 1 from vault.secrets where name = 'arcgis_import_secret') then
    perform vault.create_secret(
      replace(gen_random_uuid()::text || gen_random_uuid()::text, '-', ''),
      'arcgis_import_secret',
      'pg_cron -> arcgis-import Edge Function');
  end if;
end $$;

create or replace function public.check_import_secret(s text)
returns boolean language sql stable security definer set search_path = '' as $$
  select exists (select 1 from vault.decrypted_secrets where name = 'arcgis_import_secret' and decrypted_secret = s);
$$;
revoke all on function public.check_import_secret(text) from public, anon, authenticated;
grant execute on function public.check_import_secret(text) to service_role;

-- 4. Start a run now (owners and editors from the app; anyone in the SQL Editor)
create or replace function public.run_arcgis_import_now(days integer default 14, full_history boolean default false)
returns bigint language plpgsql security definer set search_path = '' as $$
begin
  if auth.uid() is not null and not exists (
       select 1 from public.members where user_id = auth.uid() and role in ('owner', 'editor')) then
    raise exception 'Only owners and editors can start an import';
  end if;
  return net.http_post(
    url := 'https://ochrxvsglrsbzrbuzpvx.supabase.co/functions/v1/arcgis-import'
           || case when full_history then '?full=1' else '?days=' || greatest(1, least(days, 3650)) end,
    headers := jsonb_build_object(
      'Content-Type', 'application/json',
      'x-import-secret', (select decrypted_secret from vault.decrypted_secrets where name = 'arcgis_import_secret')),
    body := '{}'::jsonb,
    timeout_milliseconds := 150000);
end $$;
revoke all on function public.run_arcgis_import_now(integer, boolean) from public, anon;
grant execute on function public.run_arcgis_import_now(integer, boolean) to authenticated;

-- 3. Every hour, at 7 past ------------------------------------------------------
select cron.unschedule(jobid) from cron.job where jobname in ('arcgis-import', 'import-runs-tidy');
select cron.schedule('arcgis-import', '7 * * * *', $$ select public.run_arcgis_import_now(14, false) $$);
-- keep three months of run history
select cron.schedule('import-runs-tidy', '30 3 * * *', $$ delete from public.import_runs where started_at < now() - interval '90 days' $$);

-- Try it straight away (the result appears in import_runs within a minute):
--   select public.run_arcgis_import_now();
--   select started_at, ok, summary from import_runs order by started_at desc limit 5;
