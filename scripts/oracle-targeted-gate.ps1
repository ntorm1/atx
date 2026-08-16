# Fixed small-gate adapter for oracle bootstrap verification.
# It runs only worktree-local binaries, consumes the real OracleBench scorecard,
# and emits one closed aggregate-only typed JSON receipt.
[CmdletBinding()]
param(
  [ValidateSet('mode_a_targeted_tests', 'mode_a_smoke', 'convention_tests', 'mode_a_smoke_tune', 'residual_floor', 'convention_speed_measure', 'convention_speed', 'mode_b_targeted_tests', 'mode_b_smoke_tune', 'sprint_american_greeks_delta_put', 'sprint_adjusted_greeks_flat_smile')]
  [string]$Gate
)

$ErrorActionPreference = 'Stop'
$script:OracleRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:OracleStoreRoot = 'C:\atx-cache\oracle\spiderrock'
$script:OracleBenchTestIds = @(
  'OracleBenchBands.MoneynessCallEdgesAreHalfOpen',
  'OracleBenchBands.MoneynessPutMirrorsCall',
  'OracleBenchBands.DteEdgesBelongToTheLowerBand',
  'OracleBenchBands.BandTokensMatchTheCharter',
  'OracleBenchTolerance.PriceTickFloorWins',
  'OracleBenchTolerance.PriceSpreadFractionWins',
  'OracleBenchTolerance.PriceCrossedMarketDegradesToTick',
  'OracleBenchTolerance.VolUsesFiveBpAbsolute',
  'OracleBenchTolerance.GreekRelativeWithAbsoluteFloor',
  'OracleBenchScorecard.CellKeyMatchesCharterFormat',
  'OracleBenchScorecard.PercentilesAreNearestRank',
  'OracleBenchScorecard.WithinTolAccountingAndStats',
  'OracleBenchScorecard.UnknownCellIsNotFound',
  'OracleBenchScorecard.JsonCarriesHeaderModesTolerancesAndCells',
  'OracleBenchCohort.ParsesTheReadmeSchema',
  'OracleBenchCohort.ToleratesUnknownScalarKeys',
  'OracleBenchCohort.RejectsMissingRequiredKey',
  'OracleBenchCohort.RejectsWrongTypeForDates',
  'OracleBenchCohort.RejectsMalformedDate',
  'OracleBenchCohort.RejectsMalformedBucket',
  'OracleBenchCohort.RejectsEmptyUnderliers',
  'OracleBenchCohort.RejectsMalformedJson',
  'OracleBenchArgs.ParsesAllFlags',
  'OracleBenchArgs.DefaultsIterZeroShaUnknown',
  'OracleBenchArgs.RejectsMissingRequiredFlag',
  'OracleBenchArgs.RejectsUnknownFlag',
  'OracleBenchArgs.RejectsNonIntegerIter',
  'OracleBenchReader.OpensOnlyCohortNamedPartitionsAndFiltersUnderlier',
  'OracleBenchReader.CrossesUnderliersAndBuckets',
  'OracleBenchReader.MissingPartitionDirIsNotFound',
  'OracleBenchE2E.SyntheticCohortProducesCharterScorecard'
)
# Pinned exactly like the OracleBench registry above: the Stage 3 suite is
# discovered per gtest case, so a vanished or renamed case fails the gate
# instead of passing a filter that matched nothing.
$script:OracleConventionTestIds = @(
  'OracleConvention.DiscreteDividendForwardIsAppliedExactly',
  'OracleConvention.ProductionMapIsTheResolvedHardCut',
  'OracleConvention.BestScaleRanksOnTheSelectionObjective',
  'OracleConvention.SymmetricObjectiveHasNoSmallestScaleGradient',
  'OracleConvention.FinalistRankPrefersNoGreekRegressionOverLowerPriceMae',
  'OracleConvention.BestScaleTieBreaksOnSourceThenNumericScale',
  'OracleConvention.BestScaleWithoutSelectionEvidenceUsesCandidateIdentity',
  'OracleConvention.CompleteMapNamesEveryGreekSignAndScale',
  'OracleConvention.ThetaDayCountNeverRebucketsDteBands',
  'OracleConvention.SweepIsClosedDeterministicAndCoversElevenMetrics',
  'OracleConvention.CandidateAndBaselineFloorsShareOneRowPopulation',
  'OracleConvention.SubFloorOracleRowsBothReportAndSelect',
  'OracleConvention.SweepPublishesTheSelectedInputModelGreekRegressions',
  'OracleConvention.SweepJsonPublishesTheProductionMapBesideTheWinner',
  'OracleConvention.SweepRefusesAMetricNoRowObserved',
  'OracleConvention.SweepRejectsEmptyCohort'
)
$script:ModeAMetricMap = [ordered]@{
  price = 'mode_a_price_mae'
  vol = 'mode_a_vol_mae'
  de = 'mode_a_delta_rel'
  ga = 'mode_a_gamma_rel'
  th = 'mode_a_theta_rel'
  ve = 'mode_a_vega_rel'
  rh = 'mode_a_rho_rel'
  ph = 'mode_a_phi_rel'
  vo = 'mode_a_volga_rel'
  va = 'mode_a_vanna_rel'
  deDecay = 'mode_a_delta_decay_rel'
}

function Get-OracleGitIdentity {
  $sha = (& git -C $script:OracleRepoRoot rev-parse --verify HEAD 2>$null | Out-String).Trim().ToLowerInvariant()
  if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-f]{40}$') { throw 'oracle targeted gate cannot resolve worktree HEAD' }
  $tree = (& git -C $script:OracleRepoRoot rev-parse --verify 'HEAD^{tree}' 2>$null | Out-String).Trim().ToLowerInvariant()
  if ($LASTEXITCODE -ne 0 -or $tree -notmatch '^[0-9a-f]{40}$') { throw 'oracle targeted gate cannot resolve worktree tree' }
  & git -C $script:OracleRepoRoot diff --no-ext-diff --quiet -- 2>$null
  if ($LASTEXITCODE -ne 0) { throw 'oracle targeted gate refuses tracked worktree changes' }
  & git -C $script:OracleRepoRoot diff --cached --no-ext-diff --quiet -- 2>$null
  if ($LASTEXITCODE -ne 0) { throw 'oracle targeted gate refuses staged worktree changes' }
  $untracked = @(& git -C $script:OracleRepoRoot ls-files --others --exclude-standard -- .claude atx-vol scripts 2>$null)
  if ($LASTEXITCODE -ne 0 -or $untracked.Count) { throw 'oracle targeted gate refuses untracked source files' }
  return [pscustomobject]@{ Sha = $sha; Tree = $tree }
}

