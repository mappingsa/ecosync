@echo off
title EcoSync - stop the hourly import on this PC
rem The import now runs in the cloud every hour (Supabase, 010_cloud_import.sql),
rem so this PC no longer needs to. Import now.bat still works for a manual run.
schtasks /Delete /TN "EcoSync ArcGIS import" /F
if errorlevel 1 (
  echo The hourly task was not found - it may already be off.
) else (
  echo Done. This PC no longer runs the hourly import; the cloud one does.
)
pause
