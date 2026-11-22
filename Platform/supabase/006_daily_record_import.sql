-- 006: let the importer copy Daily Records (job sheets) and their photos,
-- signatures and issued PDFs from ArcGIS without making duplicates.
-- Safe to run more than once.

-- Which ArcGIS attachment a file came from (null for files added in the app).
alter table public.attachments add column if not exists source_ref text;

-- One row per stored file, so re-running the import updates instead of adding.
do $$
begin
  if not exists (select 1 from pg_constraint where conname = 'attachments_org_path_key') then
    alter table public.attachments add constraint attachments_org_path_key unique (org_id, storage_path);
  end if;
end $$;
