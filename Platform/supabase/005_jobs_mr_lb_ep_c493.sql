-- EcoSync Data Platform - two jobs found by the first import (22 Nov 2026).
-- Safe to run again. Then re-run the importer with --full.

do $$
declare org uuid;
begin
  select id into org from public.organisations where name = 'FieldPoint Environmental';

  insert into public.jobs (org_id, job_number, client_id, letterhead, title, aliases, active, notes)
  select org, j.job_number, c.id, 'MSA', j.title, j.aliases, j.active, j.notes
  from (values
    ('MR-LB', 'RM', 'Riverland Boxthorn Control', array['Riverland Boxthorn Control'], false,
       'Completed; finished until May 2027'),
    ('EP-C493.3', 'EP', null, array[]::text[], true, null)
  ) as j(job_number, client, title, aliases, active, notes)
  join public.clients c on c.org_id = org and c.code = j.client
  on conflict (org_id, job_number) do nothing;

  delete from public.needs_a_home where org_id = org and field = 'job_number' and value in ('MR-LB', 'EP-C493.3');
end $$;