function Get-OracleTargetedGateSpec([string]$GateId, $Identity) {
  if (-not $Identity) { $Identity = Get-OracleGitIdentity }
  $buildScript = Join-Path $script:OracleRepoRoot 'scripts\atx-build.ps1'
  $testExe = Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-tests.exe'
  $benchExe = Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-oracle-bench.exe'
  $conventionTestExe = Join-Path $script:OracleRepoRoot 'build\bin\atx-vol-oracle-convention-tests.exe'
  $relBenchExe = Join-Path $script:OracleRepoRoot 'build-rel-avx2\bin\atx-vol-oracle-bench.exe'
  $smokeCohort = Join-Path $script:OracleRepoRoot 'atx-vol\bench\oracle\cohorts\smoke.json'
  $tuneCohort = Join-Path $script:OracleRepoRoot 'atx-vol\bench\oracle\cohorts\tune.json'
  $floorPath = Join-Path $script:OracleRepoRoot 'atx-vol\bench\oracle\scorecards\iter-000.json'
  $outputRoot = Join-Path $script:OracleRepoRoot 'build\oracle-gates'
  switch ($GateId) {
    'mode_a_targeted_tests' {
      return [pscustomobject]@{
        Kind = 'ctest'; Program = 'powershell'; OutputPath = ''
        RequiredExecutables = @($testExe, $benchExe)
        ExpectedTestIds = @($script:OracleBenchTestIds)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', 'build', 'atx-vol-tests', 'atx-vol-oracle-bench', '--parallel', '2')
        Arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^OracleBench.*$', '--no-tests=error')
      }
    }
    'mode_a_smoke' {
      $out = Join-Path $outputRoot ('mode-a-smoke-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_bench'; Program = $benchExe; OutputPath = $out
        RequiredExecutables = @($benchExe)
        Arguments = @('--cohort', $smokeCohort, '--store', $script:OracleStoreRoot, '--out', $out, '--iter', '0', '--git-sha', $Identity.Sha)
      }
    }
    'convention_tests' {
      return [pscustomobject]@{
        Kind = 'ctest'; Program = 'powershell'; OutputPath = ''
        RequiredExecutables = @($conventionTestExe)
        ExpectedTestIds = @($script:OracleConventionTestIds)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', 'build', 'atx-vol-oracle-convention-tests', '--parallel', '2')
        Arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^OracleConvention\.', '--no-tests=error')
      }
    }
    'mode_a_smoke_tune' {
      $out = Join-Path $outputRoot ('mode-a-smoke-tune-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_convention'; Program = $benchExe; OutputPath = $out
        RequiredExecutables = @($benchExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', 'build', 'atx-vol-oracle-bench', '--parallel', '2')
        Arguments = @('--convention-sweep', '--smoke', $smokeCohort, '--tune', $tuneCohort, '--store', $script:OracleStoreRoot, '--out', $out, '--git-sha', $Identity.Sha)
      }
    }
    'residual_floor' {
      # Deliberately verify the exact-SHA artifact emitted by the immediately
      # preceding smoke+tune gate. Repricing the same 277k aggregate rows here
      # added minutes without adding independent evidence.
      $out = Join-Path $outputRoot ('mode-a-smoke-tune-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_floor_verify'; Program = ''; OutputPath = $out
        ExpectedFloorPath = $floorPath; RequiredExecutables = @(); Arguments = @()
      }
    }
    'convention_speed_measure' {
      # The only sanctioned producer of a rel-avx2 rows_per_second number, and
      # therefore the only thing that can run BEFORE iter-000 exists. It pins
      # nothing. iter-000's speed floor is DERIVED from this receipt, not copied
      # from it: baseline = the measured rows_per_second, and
      #   pin = floor(baseline * 0.90)
      # convention_speed then re-measures on a quiet host and requires
      # rows_per_second >= pin. A verbatim copy (pin == baseline) would make that
      # a coin flip on run-to-run noise, so the 10% margin is part of the
      # contract and Test-SpeedFloor rejects any pin above baseline * 0.95.
      $out = Join-Path $outputRoot ('convention-speed-measure-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_speed'; Program = $relBenchExe; OutputPath = $out
        ExpectedFloorPath = ''; RequiredExecutables = @($relBenchExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'rel-avx2', 'build', 'atx-vol-oracle-bench', '--parallel', '2')
        Arguments = @('--cohort', $tuneCohort, '--store', $script:OracleStoreRoot, '--out', $out, '--iter', '0', '--git-sha', $Identity.Sha)
      }
    }
    'convention_speed' {
      $out = Join-Path $outputRoot ('convention-speed-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_speed'; Program = $relBenchExe; OutputPath = $out
        ExpectedFloorPath = $floorPath; RequiredExecutables = @($relBenchExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'rel-avx2', 'build', 'atx-vol-oracle-bench', '--parallel', '2')
        Arguments = @('--cohort', $tuneCohort, '--store', $script:OracleStoreRoot, '--out', $out, '--iter', '0', '--git-sha', $Identity.Sha)
      }
    }
    'mode_b_targeted_tests' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_b_targeted_tests$', '--no-tests=error') } }
    'mode_b_smoke_tune' { return [pscustomobject]@{ Kind = 'oracle_bench'; Program = 'atx-vol-oracle-bench'; OutputPath = ''; RequiredExecutables = @(); Arguments = @('--cohort', 'smoke,tune', '--mode', 'B', '--aggregate-only') } }
    'sprint_american_greeks_delta_put' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^AmericanGreeks.Delta_MatchesFd_Put$', '--no-tests=error') } }
    'sprint_adjusted_greeks_flat_smile' { return [pscustomobject]@{ Kind = 'ctest'; Program = 'powershell'; OutputPath = ''; RequiredExecutables = @($testExe); Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^AdjustedGreeks.FlatSmileLeavesDeltaUnchanged$', '--no-tests=error') } }
    default { throw "unknown oracle targeted gate: $GateId" }
  }
}

function Get-OracleRequiredMetricIds([string]$GateId) {
  if ($GateId -in @('mode_a_smoke', 'mode_a_smoke_tune', 'residual_floor')) {
    return @($script:ModeAMetricMap.Values | ForEach-Object { [string]$_ })
  }
  if ($GateId -in @('convention_speed_measure', 'convention_speed')) { return @('rel_avx2_rows_per_second') }
  if ($GateId -eq 'mode_b_smoke_tune') {
    return @('mode_b_price_mae', 'mode_b_vol_mae', 'mode_b_delta_rel', 'mode_b_gamma_rel', 'mode_b_theta_rel', 'mode_b_vega_rel', 'mode_b_rho_rel', 'mode_b_phi_rel', 'mode_b_volga_rel', 'mode_b_vanna_rel', 'mode_b_delta_decay_rel')
  }
  return @()
}

function Test-OracleExactStringSet($Values, [string[]]$Expected) {
  $actual = @($Values | ForEach-Object { [string]$_ })
  return $actual.Count -eq $Expected.Count -and @($actual | Select-Object -Unique).Count -eq $Expected.Count -and -not (Compare-Object ($actual | Sort-Object) ($Expected | Sort-Object))
}

function Test-OracleExactKeys($Value, [string[]]$Expected) {
  if (-not $Value) { return $false }
  return Test-OracleExactStringSet @($Value.PSObject.Properties.Name) $Expected
}

function Test-OracleNonnegativeInteger($Value) {
  if ($null -eq $Value) { return $false }
  $number = 0L
  return [long]::TryParse(([string]$Value), [ref]$number) -and $number -ge 0 -and [double]$Value -eq [double]$number
}

function Test-OracleFiniteNumber($Value) {
  if ($null -eq $Value) { return $false }
  $number = 0.0
  return [double]::TryParse(([string]$Value), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)
}

function Get-OracleTextSha256([string]$Text) {
  $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($Text)
  try {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
  } finally { if ($sha) { $sha.Dispose() } }
}

function Assert-OracleGateExecutables($Spec) {
  foreach ($path in @($Spec.RequiredExecutables)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw ('oracle targeted gate executable missing: ' + $path) }
  }
}

function Invoke-OracleNativeProcess([string]$Program, [string[]]$Arguments) {
  # Windows PowerShell 5 surfaces a native program's ordinary stderr as
  # NativeCommandError records. OracleBench intentionally reports progress on
  # stderr, so capture those records without letting the script-wide Stop
  # policy abort a successful native process.
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'Continue'
    $lines = @(& $Program @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $exitCode = $LASTEXITCODE
  } finally { $ErrorActionPreference = $savedPreference }
  return [pscustomobject]@{ ExitCode = [int]$exitCode; Lines = $lines }
}

function Assert-OracleQuietHost {
  $busyNames = @('clang-cl', 'cl', 'link', 'lld-link', 'ninja', 'msbuild', 'atx-vol-oracle-bench')
  $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $busyNames -contains $_.ProcessName })
  if ($busy.Count) { throw ('quiet rel-avx2 gate found competing process(es): ' + (($busy.ProcessName | Sort-Object -Unique) -join ',')) }
}

