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
  # The FROZEN oracle-loop command lines, asserted verbatim against
  # parse_bench_args. RATCHET_GATE_COMMANDS / READY_MEASURE_GATES in
  # .claude/workflows/vol-oracle-iter.js and mode_b_smoke_tune below are the
  # definition sites; these cases are what stops a flag rename from killing the
  # loop at argument parsing three layers from its cause. The two holdout
  # commands are asserted at PARSE ONLY -- nothing here benchmarks holdout.
  'OracleBenchArgs.ParsesFrozenMeasureModeACommand',
  'OracleBenchArgs.ParsesFrozenMeasureModeBCommand',
  'OracleBenchArgs.ParsesFrozenSpeedCommand',
  'OracleBenchArgs.ParsesFrozenHoldoutCommandsWithoutRunningThem',
  'OracleBenchArgs.RejectsUnknownModeValue',
  'OracleBenchArgs.AggregateOnlyDefaultsStoreAndStdout',
  'OracleBenchArgs.ScorecardPathStillRequiresStoreAndOut',
  'OracleBenchArgs.ConventionSweepRejectsTheNewFlags',
  'OracleBenchArgs.RejectsScorecardTogetherWithBenchmarkSpeed',
  'OracleBenchCohortSpec.ResolvesNamesAndKeepsPathsVerbatim',
  'OracleBenchCohortSpec.RejectsUnresolvableNameAndDuplicates',
  'OracleBenchCohortSpec.FindsTheManifestDirByWalkingUp',
  'OracleBenchQuietHost.RefusesWhenACompetingProcessIsRunning',
  'OracleBenchSpeedMetric.IdIsDerivedFromThePreset',
  'OracleBenchReader.OpensOnlyCohortNamedPartitionsAndFiltersUnderlier',
  'OracleBenchReader.CrossesUnderliersAndBuckets',
  'OracleBenchReader.MissingPartitionDirIsNotFound',
  'OracleBenchE2E.SyntheticCohortProducesCharterScorecard',
  # --aggregate-only is a CONFIDENTIALITY boundary, not a formatting option:
  # these three assert that no cell key, band, date, bucket or underlier
  # reaches the aggregate receipt or stderr.
  'OracleBenchAggregate.PublishesTheElevenTargetsAndNoMembership',
  'OracleBenchAggregate.WritesToStdoutWhenOutIsAbsent',
  'OracleBenchAggregate.BenchmarkSpeedPublishesRowsPerSecondOnly',
  # Stage 4, Mode B: volatility MEASURED from raw NBBO. The first five REPLACED
  # OracleBenchModeB.FailsAtRunTimeWithADistinctActionableError, which asserted
  # the run-time refusal a real Mode B runner has now retired. That test's load-
  # bearing property -- no Mode B invocation may hand the ratchet a number it did
  # not measure -- survives in WithoutRealDataItFailsInsteadOfPublishingNumbers,
  # and the confidentiality boundary above is RE-ASSERTED for Mode B rather than
  # assumed inherited (Mode B added both a stderr line and a receipt block).
  # These are also exactly the cases the mode_b_targeted_tests ctest case runs,
  # whose PASS_REGULAR_EXPRESSION pins that count (atx-vol/tests/CMakeLists.txt).
  # Another Mode B case therefore moves THREE definition sites: this set, that
  # regex, and ORACLE_BENCH_TEST_COUNT in .claude/workflows/vol-oracle-iter.js.
  'OracleBenchModeB.RecoversTheVolThatGeneratedTheQuote',
  'OracleBenchModeB.GroupsByUnderlierExpiryAndBucket',
  'OracleBenchModeB.RefusesUnidentifiedRowsInsteadOfClampingToTheVolFloor',
  'OracleBenchModeB.AggregatePublishesTheElevenTargetsAndNoMembership',
  'OracleBenchModeB.WithoutRealDataItFailsInsteadOfPublishingNumbers',
  # Mode B, the EUROPEAN leg (iter-002): the exercise-style axis honoured inside
  # the inversion. European rows invert against the European pricing leg with
  # the European admission band (discounted forward intrinsic, no early-exercise
  # floor); American rows keep the American inverter and bounds byte-for-byte.
  'OracleBenchModeB.EuropeanRowsInvertAgainstTheEuropeanLeg',
  'OracleBenchModeB.RefusesAEuropeanMidAtTheDiscountedForwardIntrinsic',
  'OracleBenchModeB.DeepItmEuropeanPutBelowIntrinsicStillInverts',
  'OracleBenchModeB.AmericanRowsKeepTheAmericanBoundsAndInverter'
)
# Pinned exactly like the OracleBench registry above: the Stage 3 suite is
# discovered per gtest case, so a vanished or renamed case fails the gate
# instead of passing a filter that matched nothing.
$script:OracleConventionTestIds = @(
  'OracleConvention.DiscreteDividendForwardIsAppliedExactly',
  # The exercise-style convention axis: which PRICER a row is entitled to.
  # Three cases pin the three load-bearing facts — the European leg is the
  # independent European rung (no American intrinsic floor), each rule routes
  # exactly its named roots and nothing else, and an ingested per-row style
  # outranks the root-list rule.
  'OracleConvention.EuropeanLegMatchesTheIndependentRungWithNoIntrinsicFloor',
  'OracleConvention.ExerciseStyleRulesRouteOnlyTheirNamedRoots',
  'OracleConvention.IngestedExerciseStyleOutranksTheRootListRule',
  # The time-decay convention axis: HOW theta and delta decay are formed. Three
  # cases pin the three load-bearing facts — the secant is the one-business-day
  # difference of price and delta (and the axis never moves a price), the two
  # per-day scales are INERT under it because the secant is already a one-day
  # quantity, and the expiration-day boundary resolves to the intrinsic payoff
  # rather than to an invented epsilon.
  'OracleConvention.SecantDecayIsTheOneDayDifferenceOfPriceAndDelta',
  'OracleConvention.SecantDecayIgnoresTheThetaAndDecayScales',
  'OracleConvention.ExpirationDayDecayLegIsTheIntrinsicPayoff',
  'OracleConvention.ProductionMapIsTheResolvedHardCut',
  'OracleConvention.BestScaleRanksOnTheSelectionObjective',
  'OracleConvention.SymmetricObjectiveHasNoSmallestScaleGradient',
  'OracleConvention.FinalistRankPrefersNoGreekRegressionOverLowerPriceMae',
  # A candidate id is ordered FIELD BY FIELD on '|', never as one flat string:
  # '|' sorts above '_' and one exercise-style id is a strict prefix of another,
  # so a flat comparison reversed that pair the moment the time-decay field was
  # appended after it.
  'OracleConvention.CandidateIdentityOrdersFieldByFieldNotAsAFlatString',
  'OracleConvention.BestScaleTieBreaksOnSourceThenNumericScale',
  'OracleConvention.BestScaleWithoutSelectionEvidenceUsesCandidateIdentity',
  'OracleConvention.CompleteMapNamesEveryGreekSignAndScale',
  'OracleConvention.ThetaDayCountNeverRebucketsDteBands',
  'OracleConvention.SweepIsClosedDeterministicAndCoversElevenMetrics',
  'OracleConvention.AcceptedRegressionsPublishWithinBoundAndOmitBeyondIt',
  'OracleConvention.CandidateAndBaselineFloorsShareOneRowPopulation',
  'OracleConvention.StandardAndSymmetricFloorsDisagreeInDirection',
  'OracleConvention.SubFloorOracleRowsBothReportAndSelect',
  'OracleConvention.SweepPublishesTheSelectedInputModelGreekRegressions',
  'OracleConvention.SweepJsonPublishesTheProductionMapBesideTheWinner',
  'OracleConvention.SweepRefusesAMetricNoRowObserved',
  'OracleConvention.SweepRejectsEmptyCohort'
)
# The BOUNDED no-regression rule, as a multiplier on the baseline value. Stated
# as the multiplier and not as `1 + fraction` because five layers in three
# languages re-evaluate this same comparison and `1.0 + 0.01` is not required to
# be the same double as `1.01`. Mirrors kRegressionBoundMultiplier in
# atx-vol/tools/oracle_convention_sweep.hpp, which carries the full rationale.
$script:OracleRegressionBoundMultiplier = 1.01
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
      # rel-avx2, NOT dev. The convention sweep is the single most expensive
      # thing this gate set runs, and the Debug build made it unrunnable rather
      # than merely slow: a full sweep took 74,711 s (20.7 h) from the dev
      # binary against 193 s from rel-avx2 -- a 387x difference that turns a gate
      # into an overnight job nobody waits for. The two runs are the SAME
      # answer, not a speed/accuracy trade: identical winning convention map,
      # metrics agreeing to <= 1.7e-9 relative, and price MAE identical to
      # 6.7e-15. The optimization level cannot change the selection here, so the
      # only thing Debug bought was the wall clock.
      #
      # Only the two sweep gates move. Every other gate below stays on dev,
      # where the debug build is the point of the test.
      $out = Join-Path $outputRoot ('mode-a-smoke-tune-' + $Identity.Sha + '.json')
      return [pscustomobject]@{
        Kind = 'oracle_convention'; Program = $relBenchExe; OutputPath = $out
        RequiredExecutables = @($relBenchExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'rel-avx2', 'build', 'atx-vol-oracle-bench', '--parallel', '2')
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
    'mode_b_targeted_tests' {
      # PrepareArguments matches every other ctest gate here (mode_a_targeted_tests,
      # convention_tests): without a build step the gate validates whatever
      # atx-vol-tests.exe happens to be on disk, which for a source-level gate is
      # the difference between "the Mode B tests pass" and "they passed once".
      # It also regenerates the build tree, which the `mode_b_targeted_tests`
      # ctest case needs to exist at all. The gate's own Arguments are untouched.
      return [pscustomobject]@{
        Kind = 'ctest'; Program = 'powershell'; OutputPath = ''
        RequiredExecutables = @($testExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'dev', 'build', 'atx-vol-tests', '--parallel', '2')
        Arguments = @('-NoProfile', '-File', $buildScript, '-Preset', 'dev', '-Ctest', '-R', '^mode_b_targeted_tests$', '--no-tests=error')
      }
    }
    'mode_b_smoke_tune' {
      # KIND 'oracle_aggregate', not 'oracle_bench': this command writes its
      # receipt to STDOUT, because the two verbatim-frozen Mode B command lines
      # -- READY_MEASURE_GATES.measure_mode_b and RATCHET_GATE_COMMANDS
      # .holdout_mode_b in .claude/workflows/vol-oracle-iter.js -- both use
      # --aggregate-only with NO --out. Giving this gate the easier --out shape
      # would leave the stdout path unexercised by any gate until the ready-state
      # run depended on it, so the ARGUMENTS below stay byte-identical to that
      # shape deliberately. The distinct kind is what keeps the output-file
      # preparation in Invoke-OracleTargetedGate (which cannot take an empty
      # OutputPath) from running at all, rather than papering over it.
      #
      # Program is the WORKTREE-LOCAL binary. It was a bare 'atx-vol-oracle-bench'
      # PATH lookup, which this script's own banner forbids -- a gate that
      # resolves its binary off PATH can validate a build from another tree.
      #
      # That binary is now the rel-avx2 one, for the reason spelled out on
      # mode_a_smoke_tune above: a full sweep costs 74,711 s (20.7 h) from the
      # dev build and 193 s from rel-avx2, for the same winning convention map
      # and metrics within 1.7e-9 relative (price MAE identical to 6.7e-15).
      # Note that the PRESET moved and the ARGUMENTS did not, and must not: the
      # freeze described above is on the command line, not on the build it runs
      # from, so swapping the binary keeps the stdout/--aggregate-only shape
      # byte-identical to the two verbatim-frozen Mode B command lines.
      return [pscustomobject]@{
        Kind = 'oracle_aggregate'; Program = $relBenchExe; OutputPath = ''
        RequiredExecutables = @($relBenchExe)
        PrepareProgram = 'powershell'
        PrepareArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript, '-Preset', 'rel-avx2', 'build', 'atx-vol-oracle-bench', '--parallel', '2')
        Arguments = @('--cohort', 'smoke,tune', '--mode', 'B', '--aggregate-only')
      }
    }
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

