# Renders the bench project with aerender and reports wall time + peak
# memory of the render worker. Usage:
#   run_bench.ps1 -Aep bench.aep -Out C:\tmp\out_[####].exr -Label before
# The plugin under test must already be in AE's Plug-ins folder; swap it
# between runs. Purge AE's disk cache between runs too, or the second run
# can be served the first run's frames.
param([string]$Aep, [string]$Out, [string]$Label = "run")
$aer = "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\aerender.exe"
$t0 = Get-Date
$p = Start-Process -FilePath $aer -PassThru -NoNewWindow -ArgumentList @(
    "-project", "`"$Aep`"", "-comp", "`"EXRDemux bench`"",
    "-output", "`"$Out`"")
$peak = 0
while (-not $p.HasExited) {
    Get-Process AfterFX* -ErrorAction SilentlyContinue | Where-Object {
        $_.StartTime -ge $t0 } | ForEach-Object {
        if ($_.PeakWorkingSet64 -gt $peak) { $peak = $_.PeakWorkingSet64 } }
    Start-Sleep -Milliseconds 500
}
$s = ((Get-Date) - $t0).TotalSeconds
"{0}: {1:N1} s total, peak worker working set {2:N1} GB" -f $Label, $s, ($peak / 1GB)
