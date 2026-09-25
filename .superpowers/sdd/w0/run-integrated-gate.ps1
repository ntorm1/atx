# W0 integrated correctness qualification. No post-2019 market data is opened.
[CmdletBinding()]
param([switch] $SkipBuild)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
Set-Location -LiteralPath $repo
$wrapper = Join-Path $repo 'scripts/atx-build.ps1'
$logs = Join-Path $repo 'build-equity/w0-integrated-gate'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
$groups = @('alpha', 'factory', 'learn', 'data', 'eval', 'combine', 'risk', 'book')
$targets = @($groups | ForEach-Object { 'atx-engine-' + $_ + '-tests' })
$targets += @('atx-impl-tests', 'atx-shm-worker')
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'

# These opt-ins otherwise permit external-data reads or the multi-hour nightly lane.
$optIns = @('ATX_DATA_DIR', 'ATX_ORATS_ZIP', 'ATX_ALPHA101_PANEL', 'ATX_ALPHA101_FIXTURE',
  'ATX_L10_FUNDZOO_OUT', 'ATX_L10_CONTEXTS', 'ATX_L10_FUND_POINTS',
  'ATX_L10_SURVIVOR_CONTEXTS', 'ATX_NIGHTLY', 'ATX_RISK_NIGHTLY')
$optIns += @('ATX_PNL_CSV', 'ATX_MIN_PRICE', 'ATX_MIN_ADV', 'ATX_ADV_WINDOW')
foreach ($name in $optIns) {
  [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
& git -C $repo rev-parse HEAD | Set-Content (Join-Path $logs 'source-sha.txt')
& git -C $repo status --short | Set-Content (Join-Path $logs 'source-status.txt')
Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256 |
  Format-List | Out-File (Join-Path $logs 'runner-sha256.txt')

if (-not $SkipBuild) {
  $freeGiB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB
  if ($freeGiB -lt 4) { throw "Need 4 GiB free before build; found $freeGiB" }
  & $wrapper configure -Preset equity-dev -Groups ($groups -join ';') `
    "-DFETCHCONTENT_BASE_DIR=$($repo.Replace('\', '/'))/deps/equity-dev" `
    '-DATX_USE_PCH=ON' *> (Join-Path $logs 'configure.log')
  if ($LASTEXITCODE -ne 0) { throw 'Integrated configure failed; see configure.log' }
  & $wrapper build -Preset equity-dev @targets *> (Join-Path $logs 'build.log')
  if ($LASTEXITCODE -ne 0) { throw 'Integrated target build failed; see build.log' }
}

# These fixtures auto-discover the live checkout's 2024/2026 data even without env vars.
# Other DataAdjust tests use hard-coded arithmetic fixtures and remain enabled.
$dataExcluded = @('DataRealPanel.*',
  'DataCorporateActions.LoadsSmokeMasterRowShapeMatchesManifest',
  'DataCorporateActions.DividendZeroFilledOffExDates',
  'DataCorporateActions.SharesOutstandingPitForwardFill',
  'DataCorporateActions.SymbolInterningDeterministic',
  'DataCorporateActions.PartitionedLoaderOrdersBySymbolArgument',
  'DataAdjust.SplitAdjustedNoDiscontinuityAtKnownAaplSplits',
  'OratsE2ESmoke.RealPartitionRunsUnchangedRobustPipeline',
  'OratsE2ESmoke.OperatorOratsZip')
$dataExcluded | Set-Content (Join-Path $logs 'data-exclusions.txt')
'' | Set-Content (Join-Path $logs 'results.txt')
$failures = @()
foreach ($target in $targets) {
  if ($target -eq 'atx-shm-worker') { continue }
  $exe = Join-Path $repo ('build-equity/bin/' + $target + '.exe')
  if (-not (Test-Path -LiteralPath $exe)) { throw "Missing test executable: $exe" }
  Get-FileHash -LiteralPath $exe -Algorithm SHA256 |
    Format-List | Out-File (Join-Path $logs ($target + '.sha256.txt'))
  $testArgs = @('--gtest_brief=1')
  if ($target -eq 'atx-engine-data-tests') {
    $testArgs += '--gtest_filter=-' + ($dataExcluded -join ':')
  }
  # Failure fixtures may intentionally write stderr; judge the executable's exit code.
  $ErrorActionPreference = 'Continue'
  & $exe @testArgs *> (Join-Path $logs ($target + '.log'))
  $testExit = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  "$target exit=$testExit" | Tee-Object -FilePath (Join-Path $logs 'results.txt') -Append
  Get-Content -LiteralPath (Join-Path $logs ($target + '.log')) -Tail 6
  if ($testExit -ne 0) { $failures += $target }
}
if ($failures.Count -gt 0) { throw ('Failed targets: ' + ($failures -join ', ')) }
'PASS: all selected integrated targets; external-data opt-ins and exclusions disclosed.'
