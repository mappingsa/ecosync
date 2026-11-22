-- 008: the map workspace (reporting app > Map tab). Safe to run more than once.
--
-- report_tracks: the driven track, one line per run, in the same shape as the
-- report_* views from 007 (treated_at = when the run started, so one date
-- filter works for every layer). The reader's own row-level security applies.

create index if not exists tracks_time on public.tracks (started_at);

create or replace view public.report_tracks with (security_invoker = true) as
select t.id, t.org_id, t.job_id,
       t.started_at                                       as treated_at,
       t.ended_at,
       j.job_number                                       as "Job_Number",
       (extract(epoch from t.started_at) * 1000)::bigint  as treatment_time,
       round((st_length(t.geom::geography))::numeric, 1)  as length_m,
       st_asgeojson(t.geom, 7)::json                      as geometry
from public.tracks t
left join public.jobs j on j.id = t.job_id;

grant select on public.report_tracks to authenticated;
revoke all on public.report_tracks from anon;
