$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$scriptPath = Join-Path (Split-Path -Parent $here) 'oracle-capability.ps1'
. $scriptPath

function Write-JsonFile([string]$Path, $Value) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
  $Value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}

function Commit-All([string]$Repository, [string]$Message) {
  git -C $Repository add -- . | Out-Null
  git -C $Repository commit -m $Message --quiet
  return (git -C $Repository rev-parse HEAD).Trim()
}

function New-TestConventionMap {
  return [ordered]@{
    input_model = 'discrete_forward_pv__rate__sdiv_yield'; forward_formula = 'uprc_exp_rate_t_minus_ddiv'
    rate_model = 'continuous_row_rate'; carry_model = 'sdiv_as_yield'; dividend_model = 'discrete_cash_forward'
    day_count = 'ACT_365_25'; dte_banding_day_count = 'ACT_365F'
    price_scale = 'per_share'; price_sign = 'positive'; vol_scale = 'decimal_identity'
    delta_scale = 'per_unit'; delta_sign = 'positive'; gamma_scale = 'per_unit'; gamma_sign = 'positive'
    theta_basis = 'per_day'; theta_sign = 'positive'; vega_scale = 'per_point'; vega_sign = 'positive'
    rho_scale = 'per_point'; rho_sign = 'positive'; phi_scale = 'per_point_squared'; phi_sign = 'positive'
    volga_source = 'volga'; volga_scale = 'per_point_squared'; volga_sign = 'positive'
    vanna_source = 'vanna'; vanna_scale = 'per_point'; vanna_sign = 'positive'
    delta_decay_basis = 'per_day'; delta_decay_day_count = 'ACT_365_25'; delta_decay_sign = 'positive'
  }
}

function New-TestFloorMetrics([double]$Offset = 0.0) {
  return @($targetA | ForEach-Object {
    [ordered]@{
      metric_id = $_; value = 1.0 + $Offset; count = 100; selection_count = 90
      unit = if ($_ -eq 'mode_a_price_mae') { 'ticks' } elseif ($_ -eq 'mode_a_vol_mae') { 'bp' } else { 'relative' }
    }
  })
}

function New-TestMetricDeltas {
  return @($targetA | ForEach-Object {
    [ordered]@{
      metric_id = $_; candidate = 1.0; baseline = 2.0; delta = -1.0; count = 100
      unit = if ($_ -eq 'mode_a_price_mae') { 'ticks' } elseif ($_ -eq 'mode_a_vol_mae') { 'bp' } else { 'relative' }
    }
  })
}

# One candidate-price array of an arbitrary grid rung: $Ids candidates of which
# the leading $Finalists carry a positive tune_sample_count.
function New-TestCandidateGrid([string[]]$Ids, [int]$Finalists) {
  return @(for ($index = 0; $index -lt $Ids.Count; $index++) {
    [ordered]@{ candidate_id = $Ids[$index]; smoke_price_mae_ticks = 10.0 + $index; smoke_count = 20; tune_sample_price_mae_ticks = if ($index -lt $Finalists) { 11.0 + $index } else { 0.0 }; tune_sample_count = if ($index -lt $Finalists) { 10 } else { 0 } }
  })
}

# The PRE-AXIS 8-candidate / 2-finalist rung, kept deliberately. The committed
# bootstrap receipts this probe must go on validating were written before the
# exercise-style and time-decay axes existed, so this is a real accepted case,
# not a stale one — Test-CandidatePrices names it as the third accepted rung.
# What it did NOT do was exercise the CURRENT grid: see 'accepts the current
# three-axis grid...' below for 48/12.
function New-TestCandidatePrices {
  $ids = @('uprc_spot__rate__sdiv_yield', 'discrete_forward_pv__rate__sdiv_yield', 'discrete_forward_net_carry__rate__sdiv_yield', 'discrete_forward__rate__sdiv_yield', 'discrete_forward__rate_minus_sdiv__zero_carry', 'discrete_forward__zero_rate__zero_carry', 'discrete_forward_pv__rate_minus_sdiv__zero_carry', 'discrete_forward_pv__rate_plus_sdiv__zero_carry')
  return New-TestCandidateGrid $ids 2
}