function Get-OracleGreekMetricIds {
  return @($script:ModeAMetricMap.Values | ForEach-Object { [string]$_ } |
    Where-Object { $_ -ne 'mode_a_price_mae' -and $_ -ne 'mode_a_vol_mae' })
}

# HARD no-regression gate. No REPORTED metric may be worse than its baseline;
# equality is allowed because mode_a_vol_mae is structurally 0 on both arms.
# There is deliberately no bypass flag, no allowlist and no tolerance fudge: a
# convention map that makes a published number worse than doing nothing is not a
# candidate, and hiding that behind a knob is how the delta_decay regression
# survived a full gate run.
function Get-OracleMetricRegressions($Metrics, $BaselineMetrics) {
  $baselineById = @{}
  foreach ($metric in @($BaselineMetrics)) { $baselineById[[string]$metric.metric_id] = $metric }
  $offenders = @()
  $invariant = [Globalization.CultureInfo]::InvariantCulture
  foreach ($metric in @($Metrics)) {
    $id = [string]$metric.metric_id
    $baseline = $baselineById[$id]
    if (-not $baseline) { $offenders += ($id + ' has no baseline metric'); continue }
    $candidateValue = [double]$metric.value
    $baselineValue = [double]$baseline.value
    if ($candidateValue -gt $baselineValue) {
      $offenders += ($id + ' candidate=' + $candidateValue.ToString('R', $invariant) + ' baseline=' + $baselineValue.ToString('R', $invariant))
    }
  }
  return @($offenders)
}

function Test-OracleMetricArray($Metrics) {
  $items = @($Metrics)
  $expected = @($script:ModeAMetricMap.Values | ForEach-Object { [string]$_ })
  if ($items.Count -ne $expected.Count -or -not (Test-OracleExactStringSet @($items.metric_id) $expected)) { return $false }
  foreach ($metric in $items) {
    # selection_count now equals count (the symmetric selection objective needs
    # no sub-floor exclusion), but the field stays pinned and the ratio stays
    # checked: an objective that ever narrowed the selection population again
    # would have to move this published number instead of doing it silently.
    if (-not (Test-OracleExactKeys $metric @('metric_id', 'value', 'count', 'selection_count', 'unit')) -or
        -not (Test-OracleFiniteNumber $metric.value) -or -not (Test-OracleNonnegativeInteger $metric.count) -or [long]$metric.count -le 0 -or
        -not (Test-OracleNonnegativeInteger $metric.selection_count) -or [long]$metric.selection_count -le 0 -or
        [long]$metric.selection_count -gt [long]$metric.count -or
        (10L * [long]$metric.selection_count) -lt [long]$metric.count) { return $false }
    $wantedUnit = if ($metric.metric_id -eq 'mode_a_price_mae') { 'ticks' } elseif ($metric.metric_id -eq 'mode_a_vol_mae') { 'bp' } else { 'relative' }
    if ($metric.unit -ne $wantedUnit -or [double]$metric.value -lt 0) { return $false }
  }
  return $true
}

# The candidate and baseline floors must describe ONE row population per metric.
# Without this, a row abandoned by only one arm turns metric_deltas into a
# comparison of two different samples, biased toward whichever arm kept it.
function Test-OracleMetricPopulationParity($Metrics, $BaselineMetrics) {
  $baselineById = @{}
  foreach ($metric in @($BaselineMetrics)) { $baselineById[[string]$metric.metric_id] = $metric }
  foreach ($metric in @($Metrics)) {
    $baseline = $baselineById[[string]$metric.metric_id]
    if (-not $baseline -or [long]$metric.count -ne [long]$baseline.count -or
        [long]$metric.selection_count -ne [long]$baseline.selection_count) { return $false }
  }
  return $true
}

