$ErrorActionPreference = 'Stop'
$workspace = 'C:\atx'
$stage = 'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_v11'
$sourceStage = 'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_v10'
$evidence = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t5-interruption-inspection-v2.json'
$receipt = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t5-preserved-interruption-v2.json'
if (Test-Path -LiteralPath $receipt) { throw 'Preservation receipt already exists' }
$audit = Get-Content -LiteralPath $evidence -Raw | ConvertFrom-Json
$resolvedStage = (Resolve-Path -LiteralPath $stage).Path.TrimEnd('\')
if ($resolvedStage -ne $stage) { throw 'Unexpected stage path' }
$from = [IO.Path]::GetFullPath((Join-Path $resolvedStage '_work'))
$to = [IO.Path]::GetFullPath((Join-Path $resolvedStage '_interrupted_work-r2-20261003T211353Z'))
foreach ($path in @($from, $to)) {
    if ([IO.Path]::GetDirectoryName($path) -ne $resolvedStage) { throw 'Move escapes intended stage' }
}
foreach ($path in @($resolvedStage, $from)) {
    if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Refuse a reparse-point stage or work directory'
    }
}
if (Test-Path -LiteralPath $to) { throw 'Forensic destination already exists' }
if (@(Get-ChildItem -LiteralPath $stage -File).Count) { throw 'Stage root contains files; reconcile again' }
$writers = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq 'python.exe' -and $_.CommandLine -match 't5-publish|alpha_panel[.]fundamentals(?:\s|$)'
})
if ($writers.Count) { throw 'A fundamental producer may be active' }
foreach ($pin in $audit.code_sha256.PSObject.Properties) {
    $path = [IO.Path]::GetFullPath((Join-Path $workspace $pin.Name))
    if (-not $path.StartsWith($workspace + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Code pin escapes workspace'
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $pin.Value) {
        throw "Code pin changed: $($pin.Name)"
    }
}
if ((Get-FileHash -LiteralPath (Join-Path $sourceStage 'manifest.json') -Algorithm SHA256).Hash -ne $audit.source_manifest_sha256) {
    throw 'Source manifest changed'
}
if ((Get-FileHash -LiteralPath (Join-Path $sourceStage '_work\cf\plan.json') -Algorithm SHA256).Hash -ne $audit.extract_plan_sha256) {
    throw 'Source extraction plan changed'
}
# Both absolute targets were checked above. This preserves every interrupted file.
Move-Item -LiteralPath $from -Destination $to
if ((Test-Path -LiteralPath $from) -or -not (Test-Path -LiteralPath $to)) {
    throw 'Preservation postcondition failed'
}
[ordered]@{
    status = 'PRESERVED_NOT_RESUMED'
    from = $from
    to = $to
    observed_utc = [DateTime]::UtcNow.ToString('o')
    inspection_sha256 = (Get-FileHash -LiteralPath $evidence -Algorithm SHA256).Hash.ToLowerInvariant()
    source_manifest_sha256 = $audit.source_manifest_sha256
    code_pins_match = $true
    old_source_mutated = $false
    acceptance = 'NOT_ESTABLISHED'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receipt -Encoding UTF8
Get-Content -LiteralPath $receipt -Raw
