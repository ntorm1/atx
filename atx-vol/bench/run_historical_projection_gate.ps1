param(
    [string]$BuildDir = "build-rel",
    [string]$Output = "historical-projection-new.json"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$executable = Join-Path $repo "$BuildDir\bin\atx-vol-projection-bench.exe"
$baseline = Join-Path $PSScriptRoot "baselines\i7-1260p-clang18-sse2-historical-projection.json"
$comparator = Join-Path $PSScriptRoot "compare_baseline.py"
$outputPath = if ([System.IO.Path]::IsPathRooted($Output)) {
    [System.IO.Path]::GetFullPath($Output)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repo $Output))
}

if (-not (Test-Path -LiteralPath $executable)) {
    throw "projection benchmark not found: $executable"
}

$arguments = @(
    "--benchmark_filter=projection/historical/scenarios61/legs102/t8/.*",
    "--benchmark_out=$outputPath",
    "--benchmark_out_format=json"
)
$process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru -WindowStyle Hidden
try {
    # Pinned i7-1260P gate: keep the eight benchmark workers on one stable
    # logical-core set and avoid hybrid-core migration during wall-time sampling.
    $process.ProcessorAffinity = [IntPtr]0xFF
    $process.PriorityClass = "High"
} catch {
    $process.Kill()
    throw "cannot apply benchmark affinity/priority: $($_.Exception.Message)"
}
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
    throw "historical projection benchmark failed with exit code $($process.ExitCode)"
}

& python $comparator $baseline $outputPath --threshold 0.30 --cv-max 0.15
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