# Value domains are the SAME closed enums oracle-capability.ps1 enforces on the
# committed receipt. They are duplicated here deliberately: a targeted gate that
# validated fewer domains would pass a map the capability probe then rejects,
# i.e. AFTER the commit, which is the one place the failure cannot be undone.
function Test-OracleConventionMap($Map) {
  $keys = @('input_model', 'forward_formula', 'rate_model', 'carry_model', 'dividend_model', 'day_count', 'dte_banding_day_count', 'price_scale', 'price_sign', 'vol_scale', 'delta_scale', 'delta_sign', 'gamma_scale', 'gamma_sign', 'theta_basis', 'theta_sign', 'vega_scale', 'vega_sign', 'rho_scale', 'rho_sign', 'phi_scale', 'phi_sign', 'volga_source', 'volga_scale', 'volga_sign', 'vanna_source', 'vanna_scale', 'vanna_sign', 'delta_decay_basis', 'delta_decay_day_count', 'delta_decay_sign')
  if (-not (Test-OracleExactKeys $Map $keys)) { return $false }
  foreach ($key in $keys) { if (-not ($Map.$key -is [string]) -or -not $Map.$key) { return $false } }
  $inputModels = @('uprc_spot__rate__sdiv_yield', 'discrete_forward_pv__rate__sdiv_yield', 'discrete_forward_net_carry__rate__sdiv_yield', 'discrete_forward__rate__sdiv_yield', 'discrete_forward__rate_minus_sdiv__zero_carry', 'discrete_forward__zero_rate__zero_carry', 'discrete_forward_pv__rate_minus_sdiv__zero_carry', 'discrete_forward_pv__rate_plus_sdiv__zero_carry')
  $dayCounts = @('ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252')
  if ($inputModels -notcontains $Map.input_model -or
      @('none', 'uprc_exp_rate_t_minus_ddiv') -notcontains $Map.forward_formula -or
      @('continuous_row_rate', 'continuous_rate_minus_sdiv', 'continuous_rate_plus_sdiv', 'zero') -notcontains $Map.rate_model -or
      @('sdiv_as_yield', 'zero') -notcontains $Map.carry_model -or
      @('continuous_yield_only', 'discrete_cash_forward') -notcontains $Map.dividend_model -or
      $dayCounts -notcontains $Map.day_count -or $dayCounts -notcontains $Map.dte_banding_day_count -or
      $dayCounts -notcontains $Map.delta_decay_day_count -or
      @('per_share', 'per_contract_100', 'per_share_from_contract') -notcontains $Map.price_scale -or
      @('positive', 'negative') -notcontains $Map.price_sign -or
      @('decimal_identity') -notcontains $Map.vol_scale -or
      @('per_day', 'per_year') -notcontains $Map.theta_basis -or @('per_day', 'per_year') -notcontains $Map.delta_decay_basis -or
      @('volga', 'vanna') -notcontains $Map.volga_source -or @('volga', 'vanna') -notcontains $Map.vanna_source) { return $false }
  foreach ($name in @('delta_scale', 'gamma_scale', 'vega_scale', 'rho_scale', 'phi_scale', 'volga_scale', 'vanna_scale')) {
    if (@('per_unit', 'per_point', 'per_point_squared', 'per_contract_100') -notcontains $Map.$name) { return $false }
  }
  foreach ($name in @('delta_sign', 'gamma_sign', 'theta_sign', 'vega_sign', 'rho_sign', 'phi_sign', 'volga_sign', 'vanna_sign', 'delta_decay_sign')) {
    if (@('positive', 'negative') -notcontains $Map.$name) { return $false }
  }
  return $true
}

# Committed receipts are compared FIELD BY FIELD, with numbers compared by
# VALUE. Windows PowerShell 5.1's ConvertFrom-Json parses JSON numbers into
# System.Decimal and ConvertTo-Json re-emits the SOURCE DIGITS, so comparing two
# re-serialized documents as text made an authored `0.0` differ from the sweep's
# `%.17g` rendering of the same number (`0`) — a byte comparison of digits
# masquerading as a value comparison.
function Test-OracleJsonValueEqual($Left, $Right) {
  if ($null -eq $Left -or $null -eq $Right) { return ($null -eq $Left) -and ($null -eq $Right) }
  if ($Left -is [bool] -or $Right -is [bool]) { return ($Left -is [bool]) -and ($Right -is [bool]) -and ([bool]$Left -eq [bool]$Right) }
  if ($Left -is [string] -or $Right -is [string]) { return ($Left -is [string]) -and ($Right -is [string]) -and ([string]$Left -ceq [string]$Right) }
  if ($Left -is [array] -or $Right -is [array]) {
    if (-not ($Left -is [array]) -or -not ($Right -is [array]) -or $Left.Count -ne $Right.Count) { return $false }
    for ($index = 0; $index -lt $Left.Count; $index++) {
      if (-not (Test-OracleJsonValueEqual $Left[$index] $Right[$index])) { return $false }
    }
    return $true
  }
  if ($Left -is [System.Management.Automation.PSCustomObject] -or $Right -is [System.Management.Automation.PSCustomObject]) {
    if (-not ($Left -is [System.Management.Automation.PSCustomObject]) -or -not ($Right -is [System.Management.Automation.PSCustomObject])) { return $false }
    $leftNames = @($Left.PSObject.Properties.Name)
    if (-not (Test-OracleExactStringSet $leftNames @($Right.PSObject.Properties.Name))) { return $false }
    foreach ($name in $leftNames) {
      if (-not (Test-OracleJsonValueEqual $Left.$name $Right.$name)) { return $false }
    }
    return $true
  }
  return (Test-OracleFiniteNumber $Left) -and (Test-OracleFiniteNumber $Right) -and ([double]$Left -eq [double]$Right)
}