# BOUNDED no-regression gate, stated against the SYMMETRIC-RELATIVE arrays. A
# symmetric metric may end up worse than its baseline only while
#   candidate <= baseline * $script:OracleRegressionBoundMultiplier
# and every regression the bound permits is PUBLISHED in `accepted_regressions`.
# Anything past the bound fails the gate closed.
#
# Why a bound rather than `candidate <= baseline`: the convention fit is
# multi-objective over ELEVEN targets that share ONE map, and no point in the
# closed candidate grid strictly dominates every other on all eleven. A strict
# per-metric rule therefore cannot be satisfied by anything the search can reach
# — it does not say "never get worse", it says "never pick anything" — and the
# only ways past it are hand-tuning the map or a bypass flag, both worse than a
# stated bound. Why 1%: it is the charter's own Mode A Greek tolerance, so a
# regression inside it cannot flip a scorecard cell's verdict.
#
# The bound is a licence to LOSE ground, never to hide it. There is still no
# bypass flag and no per-metric allowlist, and Test-OracleAcceptedRegressions
# below checks BOTH directions, so a receipt that regresses and publishes an
# empty array fails exactly like one that regresses past the bound. That
# two-way check is what stops the array from becoming a rubber stamp.
#
# The symmetric loss is the one the scale SELECTION minimises, and it is bounded
# with no smallest-scale gradient. Gating the standard-relative array instead
# pins the denominator on near-zero-oracle rows and therefore systematically
# rewards the smaller multiplier — a criterion that contradicts the selector by
# construction rather than catching a real defect. The standard array is still
# validated for shape, finiteness, non-negativity, population parity and delta
# arithmetic; it is simply no longer the regression criterion.
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
    if ($candidateValue -gt ($baselineValue * $script:OracleRegressionBoundMultiplier)) {
      $offenders += ($id + ' candidate=' + $candidateValue.ToString('R', $invariant) + ' baseline=' +
                     $baselineValue.ToString('R', $invariant) + ' bound=' +
                     ($baselineValue * $script:OracleRegressionBoundMultiplier).ToString('R', $invariant))
    }
  }
  return @($offenders)
}

