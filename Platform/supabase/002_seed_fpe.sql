-- EcoSync Data Platform - starting data for FieldPoint Environmental.
-- Run AFTER 001_schema.sql, and after adding yourself as a user
-- (Authentication > Users > Add user, with your email). Safe to run again:
-- it only adds what is missing.
--
-- Copied from the EcoSync Jobs / Targets / Clients / Lists tables in ArcGIS
-- as of 21 Nov 2026. Add pest species in the app (or below) as they come.

do $$
declare
  owner_email constant text := 'jayme@fieldpointenvironmental.com.au';
  org uuid;
  m_spot uuid; m_foliar uuid; m_cut uuid;
begin
  select id into org from public.organisations where name = 'FieldPoint Environmental';
  if org is null then
    insert into public.organisations (name) values ('FieldPoint Environmental') returning id into org;
  end if;

  insert into public.members (org_id, user_id, role)
    select org, u.id, 'owner' from auth.users u where lower(u.email) = owner_email
    on conflict (org_id, user_id) do nothing;
  if not found and not exists (select 1 from public.members where org_id = org) then
    raise notice 'No user % yet: add it under Authentication > Users, then run this again to become owner.', owner_email;
  end if;

  insert into public.clients (org_id, code, name) values
    (org, 'DEW',  'Department for Environment and Water'),
    (org, 'EP',   'Eyre Peninsula Landscape Board'),
    (org, 'AW',   'Alinytjara Wiluṟara Landscape Board'),
    (org, 'RM',   'Murraylands and Riverland Landscape Board'),
    (org, 'SAAL', 'SA Arid Lands Landscape Board')
  on conflict (org_id, code) do nothing;

  insert into public.control_methods (org_id, name, applies_to) values
    (org, 'Spot Spray', 'weed'), (org, 'Foliar spray', 'weed'), (org, 'Cut and swab', 'weed'),
    (org, 'Drill and fill', 'weed'), (org, 'Boom spray', 'weed'), (org, 'Slashing', 'weed'),
    (org, 'Hand removal', 'weed'),
    (org, 'Baiting', 'pest'), (org, 'Trapping', 'pest'), (org, 'Observation', 'both')
  on conflict (org_id, name) do nothing;
  select id into m_spot   from public.control_methods where org_id = org and name = 'Spot Spray';
  select id into m_foliar from public.control_methods where org_id = org and name = 'Foliar spray';
  select id into m_cut    from public.control_methods where org_id = org and name = 'Cut and swab';

  insert into public.species (org_id, name, kind, scientific_name, default_method_id, colour) values
    (org, 'African Boxthorn', 'weed', 'Lycium ferocissimum',      m_cut,  '#63D91E'),
    (org, 'Boneseed',         'weed', 'Chrysanthemoides monilifera', m_cut, '#E9ED09'),
    (org, 'Buffel Grass',     'weed', 'Cenchrus ciliaris',        m_spot, '#188C08'),
    (org, 'Gazania',          'weed', 'Gazania spp.',             m_spot, '#D45324'),
    (org, 'Golden Dodder',    'weed', 'Cuscuta campestris',       m_spot, '#E2ED07'),
    (org, 'Noogoora Burr',    'weed', 'Xanthium strumarium',      m_spot, '#25E80C')
  on conflict (org_id, name) do nothing;

  -- Sprayed Boxthorn draws red; cut and swab uses the species colour (green).
  insert into public.species_method_colours (species_id, method_id, org_id, colour)
    select s.id, m.id, org, '#E31A1C'
    from public.species s, public.control_methods m
    where s.org_id = org and s.name = 'African Boxthorn'
      and m.org_id = org and m.name in ('Spot Spray', 'Foliar spray')
  on conflict do nothing;

  insert into public.jobs (org_id, job_number, client_id, letterhead, title, property_address,
                           default_equipment, default_chemicals, aliases, notes)
  select org, j.job_number, c.id, j.letterhead, j.title, j.address, j.equipment, j.chemicals, j.aliases, j.notes
  from (values
    ('DEW-NP',    'DEW', 'MSA', 'DEW National Parks weed control', null, null, null,
       array['DEW NP Weed Control'], null),
    ('EP-C489.2', 'EP',  'MSA', 'Buffel Grass control', 'Iron Knob Rd', 'Backpack Sprayer',
       'Glyphosate 450; Brushwet/Consume; Pine oil', array[]::text[], null),
    ('EP-C489.5', 'EP',  'MSA', 'African Boxthorn control', null, 'Cut and swab', null,
       array['Works Order C489.5'], 'Used the Boxthorn report template'),
    ('AW-2628',   'AW',  'MSA', 'Buffel Grass control', null, null, null,
       array['AW-LW', 'AW Landscape'], null)
  ) as j(job_number, client, letterhead, title, address, equipment, chemicals, aliases, notes)
  join public.clients c on c.org_id = org and c.code = j.client
  on conflict (org_id, job_number) do nothing;

  insert into public.list_values (org_id, list, value)
  select org, v.list, v.value from (values
    ('operator', 'Jayme Peltz (0429844652)'), ('operator', 'Calvin Mackereth (0493342879)'),
    ('operator', 'Justin Dixon (0429844622)'),
    ('equipment', 'Vehicle Mounted Spot Sprayer'), ('equipment', 'Vehicle Mounted Foliar Sprayer'),
    ('equipment', 'Backpack Sprayer'), ('equipment', 'Cut and swab'), ('equipment', 'Drill and Fill'),
    ('equipment', 'Boom Spray'), ('equipment', 'Graslan Applicator'), ('equipment', 'Slasher'),
    ('equipment', 'Brushcutter'),
    ('chemical', 'Glyphosate xtraquatic 450'), ('chemical', 'Glyphosate 450'), ('chemical', 'Fluproponate'),
    ('chemical', 'Garlon Triclopyr'), ('chemical', 'Metsulfuron Methyl'), ('chemical', 'SP700 Surfactant'),
    ('chemical', 'Pine oil'), ('chemical', 'Nemo Surfactant'), ('chemical', 'Brushwet/Consume'),
    ('chemical', 'Vigilant'), ('chemical', 'Graslan'), ('chemical', 'Lontrel'),
    ('chemical', 'Starane Advanced'), ('chemical', 'Uptake oil'), ('chemical', 'Wilt 700 oil'),
    ('chemical', 'Sulfomac'), ('chemical', 'Gauntlet 720 (MSMA)')
  ) as v(list, value)
  on conflict (org_id, list, value) do nothing;

  raise notice 'FieldPoint Environmental organisation: %', org;
end $$;