function ConvertFrom-OracleConventionSweep([string]$ScorecardText, [string]$GateId, $Identity, [string]$ExpectedFloorPath) {
  try { $sweep = $ScorecardText | ConvertFrom-Json } catch { throw "oracle targeted gate $GateId sweep is not JSON" }
  $keys = @('schema_version', 'kind', 'git_sha', 'cohorts', 'selection_strategy', 'smoke_rows', 'tune_rows', 'rows_priced', 'engine_errors', 'baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status', 'diagnostic_speed')
  if (-not (Test-OracleExactKeys $sweep $keys) -or $sweep.schema_version -ne 2 -or $sweep.kind -ne 'convention_sweep' -or
      $sweep.git_sha -ne $Identity.Sha -or -not (Test-OracleExactStringSet @($sweep.cohorts) @('smoke', 'tune')) -or
      -not (Test-OracleNonnegativeInteger $sweep.smoke_rows) -or [long]$sweep.smoke_rows -le 0 -or
      -not (Test-OracleNonnegativeInteger $sweep.tune_rows) -or [long]$sweep.tune_rows -le 0 -or
      -not (Test-OracleNonnegativeInteger $sweep.rows_priced) -or [long]$sweep.rows_priced -le 0 -or
      -not (Test-OracleNonnegativeInteger $sweep.engine_errors) -or -not (Test-OracleMetricArray $sweep.metrics) -or
      -not (Test-OracleMetricArray $sweep.baseline_metrics) -or -not (Test-OracleConventionMap $sweep.conventions) -or
      -not (Test-OracleConventionMap $sweep.baseline_conventions) -or -not (Test-OracleConventionMap $sweep.production_conventions) -or
      -not (Test-OracleMetricPopulationParity $sweep.metrics $sweep.baseline_metrics) -or
      @($sweep.oracle_suspect_candidates).Count -ne 0 -or
      $sweep.market_evidence_status -ne 'not_evaluated_no_nbbo_gate') { throw "oracle targeted gate $GateId sweep schema mismatch" }
  # Row accounting closes by construction in the sweep, but nothing asserted it,
  # so a run that failed 99% of its rows in the engine still reported PASS on the
  # 1% it priced.
  if (([long]$sweep.smoke_rows + [long]$sweep.tune_rows) -ne ([long]$sweep.rows_priced + [long]$sweep.engine_errors)) {
    throw "oracle targeted gate $GateId sweep row accounting does not close: smoke_rows+tune_rows != rows_priced+engine_errors"
  }
  # Fail closed while the pinned production map differs from what the sweep
  # resolved: otherwise a committed floor can describe a map production never
  # prices with.
  if (-not (Test-OracleJsonValueEqual $sweep.production_conventions $sweep.conventions)) {
    throw "oracle targeted gate $GateId production convention map differs from the resolved sweep winner"
  }
  # HARD gate: fail closed on ANY reported metric worse than its baseline, and
  # name every offender with both values so the failure is diagnosable without
  # re-running a 12-minute sweep.
  $regressions = @(Get-OracleMetricRegressions $sweep.metrics $sweep.baseline_metrics)
  if ($regressions.Count) {
    throw ("oracle targeted gate $GateId candidate is worse than baseline on " + $regressions.Count +
           ' metric(s): ' + ($regressions -join '; '))
  }
  # Greeks the SELECTED input model still regresses on versus baseline on the
  # tune sample. Non-empty only when BOTH finalists regressed and the
  # lexicographic rank fell through to price MAE — published, never silent.
  $regressedGreeks = @($sweep.input_model_regressed_greeks | ForEach-Object { [string]$_ })
  $greekIds = Get-OracleGreekMetricIds
  if (@($regressedGreeks | Where-Object { $greekIds -notcontains $_ }).Count -or
      @($regressedGreeks | Select-Object -Unique).Count -ne $regressedGreeks.Count) {
    throw "oracle targeted gate $GateId input_model_regressed_greeks is not a unique subset of the nine Greek metric ids"
  }
  $candidatePrices = @($sweep.candidate_prices)
  if ($candidatePrices.Count -ne 8 -or @($candidatePrices.candidate_id | Select-Object -Unique).Count -ne 8) { throw "oracle targeted gate $GateId candidate registry mismatch" }
  foreach ($candidate in $candidatePrices) {
    if (-not (Test-OracleExactKeys $candidate @('candidate_id', 'smoke_price_mae_ticks', 'smoke_count', 'tune_sample_price_mae_ticks', 'tune_sample_count')) -or
        -not ($candidate.candidate_id -is [string]) -or -not (Test-OracleFiniteNumber $candidate.smoke_price_mae_ticks) -or
        -not (Test-OracleNonnegativeInteger $candidate.smoke_count) -or [long]$candidate.smoke_count -le 0 -or
        -not (Test-OracleFiniteNumber $candidate.tune_sample_price_mae_ticks) -or -not (Test-OracleNonnegativeInteger $candidate.tune_sample_count)) { throw "oracle targeted gate $GateId candidate evidence mismatch" }
  }
  $deltas = @($sweep.metric_deltas)
  if ($deltas.Count -ne 11 -or -not (Test-OracleExactStringSet @($deltas.metric_id) @($script:ModeAMetricMap.Values))) { throw "oracle targeted gate $GateId delta coverage mismatch" }
  $metricsById = @{}
  foreach ($metric in @($sweep.metrics)) { $metricsById[[string]$metric.metric_id] = $metric }
  foreach ($delta in $deltas) {
    if (-not (Test-OracleExactKeys $delta @('metric_id', 'candidate', 'baseline', 'delta', 'count', 'unit')) -or
        -not (Test-OracleFiniteNumber $delta.candidate) -or -not (Test-OracleFiniteNumber $delta.baseline) -or
        -not (Test-OracleFiniteNumber $delta.delta) -or -not (Test-OracleNonnegativeInteger $delta.count) -or [long]$delta.count -le 0) { throw "oracle targeted gate $GateId delta schema mismatch" }
    if ([Math]::Abs(([double]$delta.candidate - [double]$delta.baseline) - [double]$delta.delta) -gt 1.0e-12) { throw "oracle targeted gate $GateId delta arithmetic mismatch" }
    $metric = $metricsById[[string]$delta.metric_id]
    if (-not $metric -or [long]$delta.count -ne [long]$metric.count) { throw "oracle targeted gate $GateId delta population mismatch" }
  }
  if ($GateId -eq 'residual_floor') {
    if (-not (Test-Path -LiteralPath $ExpectedFloorPath -PathType Leaf)) { throw 'residual floor receipt is missing' }
    try { $floor = [System.IO.File]::ReadAllText($ExpectedFloorPath) | ConvertFrom-Json } catch { throw 'residual floor receipt is not JSON' }
    # `production_conventions` is committed too: without it the floor records the
    # map the sweep RESOLVED but not the map production actually prices with, and
    # the two are only checked against each other while a sweep is running.
    $floorKeys = @('schema_version', 'kind', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'mode', 'cohorts', 'smoke_blob_oid', 'tune_blob_oid', 'rows_processed', 'target_metric_ids', 'baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status', 'diagnostic_speed', 'speed')
    if (-not (Test-OracleExactKeys $floor $floorKeys) -or $floor.schema_version -ne 2 -or $floor.kind -ne 'residual_floor' -or
        $floor.command_id -ne 'mode_a_residual_floor' -or $floor.exit_code -ne 0 -or $floor.mode -ne 'A' -or
        [long]$floor.rows_processed -ne [long]$sweep.rows_priced -or -not (Test-OracleExactStringSet @($floor.cohorts) @('smoke', 'tune')) -or
        -not (Test-OracleConventionMap $floor.production_conventions) -or
        -not (Test-OracleExactStringSet @($floor.target_metric_ids) @($script:ModeAMetricMap.Values))) { throw 'residual floor receipt schema mismatch' }
    foreach ($name in @('baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status')) {
      if (-not (Test-OracleJsonValueEqual $floor.$name $sweep.$name)) {
        throw ('residual floor differs from recomputed sweep: ' + $name +
               ' (fields compare by VALUE, numbers as doubles; look for a real value change, a differing key set or array order,' +
               ' or a number written as a string / with digits that do not round-trip)')
      }
    }
  }
  # The scale selection ran on selection_count of count rows per metric; the
  # weakest of those ratios is the one worth carrying into the receipt.
  $minSelectionPercent = 100L
  foreach ($metric in @($sweep.metrics)) {
    $percent = [long][Math]::Floor((100.0 * [long]$metric.selection_count) / [long]$metric.count)
    if ($percent -lt $minSelectionPercent) { $minSelectionPercent = $percent }
  }
  return [pscustomobject]@{
    RowsProcessed = [long]$sweep.rows_priced
    RowsTotal = [long]$sweep.smoke_rows + [long]$sweep.tune_rows
    EngineErrors = [long]$sweep.engine_errors
    MinSelectionPercent = $minSelectionPercent
    MetricIds = @($script:ModeAMetricMap.Values | ForEach-Object { [string]$_ })
    Metrics = @($sweep.metrics)
    BaselineMetrics = @($sweep.baseline_metrics)
    MetricDeltas = @($sweep.metric_deltas)
    Conventions = $sweep.conventions
    ProductionConventions = $sweep.production_conventions
    CandidatePrices = @($sweep.candidate_prices)
    InputModelRegressedGreeks = $regressedGreeks
    DiagnosticSpeed = $sweep.diagnostic_speed
  }
}