# Test-CandidatePrices reads receipts, so the fixtures it is handed must be
# round-tripped through JSON: Test-ExactKeys inspects PSObject properties, which
# an [ordered] hashtable does not expose the way a parsed receipt does.
function ConvertTo-Receipt($Value) {
  return @(ConvertFrom-Json (ConvertTo-Json -InputObject @($Value) -Depth 8))
}

# The axis-size AUTHORITY: the three static_assert-guarded std::array declarations
# in atx-vol/tools/oracle_convention_sweep.cpp, read back from source. The C++
# static_asserts already pin each array to its enum, so the declared size is the
# enum cardinality.
function Get-SweepAxisSize([string]$Source, [string]$Type, [string]$Name) {
  $found = [regex]::Match($Source, 'std::array<\s*' + $Type + '\s*,\s*(\d+)\s*>\s*' + $Name + '\b')
  if (-not $found.Success) { throw ('atx-vol/tools/oracle_convention_sweep.cpp no longer declares ' + $Name) }
  return [int]$found.Groups[1].Value
}

Describe 'oracle capability candidate grid rungs' {
  It 'accepts the current three-axis grid, still accepts the pre-axis rung, and rejects everything between' {
    # The CURRENT grid, crossed from the same three axis domains
    # scripts/oracle-capability.ps1 derives $script:OracleAcceptedCandidateGrids
    # from. Nothing fed Test-CandidatePrices a 48/12 receipt before this: the
    # only fixture was the pre-axis 8/2 rung, which stayed green precisely
    # because the probe still accepts it, so the current grid went uncovered.
    $currentIds = @(foreach ($model in $script:OracleInputModels) {
      foreach ($style in $script:OracleExerciseStyles) {
        foreach ($method in $script:OracleTimeDecayMethods) { $model + '|' + $style + '|' + $method }
      }
    })
    $currentIds.Count | Should Be $script:OracleCandidateCount
    (Test-CandidatePrices (ConvertTo-Receipt (New-TestCandidateGrid $currentIds $script:OracleFinalistCount))) | Should Be $true
    # The pre-axis rung the committed bootstrap receipts still carry: retiring it
    # means regenerating those receipts in the same commit, so it must stay green.
    (Test-CandidatePrices (ConvertTo-Receipt (New-TestCandidatePrices))) | Should Be $true
    # ...and so must the two legacy rungs between them, both stated against the
    # PRE-TREE input-model count: the models the committed receipts were swept
    # over are the leading entries of the now-wider list (see
    # $script:OraclePreTreeInputModelCount in scripts/oracle-capability.ps1 —
    # deriving these rungs from the 9-wide list would silently turn them into
    # grids no committed receipt ever carried).
    $preTreeModels = @($script:OracleInputModels | Select-Object -First $script:OraclePreTreeInputModelCount)
    $preTreeThreeAxisIds = @(foreach ($model in $preTreeModels) {
      foreach ($style in $script:OracleExerciseStyles) {
        foreach ($method in $script:OracleTimeDecayMethods) { $model + '|' + $style + '|' + $method }
      }
    })
    (Test-CandidatePrices (ConvertTo-Receipt (New-TestCandidateGrid $preTreeThreeAxisIds $script:OracleFinalistCount))) | Should Be $true
    $twoAxisIds = @(foreach ($model in $preTreeModels) { foreach ($style in $script:OracleExerciseStyles) { $model + '|' + $style } })
    (Test-CandidatePrices (ConvertTo-Receipt (New-TestCandidateGrid $twoAxisIds (2 * $script:OracleExerciseStyleCount)))) | Should Be $true
    # A full current-grid receipt carrying the PRE-AXIS finalist count is a tied
    # block sliced in half, not a rung: the rung is matched on candidate count,
    # so its finalist count is the only one that may appear beside it.
    (Test-CandidatePrices (ConvertTo-Receipt (New-TestCandidateGrid $currentIds 2))) | Should Be $false
    # An off-grid candidate count matches no rung at all, which is what stops a
    # silently narrowed search from being adopted as a valid receipt.
    (Test-CandidatePrices (ConvertTo-Receipt (New-TestCandidateGrid @($currentIds | Select-Object -First ($currentIds.Count - 1)) $script:OracleFinalistCount))) | Should Be $false
    # Duplicate ids never pass, at any rung.
    $duplicated = @(New-TestCandidateGrid $currentIds $script:OracleFinalistCount)
    $duplicated[$duplicated.Count - 1].candidate_id = $duplicated[0].candidate_id
    (Test-CandidatePrices (ConvertTo-Receipt $duplicated)) | Should Be $false
  }

  It 'pins the accepted grid arithmetic to the C++ sweep grid it must move with' {
    # scripts/oracle-capability.ps1 says these move in ONE commit with the C++.
    # Read both sides from source so widening an axis in
    # atx-vol/tools/oracle_convention_sweep.cpp without widening this probe fails
    # here rather than by rejecting a perfectly good receipt at a later layer.
    $sweep = [System.IO.File]::ReadAllText((Join-Path $here '..\..\atx-vol\tools\oracle_convention_sweep.cpp'))
    $inputModels = Get-SweepAxisSize $sweep 'InputModel' 'kInputModels'
    $exerciseStyles = Get-SweepAxisSize $sweep 'ExerciseStyleRule' 'kExerciseStyleRules'
    $timeDecayMethods = Get-SweepAxisSize $sweep 'TimeDecayMethod' 'kTimeDecayMethods'
    $script:OracleInputModelCount | Should Be $inputModels
    $script:OracleExerciseStyleCount | Should Be $exerciseStyles
    $script:OracleTimeDecayMethodCount | Should Be $timeDecayMethods
    $script:OracleTiedArmsPerInputModel | Should Be ($exerciseStyles * $timeDecayMethods)
    $script:OracleCandidateCount | Should Be ($inputModels * $exerciseStyles * $timeDecayMethods)
    $script:OracleFinalistCount | Should Be (2 * $exerciseStyles * $timeDecayMethods)
    # The CURRENT grid must be the newest accepted rung, and the older rungs must
    # still be there: dropping one silently invalidates committed receipts. The
    # current rung is derived from the C++ axis sizes read above; the legacy
    # rungs from the script's own pre-tree count, never from the now-wider model
    # list (see $script:OraclePreTreeInputModelCount).
    $expectedGrids = @(
      @{ Candidates = ($inputModels * $exerciseStyles * $timeDecayMethods); Finalists = (2 * $exerciseStyles * $timeDecayMethods) },
      @{ Candidates = ($script:OraclePreTreeInputModelCount * $exerciseStyles * $timeDecayMethods); Finalists = (2 * $exerciseStyles * $timeDecayMethods) },
      @{ Candidates = ($script:OraclePreTreeInputModelCount * $exerciseStyles); Finalists = (2 * $exerciseStyles) },
      @{ Candidates = $script:OraclePreTreeInputModelCount; Finalists = 2 }
    )
    $script:OracleAcceptedCandidateGrids.Count | Should Be $expectedGrids.Count
    for ($rung = 0; $rung -lt $expectedGrids.Count; $rung++) {
      [int]$script:OracleAcceptedCandidateGrids[$rung].Candidates | Should Be ([int]$expectedGrids[$rung].Candidates)
      [int]$script:OracleAcceptedCandidateGrids[$rung].Finalists | Should Be ([int]$expectedGrids[$rung].Finalists)
    }
  }
}