# The set the receipt MUST publish: every symmetric metric strictly worse than
# its baseline and within the bound. A zero baseline needs no special case —
# `candidate <= 0 * 1.01` holds only at candidate == 0, which is not a
# regression — so nothing here ever divides by zero.
function Get-OracleWithinBoundRegressions($Metrics, $BaselineMetrics) {
  $baselineById = @{}
  foreach ($metric in @($BaselineMetrics)) { $baselineById[[string]$metric.metric_id] = $metric }
  $within = @()
  foreach ($metric in @($Metrics)) {
    $baseline = $baselineById[[string]$metric.metric_id]
    if (-not $baseline) { continue }
    $candidateValue = [double]$metric.value
    $baselineValue = [double]$baseline.value
    if ($candidateValue -le $baselineValue -or
        $candidateValue -gt ($baselineValue * $script:OracleRegressionBoundMultiplier)) { continue }
    $within += [pscustomobject]@{
      MetricId = [string]$metric.metric_id
      Candidate = $candidateValue
      Baseline = $baselineValue
      Pct = ($candidateValue - $baselineValue) / $baselineValue
    }
  }
  return @($within)
}

# Cross-check in BOTH directions, which is the whole point of publishing the
# array: every entry must be a real within-bound regression carrying the same
# two values the symmetric arrays report, and every within-bound regression must
# have an entry. Returns '' when consistent, otherwise the reason.
#
# `pct_of_baseline` is compared with the 1e-12 tolerance the delta arrays
# already use, because it is a derived quotient and not a copied value.
function Test-OracleAcceptedRegressions($Published, $Metrics, $BaselineMetrics) {
  $entries = @($Published)
  $expected = @(Get-OracleWithinBoundRegressions $Metrics $BaselineMetrics)
  $invariant = [Globalization.CultureInfo]::InvariantCulture
  $expectedById = @{}
  foreach ($item in $expected) { $expectedById[$item.MetricId] = $item }
  $seen = @{}
  foreach ($entry in $entries) {
    if (-not (Test-OracleExactKeys $entry @('metric_id', 'candidate', 'baseline', 'pct_of_baseline')) -or
        -not (Test-OracleFiniteNumber $entry.candidate) -or -not (Test-OracleFiniteNumber $entry.baseline) -or
        -not (Test-OracleFiniteNumber $entry.pct_of_baseline)) { return 'accepted_regressions entry schema mismatch' }
    $id = [string]$entry.metric_id
    if ($seen.ContainsKey($id)) { return ('accepted_regressions names ' + $id + ' twice') }
    $seen[$id] = $true
    $match = $expectedById[$id]
    if (-not $match) { return ('accepted_regressions publishes ' + $id + ', which is not a within-bound symmetric regression') }
    if ([double]$entry.candidate -ne $match.Candidate -or [double]$entry.baseline -ne $match.Baseline) {
      return ('accepted_regressions entry for ' + $id + ' does not carry the symmetric arrays'' own values')
    }
    if ([Math]::Abs([double]$entry.pct_of_baseline - $match.Pct) -gt 1.0e-12) {
      return ('accepted_regressions pct_of_baseline for ' + $id + ' is not (candidate - baseline) / baseline')
    }
  }
  $unpublished = @($expected | Where-Object { -not $seen.ContainsKey($_.MetricId) })
  if ($unpublished.Count) {
    return ('symmetric regression(s) within bound but absent from accepted_regressions: ' +
            (@($unpublished | ForEach-Object {
              $_.MetricId + ' candidate=' + $_.Candidate.ToString('R', $invariant) +
              ' baseline=' + $_.Baseline.ToString('R', $invariant)
            }) -join '; '))
  }
  return ''
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
  # `exercise_style` is OPTIONAL: maps committed before the axis existed omit it,
  # and absence means `american_all` (the historical behaviour — every root gets
  # the American pricer). The value domain is closed like every other key. It is
  # never required here, because this validator also runs against committed
  # receipts that predate the axis and MUST keep validating them.
  if ($Map -and @($Map.PSObject.Properties.Name) -contains 'exercise_style') {
    if (@('american_all', 'european_cash_settled_index', 'european_cash_settled_index_plus_empirical') -notcontains $Map.exercise_style) { return $false }
    $keys = @($keys) + 'exercise_style'
  }
  # `time_decay_method` is OPTIONAL for exactly the same reason, one axis later:
  # maps committed before the axis existed omit it, and absence means
  # `analytic_derivative` (the historical behaviour — theta and delta decay are
  # the analytic dP/dt and charm jets times their per-day scales). Under
  # `secant_252` those two scales are inert, which is why this key must be read
  # before `theta_basis` / `delta_decay_basis` are believed to describe a
  # multiplier that was actually applied.
  if ($Map -and @($Map.PSObject.Properties.Name) -contains 'time_decay_method') {
    if (@('analytic_derivative', 'secant_252') -notcontains $Map.time_decay_method) { return $false }
    $keys = @($keys) + 'time_decay_method'
  }
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

# NAMED normalization for the exercise-style axis, not generic key-dropping: a
# convention map that OMITS `exercise_style` and one that says `american_all`
# mean the same pricing — absence predates the axis, whose default is the
# historical American-everywhere behaviour. Comparisons of a freshly-computed
# sweep map (new format, key always present) against a committed floor map (old
# format, key absent) must go through this so the two forms compare EQUAL when
# they mean the same thing, and still compare UNEQUAL when the sweep resolved a
# non-default style the committed floor never priced with.
function ConvertTo-OracleConventionMapWithExplicitExerciseStyle($Map) {
  if ($null -eq $Map -or -not ($Map -is [System.Management.Automation.PSCustomObject])) { return $Map }
  if (@($Map.PSObject.Properties.Name) -contains 'exercise_style') { return $Map }
  $explicit = [pscustomobject]@{}
  foreach ($property in $Map.PSObject.Properties) {
    Add-Member -InputObject $explicit -MemberType NoteProperty -Name $property.Name -Value $property.Value
  }
  Add-Member -InputObject $explicit -MemberType NoteProperty -Name 'exercise_style' -Value 'american_all'
  return $explicit
}

# The same NAMED normalization, one axis later: a convention map that OMITS
# `time_decay_method` and one that says `analytic_derivative` describe the same
# reported Greeks — absence predates the axis, whose default is the historical
# analytic dP/dt and charm jets. Kept as a second named function rather than
# folded into a generic "fill in missing keys" helper, because the DEFAULT is the
# whole content of each: a generic helper would silently invent a default for
# whatever key is added next, which is exactly the way a real difference gets
# normalized away.
function ConvertTo-OracleConventionMapWithExplicitTimeDecayMethod($Map) {
  if ($null -eq $Map -or -not ($Map -is [System.Management.Automation.PSCustomObject])) { return $Map }
  if (@($Map.PSObject.Properties.Name) -contains 'time_decay_method') { return $Map }
  $explicit = [pscustomobject]@{}
  foreach ($property in $Map.PSObject.Properties) {
    Add-Member -InputObject $explicit -MemberType NoteProperty -Name $property.Name -Value $property.Value
  }
  Add-Member -InputObject $explicit -MemberType NoteProperty -Name 'time_decay_method' -Value 'analytic_derivative'
  return $explicit
}

function ConvertFrom-OracleConventionSweep([string]$ScorecardText, [string]$GateId, $Identity, [string]$ExpectedFloorPath) {
  try { $sweep = $ScorecardText | ConvertFrom-Json } catch { throw "oracle targeted gate $GateId sweep is not JSON" }
  $keys = @('schema_version', 'kind', 'git_sha', 'cohorts', 'selection_strategy', 'smoke_rows', 'tune_rows', 'rows_priced', 'engine_errors', 'baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'accepted_regressions', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status', 'diagnostic_speed')
  if (-not (Test-OracleExactKeys $sweep $keys) -or $sweep.schema_version -ne 2 -or $sweep.kind -ne 'convention_sweep' -or
      $sweep.git_sha -ne $Identity.Sha -or -not (Test-OracleExactStringSet @($sweep.cohorts) @('smoke', 'tune')) -or
      -not (Test-OracleNonnegativeInteger $sweep.smoke_rows) -or [long]$sweep.smoke_rows -le 0 -or
      -not (Test-OracleNonnegativeInteger $sweep.tune_rows) -or [long]$sweep.tune_rows -le 0 -or
      -not (Test-OracleNonnegativeInteger $sweep.rows_priced) -or [long]$sweep.rows_priced -le 0 -or
      -not (Test-OracleNonnegativeInteger $sweep.engine_errors) -or -not (Test-OracleMetricArray $sweep.metrics) -or
      -not (Test-OracleMetricArray $sweep.baseline_metrics) -or -not (Test-OracleConventionMap $sweep.conventions) -or
      -not (Test-OracleMetricArray $sweep.symmetric_metrics) -or -not (Test-OracleMetricArray $sweep.baseline_symmetric_metrics) -or
      -not (Test-OracleConventionMap $sweep.baseline_conventions) -or -not (Test-OracleConventionMap $sweep.production_conventions) -or
      -not (Test-OracleMetricPopulationParity $sweep.metrics $sweep.baseline_metrics) -or
      -not (Test-OracleMetricPopulationParity $sweep.symmetric_metrics $sweep.baseline_symmetric_metrics) -or
      -not (Test-OracleMetricPopulationParity $sweep.symmetric_metrics $sweep.metrics) -or
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
  # BOUNDED gate: fail closed on any SYMMETRIC metric worse than its baseline by
  # more than the published bound, and name every offender with both values and
  # the bound so the failure is diagnosable without re-running a 12-minute
  # sweep. The symmetric array is the one the scale selection optimises, so gate
  # and selector agree; the standard-relative array above is validated for
  # shape/parity and published for comparability with the charter target, but it
  # is not the regression criterion.
  $regressions = @(Get-OracleMetricRegressions $sweep.symmetric_metrics $sweep.baseline_symmetric_metrics)
  if ($regressions.Count) {
    throw ("oracle targeted gate $GateId candidate exceeds the published regression bound on " + $regressions.Count +
           ' symmetric metric(s): ' + ($regressions -join '; '))
  }
  # Every regression the bound permitted must be PUBLISHED, and everything
  # published must be a real within-bound regression. Checked in both
  # directions: an empty array over a real regression is the failure mode that
  # would turn the bound into a rubber stamp.
  $acceptedError = Test-OracleAcceptedRegressions $sweep.accepted_regressions $sweep.symmetric_metrics $sweep.baseline_symmetric_metrics
  if ($acceptedError) { throw "oracle targeted gate $GateId $acceptedError" }
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
  # The candidate registry is the CLOSED three-axis grid the sweep searches:
  # every input model crossed with every exercise-style rule crossed with every
  # time-decay method, with candidate_id rendered as
  # '<input_model_id>|<exercise_style_id>|<time_decay_method_id>' (candidate_id_of
  # in atx-vol/tools/oracle_convention_sweep.cpp; the C++ side pins the same 48
  # in OracleConvention.SweepIsClosedDeterministicAndCoversElevenMetrics). THAT
  # PAIRING IS A RECORDED TRAP: this set and that C++ pin must move in the SAME
  # commit, or a gate run spends the whole sweep before failing on a registry
  # mismatch. Pinned as the exact id SET, not a count: a dropped, duplicated, or
  # renamed grid point fails here instead of passing as a silently narrower
  # search. The id domains are the same closed enums Test-OracleConventionMap
  # enforces.
  $candidateInputModels = @('uprc_spot__rate__sdiv_yield', 'discrete_forward_pv__rate__sdiv_yield', 'discrete_forward_net_carry__rate__sdiv_yield', 'discrete_forward__rate__sdiv_yield', 'discrete_forward__rate_minus_sdiv__zero_carry', 'discrete_forward__zero_rate__zero_carry', 'discrete_forward_pv__rate_minus_sdiv__zero_carry', 'discrete_forward_pv__rate_plus_sdiv__zero_carry')
  $candidateExerciseStyles = @('american_all', 'european_cash_settled_index', 'european_cash_settled_index_plus_empirical')
  $candidateTimeDecayMethods = @('analytic_derivative', 'secant_252')
  $expectedCandidateIds = @(foreach ($model in $candidateInputModels) { foreach ($style in $candidateExerciseStyles) { foreach ($method in $candidateTimeDecayMethods) { $model + '|' + $style + '|' + $method } } })
  if (-not (Test-OracleExactStringSet @($candidatePrices.candidate_id) $expectedCandidateIds)) { throw "oracle targeted gate $GateId candidate registry mismatch" }
  foreach ($candidate in $candidatePrices) {
    if (-not (Test-OracleExactKeys $candidate @('candidate_id', 'smoke_price_mae_ticks', 'smoke_count', 'tune_sample_price_mae_ticks', 'tune_sample_count')) -or
        -not ($candidate.candidate_id -is [string]) -or -not (Test-OracleFiniteNumber $candidate.smoke_price_mae_ticks) -or
        -not (Test-OracleNonnegativeInteger $candidate.smoke_count) -or [long]$candidate.smoke_count -le 0 -or
        -not (Test-OracleFiniteNumber $candidate.tune_sample_price_mae_ticks) -or -not (Test-OracleNonnegativeInteger $candidate.tune_sample_count)) { throw "oracle targeted gate $GateId candidate evidence mismatch" }
  }
  # BOTH delta arrays get the identical check — coverage, schema, the
  # `candidate - baseline == delta` arithmetic to 1e-12, and a count equal to its
  # own metric array's population. One loop, two calls: a second copy is how the
  # standard and symmetric arrays would come to disagree about what a delta is.
  foreach ($pair in @(@{ Name = 'delta'; Deltas = $sweep.metric_deltas; Metrics = $sweep.metrics },
                      @{ Name = 'symmetric delta'; Deltas = $sweep.symmetric_metric_deltas; Metrics = $sweep.symmetric_metrics })) {
    $deltas = @($pair.Deltas)
    $label = [string]$pair.Name
    if ($deltas.Count -ne 11 -or -not (Test-OracleExactStringSet @($deltas.metric_id) @($script:ModeAMetricMap.Values))) { throw "oracle targeted gate $GateId $label coverage mismatch" }
    $metricsById = @{}
    foreach ($metric in @($pair.Metrics)) { $metricsById[[string]$metric.metric_id] = $metric }
    foreach ($delta in $deltas) {
      if (-not (Test-OracleExactKeys $delta @('metric_id', 'candidate', 'baseline', 'delta', 'count', 'unit')) -or
          -not (Test-OracleFiniteNumber $delta.candidate) -or -not (Test-OracleFiniteNumber $delta.baseline) -or
          -not (Test-OracleFiniteNumber $delta.delta) -or -not (Test-OracleNonnegativeInteger $delta.count) -or [long]$delta.count -le 0) { throw "oracle targeted gate $GateId $label schema mismatch" }
      if ([Math]::Abs(([double]$delta.candidate - [double]$delta.baseline) - [double]$delta.delta) -gt 1.0e-12) { throw "oracle targeted gate $GateId $label arithmetic mismatch" }
      $metric = $metricsById[[string]$delta.metric_id]
      if (-not $metric -or [long]$delta.count -ne [long]$metric.count) { throw "oracle targeted gate $GateId $label population mismatch" }
    }
  }
  if ($GateId -eq 'residual_floor') {
    if (-not (Test-Path -LiteralPath $ExpectedFloorPath -PathType Leaf)) { throw 'residual floor receipt is missing' }
    try { $floor = [System.IO.File]::ReadAllText($ExpectedFloorPath) | ConvertFrom-Json } catch { throw 'residual floor receipt is not JSON' }
    # `production_conventions` is committed too: without it the floor records the
    # map the sweep RESOLVED but not the map production actually prices with, and
    # the two are only checked against each other while a sweep is running.
    # The committed floor carries BOTH arrays. `symmetric_metrics` is the RATCHET
    # BASELINE — the number a later iteration must not be worse than — because it
    # is the loss the scale selection minimises; `metrics` is committed beside it
    # so the floor stays directly comparable to the charter's "greeks within 1%
    # rel" target. Neither may be dropped, and they must not be unified.
    $floorKeys = @('schema_version', 'kind', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'mode', 'cohorts', 'smoke_blob_oid', 'tune_blob_oid', 'rows_processed', 'target_metric_ids', 'baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'accepted_regressions', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status', 'diagnostic_speed', 'speed')
    if (-not (Test-OracleExactKeys $floor $floorKeys) -or $floor.schema_version -ne 2 -or $floor.kind -ne 'residual_floor' -or
        $floor.command_id -ne 'mode_a_residual_floor' -or $floor.exit_code -ne 0 -or $floor.mode -ne 'A' -or
        [long]$floor.rows_processed -ne [long]$sweep.rows_priced -or -not (Test-OracleExactStringSet @($floor.cohorts) @('smoke', 'tune')) -or
        -not (Test-OracleConventionMap $floor.production_conventions) -or
        -not (Test-OracleExactStringSet @($floor.target_metric_ids) @($script:ModeAMetricMap.Values))) { throw 'residual floor receipt schema mismatch' }
    $conventionMapFields = @('baseline_conventions', 'conventions', 'production_conventions')
    foreach ($name in @('baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'accepted_regressions', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status')) {
      $floorValue = $floor.$name
      $sweepValue = $sweep.$name
      if ($conventionMapFields -contains $name) {
        # Old-format committed floors omit `exercise_style` and/or
        # `time_decay_method`; the fresh sweep always writes both. Normalize BOTH
        # sides to the explicit form, one axis at a time, so the comparison is
        # about pricing meaning and not key-set vintage.
        $floorValue = ConvertTo-OracleConventionMapWithExplicitExerciseStyle $floorValue
        $sweepValue = ConvertTo-OracleConventionMapWithExplicitExerciseStyle $sweepValue
        $floorValue = ConvertTo-OracleConventionMapWithExplicitTimeDecayMethod $floorValue
        $sweepValue = ConvertTo-OracleConventionMapWithExplicitTimeDecayMethod $sweepValue
      }
      if (-not (Test-OracleJsonValueEqual $floorValue $sweepValue)) {
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
    SymmetricMetrics = @($sweep.symmetric_metrics)
    BaselineSymmetricMetrics = @($sweep.baseline_symmetric_metrics)
    SymmetricMetricDeltas = @($sweep.symmetric_metric_deltas)
    AcceptedRegressions = @($sweep.accepted_regressions)
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

# The --aggregate-only receipt (`kind: oracle_aggregate`), read from STDOUT.
#
# The frozen Mode B command lines carry no --git-sha and no --iter, so this
# adapter deliberately does NOT compare git_sha to the tested SHA the way the
# scorecard adapters do -- the aggregate would have to fail on 'unknown', which
# is what the binary correctly writes when it was not told. Identity is instead
# pinned by Invoke-OracleTargetedGate's Get-OracleGitIdentity calls BEFORE and
# AFTER the run: the gate refuses a dirty tree and refuses a HEAD/tree that
# moved while executing, so the receipt cannot describe a different commit.
#
# ROW ACCOUNTING IS THE POINT OF THIS ADAPTER. Mode B's eleven metrics describe
# only the rows that yielded a volatility. Without the closure check below, a
# run that fitted 2% of the cohort and refused the rest would publish eleven
# flattering numbers and PASS, and nothing downstream would be able to tell.
function ConvertFrom-OracleAggregate([string]$AggregateText, [string]$GateId, [string]$ExpectedMode, [string[]]$ExpectedMetricIds) {
  try { $aggregate = $AggregateText | ConvertFrom-Json } catch { throw "oracle targeted gate $GateId aggregate is not JSON" }
  $keys = @('schema_version', 'kind', 'mode', 'cohorts', 'iter', 'git_sha', 'preset', 'quiet_host', 'rows_total', 'rows_priced', 'rows_null_sentinel', 'rows_bad_input', 'rows_engine_error', 'mode_b_fit', 'wall_seconds', 'rows_per_second', 'metrics')
  if (-not (Test-OracleExactKeys $aggregate $keys) -or $aggregate.schema_version -ne 1 -or
      $aggregate.kind -ne 'oracle_aggregate' -or $aggregate.mode -ne $ExpectedMode -or
      -not (Test-OracleExactStringSet @($aggregate.cohorts) @('smoke', 'tune'))) { throw "oracle targeted gate $GateId aggregate schema mismatch" }
  foreach ($name in @('rows_total', 'rows_priced', 'rows_null_sentinel', 'rows_bad_input', 'rows_engine_error')) {
    if (-not (Test-OracleNonnegativeInteger $aggregate.$name)) { throw "oracle targeted gate $GateId aggregate has invalid $name" }
  }
  if ([long]$aggregate.rows_priced -le 0 -or -not (Test-OracleFiniteNumber $aggregate.wall_seconds) -or [double]$aggregate.wall_seconds -le 0 -or
      -not (Test-OracleFiniteNumber $aggregate.rows_per_second) -or [double]$aggregate.rows_per_second -le 0) {
    throw "oracle targeted gate $GateId aggregate reports empty/inconsistent priced work"
  }
  # Mode B's REFUSALS, validated rather than trusted: the seven reasons must sum
  # to the published total (so a reason cannot be dropped from the sum), and at
  # least one group must have been fitted.
  $fit = $aggregate.mode_b_fit
  $fitKeys = @('groups_fitted', 'rows_unfitted', 'rows_no_quote', 'rows_below_lo_bound', 'rows_above_up_bound', 'rows_iv_no_solution', 'rows_iv_at_floor', 'rows_vega_below_floor', 'rows_round_trip_failed')
  if (-not (Test-OracleExactKeys $fit $fitKeys)) { throw "oracle targeted gate $GateId mode_b_fit schema mismatch" }
  foreach ($name in $fitKeys) {
    if (-not (Test-OracleNonnegativeInteger $fit.$name)) { throw "oracle targeted gate $GateId mode_b_fit has invalid $name" }
  }
  if ([long]$fit.groups_fitted -le 0) { throw "oracle targeted gate $GateId fitted no underlier x expiry x bucket group" }
  $reasonSum = 0L
  foreach ($name in @('rows_no_quote', 'rows_below_lo_bound', 'rows_above_up_bound', 'rows_iv_no_solution', 'rows_iv_at_floor', 'rows_vega_below_floor', 'rows_round_trip_failed')) {
    $reasonSum += [long]$fit.$name
  }
  if ($reasonSum -ne [long]$fit.rows_unfitted) { throw "oracle targeted gate $GateId mode_b_fit reasons do not sum to rows_unfitted" }
  # THE CLOSURE. Every row the cohort selected is priced, skipped at the reader,
  # refused by the engine, or refused by the fit -- in exactly one bucket.
  $accounted = [long]$aggregate.rows_priced + [long]$aggregate.rows_null_sentinel + [long]$aggregate.rows_bad_input +
               [long]$aggregate.rows_engine_error + [long]$fit.rows_unfitted
  if ([long]$aggregate.rows_total -ne $accounted) {
    throw ("oracle targeted gate $GateId aggregate row accounting does not close: rows_total=" + [long]$aggregate.rows_total + ' accounted=' + $accounted)
  }
  $metrics = @($aggregate.metrics)
  if ($metrics.Count -ne $ExpectedMetricIds.Count -or -not (Test-OracleExactStringSet @($metrics.metric_id) $ExpectedMetricIds)) {
    throw "oracle targeted gate $GateId aggregate metric coverage mismatch"
  }
  foreach ($metric in $metrics) {
    if (-not (Test-OracleExactKeys $metric @('metric_id', 'value', 'count', 'unit')) -or -not (Test-OracleFiniteNumber $metric.value) -or
        [double]$metric.value -lt 0 -or -not (Test-OracleNonnegativeInteger $metric.count) -or [long]$metric.count -le 0) {
      throw "oracle targeted gate $GateId aggregate metric schema mismatch"
    }
    $wantedUnit = if ($metric.metric_id -like '*_price_mae') { 'ticks' } elseif ($metric.metric_id -like '*_vol_mae') { 'bp' } else { 'relative' }
    if ($metric.unit -ne $wantedUnit) { throw "oracle targeted gate $GateId aggregate metric unit mismatch on $($metric.metric_id)" }
  }
  # CONFIDENTIALITY, checked at the gate and not only in the C++ tests: the
  # --aggregate-only receipt is what reaches the tool-less analyst stage, and
  # partition names ARE membership. A raw-text scan catches a leak arriving in a
  # field this adapter's key check does not yet know about.
  foreach ($leak in @('date=', 'bucket_et=', 'within_tol_rate', 'deep-itm', 'partitions')) {
    if ($AggregateText.Contains($leak)) { throw "oracle targeted gate $GateId aggregate leaked membership: $leak" }
  }
  return [pscustomobject]@{
    RowsProcessed = [long]$aggregate.rows_priced
    RowsTotal = [long]$aggregate.rows_total
    EngineErrors = [long]$aggregate.rows_engine_error
    MetricIds = @($metrics.metric_id | ForEach-Object { [string]$_ })
    ModeBMetrics = $metrics
    ModeBFit = $fit
  }
}

function ConvertFrom-OracleBenchScorecard([string]$ScorecardText, [string]$GateId, $Identity) {
  $spec = Get-OracleTargetedGateSpec $GateId $Identity
  if ($GateId -in @('mode_a_smoke_tune', 'residual_floor')) { return ConvertFrom-OracleConventionSweep $ScorecardText $GateId $Identity $spec.ExpectedFloorPath }
  if ($GateId -in @('convention_speed_measure', 'convention_speed')) { return ConvertFrom-OracleSpeed $ScorecardText $Identity $spec.ExpectedFloorPath }
  if ($GateId -eq 'mode_b_smoke_tune') { return ConvertFrom-OracleAggregate $ScorecardText $GateId 'B' @(Get-OracleRequiredMetricIds $GateId) }
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
    # 'oracle_aggregate' reads the licensed store like the kinds above but
    # publishes to STDOUT, so it deliberately shares the store check and skips
    # the output-file preparation entirely -- `Split-Path -Parent ''` is a
    # terminating error under this script's Stop policy, which is exactly why
    # this is a separate kind rather than an empty-path special case inside the
    # branch above.
    if ($spec.Kind -eq 'oracle_aggregate') {
      if (-not (Test-Path -LiteralPath $script:OracleStoreRoot -PathType Container)) { throw 'licensed aggregate oracle store is missing' }
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
    } elseif ($spec.Kind -eq 'oracle_aggregate') {
      # STDOUT receipt. The captured stream interleaves the aggregate JSON with
      # the bench's own stderr progress lines (Invoke-OracleNativeProcess merges
      # both by design, so a native process's ordinary stderr does not abort the
      # gate), so the document is carved out by its own delimiters rather than
      # assumed to be the whole stream: first '{' through last '}'. Both must
      # exist and be ordered, and what is between them must parse -- a run that
      # printed only progress lines fails here instead of reaching the adapter.
      $open = $raw.IndexOf('{')
      $close = $raw.LastIndexOf('}')
      if ($open -lt 0 -or $close -le $open) { throw "oracle targeted gate $GateId produced no aggregate JSON on stdout" }
      $scorecardText = $raw.Substring($open, $close - $open + 1)
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
    # The symmetric trio is carried too: it is the array the no-regression gate
    # ruled on and the array a later iteration ratchets against, so a reviewer
    # must see it in the receipt rather than only inside the sweep artifact.
    $result.symmetric_metrics = @($aggregate.SymmetricMetrics)
    $result.baseline_symmetric_metrics = @($aggregate.BaselineSymmetricMetrics)
    $result.symmetric_metric_deltas = @($aggregate.SymmetricMetricDeltas)
    # Every regression the bound permitted, published in the typed receipt too:
    # a permitted loss that lives only inside a 12-minute run's artifact is a
    # silent one, and the whole point of the bound is that it never is.
    $result.accepted_regressions = @($aggregate.AcceptedRegressions)
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
  # Mode B's eleven targets and its fit accounting, carried into the typed
  # receipt. The refusal counts travel WITH the metrics on purpose: the eleven
  # numbers describe only the rows that yielded a volatility, and a reader who
  # cannot see how many rows did not is being shown a filtered sample without
  # being told. They are whole-run counts, so they carry no membership.
  if ($aggregate -and $aggregate.PSObject.Properties.Name -contains 'ModeBMetrics') {
    $result.rows_total = [long]$aggregate.RowsTotal
    $result.engine_errors = [long]$aggregate.EngineErrors
    $result.mode_b_metrics = @($aggregate.ModeBMetrics)
    $result.mode_b_fit = $aggregate.ModeBFit
  }
  return $result
}

# Pester imports the production functions and injects only the process invoker.
if ($MyInvocation.InvocationName -eq '.') { return }
if (-not $Gate) { throw '-Gate is required' }
Invoke-OracleTargetedGate $Gate $null | ConvertTo-Json -Compress