# An empty $ExpectedFloorPath is the MEASURE arm: it emits the measured
# rel-avx2 rate with no pin comparison, because on a first-ever Stage 3 run no
# pin exists yet and this gate is the only sanctioned producer of one. The
# committed floor is DERIVED from that measurement — baseline = the measured
# rows_per_second and pin = floor(baseline * 0.90) — never copied verbatim: a
# pin equal to the baseline turns the re-measurement into a ~50/50 coin flip on
# ordinary run-to-run noise, so the margin is enforced, not merely documented.
function ConvertFrom-OracleSpeed([string]$ScorecardText, $Identity, [string]$ExpectedFloorPath) {
  try { $scorecard = $ScorecardText | ConvertFrom-Json } catch { throw 'convention speed scorecard is not JSON' }
  if (-not (Test-OracleExactKeys $scorecard @('iter', 'git_sha', 'cohort', 'modes', 'tolerances', 'cells')) -or
      $scorecard.git_sha -ne $Identity.Sha -or $scorecard.cohort -ne 'tune' -or -not (Test-OracleExactKeys $scorecard.modes @('a'))) { throw 'convention speed scorecard identity mismatch' }
  $mode = $scorecard.modes.a
  if (-not (Test-OracleNonnegativeInteger $mode.rows_priced) -or [long]$mode.rows_priced -le 0 -or
      -not (Test-OracleFiniteNumber $mode.rows_per_second) -or [double]$mode.rows_per_second -le 0) { throw 'convention speed produced no positive work' }
  if (-not $ExpectedFloorPath) {
    return [pscustomobject]@{
      RowsProcessed = [long]$mode.rows_priced
      MetricIds = @('rel_avx2_rows_per_second')
      Speed = [ordered]@{ metric_id = 'rel_avx2_rows_per_second'; value = [double]$mode.rows_per_second; count = [long]$mode.rows_priced; unit = 'rows_per_second'; preset = 'rel-avx2'; quiet_host = $true }
    }
  }
  if (-not (Test-Path -LiteralPath $ExpectedFloorPath -PathType Leaf)) { throw 'convention speed pin receipt is missing' }
  try { $floor = [System.IO.File]::ReadAllText($ExpectedFloorPath) | ConvertFrom-Json } catch { throw 'convention speed pin receipt is not JSON' }
  $speed = $floor.speed
  if (-not (Test-OracleExactKeys $speed @('metric_id', 'baseline', 'pin', 'unit', 'preset', 'quiet_host')) -or
      $speed.metric_id -ne 'rel_avx2_rows_per_second' -or $speed.unit -ne 'rows_per_second' -or
      $speed.preset -ne 'rel-avx2' -or -not $speed.quiet_host -or -not (Test-OracleFiniteNumber $speed.baseline) -or
      [double]$speed.baseline -le 0 -or -not (Test-OracleFiniteNumber $speed.pin) -or [double]$speed.pin -le 0 -or
      [double]$speed.pin -gt ([double]$speed.baseline * 0.95)) { throw 'convention speed pin is not a margined floor below its measured baseline' }
  if ([double]$mode.rows_per_second -lt [double]$speed.pin) { throw 'convention speed is below the pinned rel-avx2 floor' }
  return [pscustomobject]@{
    RowsProcessed = [long]$mode.rows_priced
    MetricIds = @('rel_avx2_rows_per_second')
    Speed = [ordered]@{ metric_id = 'rel_avx2_rows_per_second'; value = [double]$mode.rows_per_second; count = [long]$mode.rows_priced; unit = 'rows_per_second'; pin = [double]$speed.pin; preset = 'rel-avx2'; quiet_host = $true }
  }
}