Describe 'oracle capability closed aggregate receipts' {
  It 'validates every state transition and fails closed on corrupt content/provenance' {
    $repoRoot = Join-Path $TestDrive 'repo'
    $dataRoot = Join-Path $TestDrive 'data'
    $oracleRoot = 'atx-vol/bench/oracle'
    New-Item -ItemType Directory -Force $repoRoot, $dataRoot | Out-Null
    git -C $repoRoot init --quiet
    git -C $repoRoot config user.email 'pester@example.invalid'
    git -C $repoRoot config user.name 'Pester'
    Set-Content -LiteralPath (Join-Path $repoRoot 'base.txt') -Value 'base' -Encoding ASCII
    $baseSha = Commit-All $repoRoot 'base'

    $manifestName = 'oracle_manifest_2026-08-14.json'
    $manifestPath = Join-Path $dataRoot $manifestName
    Write-JsonFile $manifestPath ([ordered]@{
      trading_date = '2026-08-14'; source_tsv_bytes = 1000; total_rows = 30
      buckets = [ordered]@{ '1000' = 10; '1330' = 20 }
      top_underliers_by_rows = [ordered]@{ SPY = 30 }
      ingested_at = '2026-08-15T12:00:00Z'
    })
    $manifestDigest = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $cohortRoot = Join-Path $repoRoot ($oracleRoot + '/cohorts')
    Write-JsonFile (Join-Path $cohortRoot 'smoke.json') ([ordered]@{ name = 'smoke'; dates = @('2026-08-14'); underliers = @('SPY'); buckets_et = @('1000'); notes = 'aggregate smoke' })
    Write-JsonFile (Join-Path $cohortRoot 'tune.json') ([ordered]@{ name = 'tune'; dates = @('2026-08-14'); underliers = @('QQQ'); buckets_et = @('1330'); notes = 'aggregate tune' })
    $holdoutCohort = [ordered]@{ name = 'holdout'; dates = @('2026-08-14'); underliers = @('IWM'); buckets_et = @('1500'); notes = 'fixed probe hashes membership internally' }
    Write-JsonFile (Join-Path $cohortRoot 'holdout.json') $holdoutCohort
    $digest = Get-CohortMembershipDigest ([pscustomobject]$holdoutCohort)
    Set-Content -LiteralPath (Join-Path $cohortRoot 'holdout.sha256') -Value $digest -Encoding ASCII
    $dataTested = Commit-All $repoRoot 'cohort artifacts'
    $smokeOid = Get-BlobOid $dataTested ($oracleRoot + '/cohorts/smoke.json')
    $tuneOid = Get-BlobOid $dataTested ($oracleRoot + '/cohorts/tune.json')
    $holdoutOid = Get-BlobOid $dataTested ($oracleRoot + '/cohorts/holdout.json')
    $bootstrapRoot = Join-Path $repoRoot ($oracleRoot + '/bootstrap')
    $dataReceipt = [ordered]@{
      schema_version = 1; transition = 'data'; base_sha = $baseSha; tested_sha = $dataTested
      command_id = 'oracle_ingest_and_cohort_validate'; exit_code = 0
      ingest_manifest_name = $manifestName; ingest_manifest_sha256 = $manifestDigest
      smoke_blob_oid = $smokeOid; tune_blob_oid = $tuneOid; holdout_blob_oid = $holdoutOid; holdout_membership_sha256 = $digest
      smoke_schema_valid = $true; tune_schema_valid = $true; holdout_schema_valid = $true
      tune_holdout_underliers_disjoint = $true; tune_holdout_buckets_disjoint = $true
    }
    Write-JsonFile (Join-Path $bootstrapRoot 'data.json') $dataReceipt
    $dataCommit = Commit-All $repoRoot 'data receipt'
    $loadedData = Get-CommittedJson $dataCommit ($oracleRoot + '/bootstrap/data.json')
    (Test-Provenance $loadedData $dataCommit '') | Should Be $true
    (Get-BlobOid $dataCommit ($oracleRoot + '/cohorts/smoke.json')) | Should Be $smokeOid
    (Get-BlobOid $dataCommit ($oracleRoot + '/cohorts/tune.json')) | Should Be $tuneOid
    (Test-CohortJson (Get-CommittedJson $dataCommit ($oracleRoot + '/cohorts/smoke.json')) 'smoke') | Should Be $true
    (Test-CohortJson (Get-CommittedJson $dataCommit ($oracleRoot + '/cohorts/tune.json')) 'tune') | Should Be $true
    $loadedManifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    (Test-ExactKeys $loadedManifest @('trading_date', 'source_tsv_bytes', 'total_rows', 'buckets', 'top_underliers_by_rows', 'ingested_at')) | Should Be $true
    $manifestName -match '^oracle_manifest_(\d{4}-\d{2}-\d{2})\.json$' | Should Be $true
    $loadedManifest.trading_date | Should Be '2026-08-14'
    (@($loadedManifest.top_underliers_by_rows.PSObject.Properties).Count -ge 1) | Should Be $true
    $sum = 0L
    foreach ($property in $loadedManifest.buckets.PSObject.Properties) {
      ($property.Name -match '^(?:0[0-9]|1[0-9]|2[0-3])[0-5][0-9]$') | Should Be $true
      ([long]$property.Value -gt 0) | Should Be $true
      $sum += [long]$property.Value
    }
    $sum | Should Be 30
    foreach ($property in $loadedManifest.top_underliers_by_rows.PSObject.Properties) {
      ($property.Name -match '^[A-Z0-9._-]{1,24}$') | Should Be $true
      ([long]$property.Value -le 30) | Should Be $true
    }
    $parsed = [datetime]::MinValue
    ([datetime]::TryParse([string]$loadedManifest.ingested_at, [ref]$parsed)) | Should Be $true
    (Test-IngestManifest $manifestName $manifestDigest) | Should Be $true
    $actualDigest = ''
    (Test-DataReceipt $dataCommit ([ref]$actualDigest)) | Should Be $true
    $actualDigest | Should Be $digest

    $modeAReceipt = [ordered]@{
      schema_version = 1; transition = 'mode_a'; base_sha = $dataCommit; tested_sha = $dataCommit
      command_id = 'oracle_mode_a_aggregate'; exit_code = 0; smoke_blob_oid = $smokeOid
      rows_processed = 100; target_metric_ids = $targetA
    }
    Write-JsonFile (Join-Path $bootstrapRoot 'mode-a.json') $modeAReceipt
    $modeACommit = Commit-All $repoRoot 'mode A receipt'
    $currentData = Get-CommittedJson $modeACommit ($oracleRoot + '/bootstrap/data.json')
    (Test-ModeReceipt $modeACommit 'A' $currentData) | Should Be $true

    $conventionsPath = Join-Path $repoRoot ($oracleRoot + '/CONVENTIONS.md')
    New-Item -ItemType Directory -Force (Split-Path -Parent $conventionsPath) | Out-Null
    Set-Content -LiteralPath $conventionsPath -Value '# Closed conventions v1' -Encoding UTF8
    $scorecardPath = Join-Path $repoRoot ($oracleRoot + '/scorecards/iter-000.json')
    $conventionMap = New-TestConventionMap
    $baselineMap = New-TestConventionMap
    $metrics = New-TestFloorMetrics
    $baselineMetrics = New-TestFloorMetrics 1.0
    $metricDeltas = New-TestMetricDeltas
    # The symmetric-relative trio committed beside the standard one. It is the
    # ratchet baseline and the no-regression criterion; the standard array is
    # committed for comparability with the charter target only.
    $symmetricMetrics = New-TestFloorMetrics
    $baselineSymmetricMetrics = New-TestFloorMetrics 1.0
    $symmetricMetricDeltas = New-TestMetricDeltas
    $candidatePrices = New-TestCandidatePrices
    $speed = [ordered]@{ metric_id = 'rel_avx2_rows_per_second'; baseline = 1000.0; pin = 900.0; unit = 'rows_per_second'; preset = 'rel-avx2'; quiet_host = $true }
    Write-JsonFile $scorecardPath ([ordered]@{
      schema_version = 2; kind = 'residual_floor'; base_sha = $modeACommit; tested_sha = $modeACommit
      command_id = 'mode_a_residual_floor'; exit_code = 0; mode = 'A'; cohorts = @('smoke', 'tune')
      smoke_blob_oid = $smokeOid; tune_blob_oid = $tuneOid; rows_processed = 100; target_metric_ids = $targetA
      baseline_conventions = $baselineMap; conventions = $conventionMap; production_conventions = (New-TestConventionMap)
      metrics = $metrics; baseline_metrics = $baselineMetrics
      metric_deltas = $metricDeltas
      symmetric_metrics = $symmetricMetrics; baseline_symmetric_metrics = $baselineSymmetricMetrics
      symmetric_metric_deltas = $symmetricMetricDeltas; accepted_regressions = @()
      candidate_prices = $candidatePrices; input_model_regressed_greeks = @(); oracle_suspect_candidates = @()
      market_evidence_status = 'not_evaluated_no_nbbo_gate'; diagnostic_speed = [ordered]@{ preset = 'dev'; citable = $false; wall_seconds = 1.0; rows_per_second = 100.0 }; speed = $speed
    })
    $conventionsTested = Commit-All $repoRoot 'convention artifacts'
    $conventionsReceipt = [ordered]@{
      schema_version = 2; transition = 'conventions'; base_sha = $modeACommit; tested_sha = $conventionsTested
      command_id = 'oracle_conventions_smoke_tune'; exit_code = 0; smoke_blob_oid = $smokeOid; tune_blob_oid = $tuneOid
      conventions_blob_oid = (Get-BlobOid $conventionsTested ($oracleRoot + '/CONVENTIONS.md'))
      scorecard_blob_oid = (Get-BlobOid $conventionsTested ($oracleRoot + '/scorecards/iter-000.json'))
      rows_processed = 100; target_metric_ids = $targetA; baseline_conventions = $baselineMap; conventions = $conventionMap
      production_conventions = (New-TestConventionMap)
      metrics = $metrics; baseline_metrics = $baselineMetrics; metric_deltas = $metricDeltas
      symmetric_metrics = $symmetricMetrics; baseline_symmetric_metrics = $baselineSymmetricMetrics
      symmetric_metric_deltas = $symmetricMetricDeltas; accepted_regressions = @(); candidate_prices = $candidatePrices
      input_model_regressed_greeks = @(); speed = $speed
    }
    Write-JsonFile (Join-Path $bootstrapRoot 'conventions.json') $conventionsReceipt
    $conventionsCommit = Commit-All $repoRoot 'conventions receipt'
    $currentData = Get-CommittedJson $conventionsCommit ($oracleRoot + '/bootstrap/data.json')
    (Test-ConventionsReceipt $conventionsCommit $currentData) | Should Be $true

    # Bounded no-regression gate on the committed floor: equality passes (vol is
    # structurally 0 on both arms), and so does an improvement, both with an
    # empty accepted_regressions.
    (Test-FloorNoRegression $metrics $baselineMetrics @()) | Should Be $true
    (Test-FloorNoRegression $metrics $metrics @()) | Should Be $true
    # baselineMetrics is 2.0 against a 1.0 baseline: far past the 1% bound.
    (Test-FloorNoRegression $baselineMetrics $metrics @()) | Should Be $false

    # 0.5% of baseline is INSIDE the 1% bound, so it passes — but only while it
    # is published. The same receipt with an empty array fails closed, which is
    # what keeps accepted_regressions from becoming a rubber stamp.
    $withinBound = New-TestFloorMetrics
    foreach ($metric in $withinBound) { if ($metric.metric_id -eq 'mode_a_vega_rel') { $metric.value = 1.005 } }
    $publishedWithin = @([ordered]@{ metric_id = 'mode_a_vega_rel'; candidate = 1.005; baseline = 1.0; pct_of_baseline = 0.005 })
    (Test-FloorNoRegression $withinBound $metrics ([pscustomobject[]]@($publishedWithin | ForEach-Object { [pscustomobject]$_ }))) | Should Be $true
    (Test-FloorNoRegression $withinBound $metrics @()) | Should Be $false
    # 2% of baseline is twice the bound: fails closed however it is published.
    $beyondBound = New-TestFloorMetrics
    foreach ($metric in $beyondBound) { if ($metric.metric_id -eq 'mode_a_vega_rel') { $metric.value = 1.02 } }
    (Test-FloorNoRegression $beyondBound $metrics @()) | Should Be $false
    $publishedBeyond = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; candidate = 1.02; baseline = 1.0; pct_of_baseline = 0.02 })
    (Test-FloorNoRegression $beyondBound $metrics $publishedBeyond) | Should Be $false
    # An entry for a metric that did not regress at all is equally closed.
    $forged = @([pscustomobject]@{ metric_id = 'mode_a_vega_rel'; candidate = 1.0; baseline = 1.0; pct_of_baseline = 0.0 })
    (Test-FloorNoRegression $metrics $metrics $forged) | Should Be $false
    (Test-InputModelRegressedGreeks @()) | Should Be $true
    (Test-InputModelRegressedGreeks @('mode_a_phi_rel', 'mode_a_delta_decay_rel')) | Should Be $true
    (Test-InputModelRegressedGreeks @('mode_a_price_mae')) | Should Be $false
    (Test-InputModelRegressedGreeks @('mode_a_phi_rel', 'mode_a_phi_rel')) | Should Be $false

    $modeBReceipt = [ordered]@{
      schema_version = 1; transition = 'mode_b'; base_sha = $conventionsCommit; tested_sha = $conventionsCommit
      command_id = 'oracle_mode_b_aggregate'; exit_code = 0; smoke_blob_oid = $smokeOid; tune_blob_oid = $tuneOid
      rows_processed = 100; target_metric_ids = $targetB
    }
    Write-JsonFile (Join-Path $bootstrapRoot 'mode-b.json') $modeBReceipt
    $readyCommit = Commit-All $repoRoot 'mode B receipt'
    $currentData = Get-CommittedJson $readyCommit ($oracleRoot + '/bootstrap/data.json')
    (Test-ModeReceipt $readyCommit 'B' $currentData) | Should Be $true

    Write-JsonFile $manifestPath ([ordered]@{
      trading_date = '2026-08-14'; source_tsv_bytes = 1000; total_rows = 31
      buckets = [ordered]@{ '1000' = 10; '1330' = 20 }; top_underliers_by_rows = [ordered]@{ SPY = 30 }; ingested_at = '2026-08-15T12:00:00Z'
    })
    (Test-DataReceipt $readyCommit ([ref]$actualDigest)) | Should Be $false
    Write-JsonFile $manifestPath ([ordered]@{
      trading_date = '2026-08-14'; source_tsv_bytes = 1000; total_rows = 30
      buckets = [ordered]@{ '1000' = 10; '1330' = 20 }; top_underliers_by_rows = [ordered]@{ SPY = 30 }; ingested_at = '2026-08-15T12:00:00Z'
    })

    $modeBReceipt.target_metric_ids = @('mode_b_price_mae')
    Write-JsonFile (Join-Path $bootstrapRoot 'mode-b.json') $modeBReceipt
    $corruptCommit = Commit-All $repoRoot 'corrupt mode B receipt'
    (Test-ModeReceipt $corruptCommit 'B' (Get-CommittedJson $corruptCommit ($oracleRoot + '/bootstrap/data.json'))) | Should Be $false

    Write-JsonFile (Join-Path $cohortRoot 'holdout.json') ([ordered]@{ name = 'holdout'; dates = @('2026-08-15'); underliers = @('DIA'); buckets_et = @('1515'); notes = 'tampered membership' })
    $holdoutTamperCommit = Commit-All $repoRoot 'tamper holdout blob'
    (Test-DataReceipt $holdoutTamperCommit ([ref]$actualDigest)) | Should Be $false

    $conventionsReceipt.command_id = 'unrecognized_command'
    Write-JsonFile (Join-Path $bootstrapRoot 'conventions.json') $conventionsReceipt
    $conventionsCorruptCommit = Commit-All $repoRoot 'corrupt conventions receipt'
    (Test-ConventionsReceipt $conventionsCorruptCommit (Get-CommittedJson $conventionsCorruptCommit ($oracleRoot + '/bootstrap/data.json'))) | Should Be $false

    $dataReceipt['unexpected'] = 'legacy'
    Write-JsonFile (Join-Path $bootstrapRoot 'data.json') $dataReceipt
    $dataCorruptCommit = Commit-All $repoRoot 'corrupt data schema'
    (Test-DataReceipt $dataCorruptCommit ([ref]$actualDigest)) | Should Be $false

    $forgedDigest = 'e' * 64
    $dataReceipt.Remove('unexpected')
    $dataReceipt.holdout_membership_sha256 = $forgedDigest
    Set-Content -LiteralPath (Join-Path $cohortRoot 'holdout.sha256') -Value $forgedDigest -Encoding ASCII
    Write-JsonFile (Join-Path $bootstrapRoot 'data.json') $dataReceipt
    $forgedDigestCommit = Commit-All $repoRoot 'forged arbitrary digest'
    (Test-DataReceipt $forgedDigestCommit ([ref]$actualDigest)) | Should Be $false
  }
}
