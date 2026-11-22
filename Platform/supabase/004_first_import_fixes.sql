-- EcoSync Data Platform - fixes from the first full ArcGIS import (22 Nov 2026).
-- Run after 001-003. Safe to run again. Then re-run the importer with --full
-- so the existing records pick up the matches.

-- Control methods get aliases, like species and jobs.
alter table public.control_methods add column if not exists aliases text[] not null default '{}';

do $$
declare org uuid;
begin
  select id into org from public.organisations where name = 'FieldPoint Environmental';

  -- Job spelling on the unit
  update public.jobs set aliases = array(select distinct unnest(aliases || array['C489.5']))
   where org_id = org and job_number = 'EP-C489.5';

  -- Species spellings on the unit
  update public.species set aliases = array(select distinct unnest(aliases || array['Gazanias']))
   where org_id = org and name = 'Gazania';
  update public.species set aliases = array(select distinct unnest(aliases || array['African Boxthor']))
   where org_id = org and name = 'African Boxthorn';

  insert into public.species (org_id, name, kind, scientific_name, default_method_id, colour, aliases)
  select org, 'Californian Burr', 'weed', 'Xanthium orientale',
         (select id from public.control_methods where org_id = org and name = 'Spot Spray'),
         '#8E44AD', array['California Burr']
  on conflict (org_id, name) do nothing;

  -- Method spellings on the unit / old Survey123 options
  update public.control_methods set aliases = array(select distinct unnest(aliases || array['Hand pull']))
   where org_id = org and name = 'Hand removal';
  update public.control_methods set aliases = array(select distinct unnest(aliases || array['Cut and Swab or Manual Removal']))
   where org_id = org and name = 'Cut and swab';

  -- Fixed values no longer need a home
  delete from public.needs_a_home
   where org_id = org and value in ('C489.5', 'Gazanias', 'African Boxthor', 'California Burr',
                                    'Hand pull', 'Cut and Swab or Manual Removal');
end $$;