function ConvertFrom-OracleBenchScorecard([string]$ScorecardText, [string]$GateId, $Identity) {
  $spec = Get-OracleTargetedGateSpec $GateId $Identity
  if ($GateId -in @('mode_a_smoke_tune', 'residual_floor')) { return ConvertFrom-OracleConventionSweep $ScorecardText $GateId $Identity $spec.ExpectedFloorPath }
  if ($GateId -in @('convention_speed_measure', 'convention_speed')) { return ConvertFrom-OracleSpeed $ScorecardText $Identity $spec.ExpectedFloorPath }
  try { $scorecard = $ScorecardText | ConvertFrom-Json } catch { throw "oracle targeted gate $GateId scorecard is not JSON" }
  if ($GateId -ne 'mode_a_smoke') { throw "oracle targeted gate $GateId has no production scorecard adapter yet" }
  if (-not (Test-OracleExactKeys $scorecard @('iter', 'git_sha', 'cohort', 'modes', 'tolerances', 'cells')) -or
      $scorecard.git_sha -ne $Identity.Sha -or $scorecard.cohort -ne 'smoke' -or -not (Test-OracleExactKeys $scorecard.modes @('a'))) {
    throw "oracle targeted gate $GateId scorecard identity/schema mismatch"
  }
  $mode = $scorecard.modes.a
  $modeKeys = @('rows_total', 'rows_priced', 'rows_null_sentinel', 'rows_bad_input', 'rows_engine_error', 'wall_seconds', 'rows_per_second')
  if (-not (Test-OracleExactKeys $mode $modeKeys)) { throw "oracle targeted gate $GateId scorecard mode schema mismatch" }
  foreach ($name in @('rows_total', 'rows_priced', 'rows_null_sentinel', 'rows_bad_input', 'rows_engine_error')) {
    if (-not (Test-OracleNonnegativeInteger $mode.$name)) { throw "oracle targeted gate $GateId scorecard has invalid $name" }
  }
  $rowsProcessed = [long]$mode.rows_priced
  $accounted = $rowsProcessed + [long]$mode.rows_null_sentinel + [long]$mode.rows_bad_input + [long]$mode.rows_engine_error
  if ($rowsProcessed -le 0 -or [long]$mode.rows_total -ne $accounted -or -not (Test-OracleFiniteNumber $mode.wall_seconds) -or [double]$mode.wall_seconds -le 0 -or
      -not (Test-OracleFiniteNumber $mode.rows_per_second) -or [double]$mode.rows_per_second -le 0) {
    throw "oracle targeted gate $GateId scorecard reports empty/inconsistent priced work"
  }
  $cellProperties = @($scorecard.cells.PSObject.Properties)
  if (-not $cellProperties.Count) { throw "oracle targeted gate $GateId scorecard has no aggregate cells" }
  $seenMetrics = @{}
  $cellPattern = '^a\.(price|vol|de|ga|th|ve|rh|ph|vo|va|deDecay)\.(deep-itm|itm|atm|otm|deep-otm)\.(0-7|8-30|31-90|90\+)\.(c|p)$'
  $statKeys = @('n', 'mae', 'rmse', 'p50', 'p95', 'p99', 'max', 'within_tol_rate')
  foreach ($property in $cellProperties) {
    if ($property.Name -notmatch $cellPattern) { throw "oracle targeted gate $GateId scorecard contains an unknown cell ID" }
    $metric = $Matches[1]
    $stats = $property.Value
    if (-not (Test-OracleExactKeys $stats $statKeys) -or -not (Test-OracleNonnegativeInteger $stats.n) -or [long]$stats.n -le 0) {
      throw "oracle targeted gate $GateId scorecard contains an invalid aggregate cell"
    }
    foreach ($field in @('mae', 'rmse', 'p50', 'p95', 'p99', 'max', 'within_tol_rate')) {
      if (-not (Test-OracleFiniteNumber $stats.$field)) { throw "oracle targeted gate $GateId scorecard contains a non-finite aggregate" }
    }
    if ([double]$stats.mae -lt 0 -or [double]$stats.rmse -lt 0 -or [double]$stats.p50 -lt 0 -or [double]$stats.p95 -lt 0 -or
        [double]$stats.p99 -lt 0 -or [double]$stats.max -lt 0 -or [double]$stats.within_tol_rate -lt 0 -or [double]$stats.within_tol_rate -gt 1) {
      throw "oracle targeted gate $GateId scorecard contains an out-of-range aggregate"
    }
    if (-not $seenMetrics.ContainsKey($metric)) { $seenMetrics[$metric] = 0L }
    $seenMetrics[$metric] += [long]$stats.n
  }
  $expectedMetrics = @($script:ModeAMetricMap.Keys | ForEach-Object { [string]$_ })
  if (-not (Test-OracleExactStringSet @($seenMetrics.Keys) $expectedMetrics) -or @($seenMetrics.Values | Where-Object { [long]$_ -le 0 }).Count) {
    throw "oracle targeted gate $GateId scorecard metric coverage is incomplete"
  }
  return [pscustomobject]@{
    RowsProcessed = $rowsProcessed
    MetricIds = @($expectedMetrics | ForEach-Object { [string]$script:ModeAMetricMap[$_] })
  }
}

function Invoke-OracleTargetedGate([string]$GateId, [scriptblock]$Invoker) {
  $identity = Get-OracleGitIdentity
  $spec = Get-OracleTargetedGateSpec $GateId $identity
  if (-not $Invoker) {
    if ($spec.PrepareProgram) {
      $prepare = Invoke-OracleNativeProcess $spec.PrepareProgram @($spec.PrepareArguments)
      $prepareLines = @($prepare.Lines)
      $prepareExitCode = [int]$prepare.ExitCode
      if ($prepareExitCode -ne 0) { throw "oracle targeted gate $GateId target build failed with exit code $prepareExitCode" }
    }
    Assert-OracleGateExecutables $spec
    if ($spec.Kind -in @('oracle_bench', 'oracle_convention', 'oracle_speed')) {
      if (-not (Test-Path -LiteralPath $script:OracleStoreRoot -PathType Container)) { throw 'licensed aggregate oracle store is missing' }
      New-Item -ItemType Directory -Force (Split-Path -Parent $spec.OutputPath) | Out-Null
      if (Test-Path -LiteralPath $spec.OutputPath) { Remove-Item -LiteralPath $spec.OutputPath -Force }
    }
    if ($spec.Kind -eq 'oracle_speed') { Assert-OracleQuietHost }
  }
  if ($Invoker) {
    $execution = & $Invoker $spec
    $exitCode = [int]$execution.ExitCode
    $lines = @($execution.Lines | ForEach-Object { [string]$_ })
  } elseif ($spec.Kind -eq 'oracle_floor_verify') {
    if (-not (Test-Path -LiteralPath $spec.OutputPath -PathType Leaf)) {
      throw 'residual floor requires the exact-SHA mode_a_smoke_tune artifact first'
    }
    $execution = [pscustomobject]@{
      ExitCode = 0
      Lines = @('residual-floor: verifying exact-SHA cached smoke+tune sweep ' + $identity.Sha)
    }
    $lines = @($execution.Lines)
    $exitCode = 0
  } else {
    $execution = Invoke-OracleNativeProcess $spec.Program @($spec.Arguments)
    $lines = @($execution.Lines)
    if ($prepareLines) { $lines = @($prepareLines) + @($lines) }
    $exitCode = [int]$execution.ExitCode
  }
  $identityAfter = Get-OracleGitIdentity
  if ($identityAfter.Sha -ne $identity.Sha -or $identityAfter.Tree -ne $identity.Tree) { throw "oracle targeted gate $GateId changed HEAD/tree while executing" }
  $raw = ($lines -join "`n").Trim()
  $observations = @($lines | Where-Object { $_.Trim() }).Count
  if ($exitCode -ne 0) { throw "oracle targeted gate $GateId failed with exit code $exitCode" }
  if ($observations -lt 1) { throw "oracle targeted gate $GateId emitted no process evidence" }
  $testsExecuted = 0
  $testsPassed = 0
  $rowsProcessed = 0L
  $metricIds = @()
  $rawEvidence = $raw
  if ($spec.Kind -eq 'ctest') {
    if ($raw -match 'No tests were found') { throw "oracle targeted gate $GateId executed zero tests" }
    $summary = [regex]::Match($raw, '(?m)(\d+)% tests passed,\s*(\d+) tests failed out of\s*(\d+)')
    if (-not $summary.Success) { throw "oracle targeted gate $GateId lacks a typed ctest summary" }
    $percent = [int]$summary.Groups[1].Value
    $failed = [int]$summary.Groups[2].Value
    $testsExecuted = [int]$summary.Groups[3].Value
    $testsPassed = $testsExecuted - $failed
    if ($testsExecuted -le 0 -or $failed -ne 0 -or $percent -ne 100 -or $testsPassed -ne $testsExecuted) { throw "oracle targeted gate $GateId did not pass positive test work" }
    if ($spec.PSObject.Properties.Name -contains 'ExpectedTestIds') {
      $passedTestIds = @($lines | ForEach-Object {
        $match = [regex]::Match([string]$_, 'Test\s+#[0-9]+:\s+([A-Za-z][A-Za-z0-9_.]*)\s+\.+\s+Passed')
        if ($match.Success) { $match.Groups[1].Value }
      })
      if ($testsExecuted -ne @($spec.ExpectedTestIds).Count -or -not (Test-OracleExactStringSet $passedTestIds @($spec.ExpectedTestIds))) {
        throw ("oracle targeted gate $GateId test closure differs from its pinned " + @($spec.ExpectedTestIds).Count + '-test registry')
      }
    }
    $auditSummary = "tests_executed=$testsExecuted tests_passed=$testsPassed"
  } else {
    if ($Invoker -and $execution.PSObject.Properties.Name -contains 'ScorecardJson') {
      $scorecardText = [string]$execution.ScorecardJson
    } else {
      if (-not $spec.OutputPath -or -not (Test-Path -LiteralPath $spec.OutputPath -PathType Leaf)) { throw "oracle targeted gate $GateId did not produce a scorecard" }
      $scorecardText = [System.IO.File]::ReadAllText($spec.OutputPath)
    }
    $aggregate = ConvertFrom-OracleBenchScorecard $scorecardText $GateId $identity
    $rowsProcessed = [long]$aggregate.RowsProcessed
    $metricIds = @($aggregate.MetricIds | ForEach-Object { [string]$_ })
    $requiredMetricIds = Get-OracleRequiredMetricIds $GateId
    if ($rowsProcessed -le 0 -or -not (Test-OracleExactStringSet $metricIds $requiredMetricIds)) { throw "oracle targeted gate $GateId reported empty/incomplete aggregate work" }
    $auditSummary = 'status=PASS rows_processed=' + $rowsProcessed + ' metric_ids=' + (($metricIds | Sort-Object) -join ',')
    # Surface the weakest selection population so a scale chosen on a sliver of
    # the cohort is visible in the receipt, not only inside the sweep artifact.
    if ($aggregate.PSObject.Properties.Name -contains 'MinSelectionPercent') {
      $auditSummary += ' min_selection_pct=' + [long]$aggregate.MinSelectionPercent
    }
    $rawEvidence = $raw + "`n--scorecard--`n" + $scorecardText
  }
  $result = [ordered]@{
    schema_version = 1
    status = 'PASS'
    observations = $observations
    command_id = $GateId
    tested_sha = $identity.Sha
    tested_tree = $identity.Tree
    gate_kind = $spec.Kind
    tests_executed = $testsExecuted
    tests_passed = $testsPassed
    rows_processed = $rowsProcessed
    metric_ids = $metricIds
    audit_summary = $auditSummary
    raw_output_sha256 = Get-OracleTextSha256 $rawEvidence
  }
  if ($aggregate -and $aggregate.PSObject.Properties.Name -contains 'Metrics') {
    # Carried so the row-accounting identity smoke_rows+tune_rows ==
    # rows_priced+engine_errors is re-checkable from the receipt alone.
    $result.rows_total = [long]$aggregate.RowsTotal
    $result.engine_errors = [long]$aggregate.EngineErrors
    $result.metrics = @($aggregate.Metrics)
    $result.baseline_metrics = @($aggregate.BaselineMetrics)
    $result.metric_deltas = @($aggregate.MetricDeltas)
    $result.conventions = $aggregate.Conventions
    $result.production_conventions = $aggregate.ProductionConventions
    $result.candidate_prices = @($aggregate.CandidatePrices)
    # Carried into the typed receipt, not left inside the sweep artifact: a
    # reviewer must see what the greek-aware input-model rank cost without
    # opening a 12-minute run's output.
    $result.input_model_regressed_greeks = @($aggregate.InputModelRegressedGreeks)
    $result.diagnostic_speed = $aggregate.DiagnosticSpeed
  }
  if ($aggregate -and $aggregate.PSObject.Properties.Name -contains 'Speed') { $result.speed = $aggregate.Speed }
  return $result
}

# Pester imports the production functions and injects only the process invoker.
if ($MyInvocation.InvocationName -eq '.') { return }
if (-not $Gate) { throw '-Gate is required' }
Invoke-OracleTargetedGate $Gate $null | ConvertTo-Json -Compress
