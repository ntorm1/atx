# Closed, aggregate-only capability probe for the SpiderRock oracle workflow.
# It validates versioned bootstrap receipts and provenance. The fixed probe alone
# opens committed cohort manifests to recompute membership integrity; stdout is
# state plus booleans/digest only and never contains membership or licensed rows.
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$canonicalRef = 'refs/heads/oracle/canonical'
$baseRef = 'main'
$dataRoot = 'C:\atx-cache\oracle\spiderrock'
$oracleRoot = 'atx-vol/bench/oracle'
$targetA = @('mode_a_price_mae', 'mode_a_vol_mae', 'mode_a_delta_rel', 'mode_a_gamma_rel', 'mode_a_theta_rel', 'mode_a_vega_rel', 'mode_a_rho_rel', 'mode_a_phi_rel', 'mode_a_volga_rel', 'mode_a_vanna_rel', 'mode_a_delta_decay_rel')
$targetB = @('mode_b_price_mae', 'mode_b_vol_mae', 'mode_b_delta_rel', 'mode_b_gamma_rel', 'mode_b_theta_rel', 'mode_b_vega_rel', 'mode_b_rho_rel', 'mode_b_phi_rel', 'mode_b_volga_rel', 'mode_b_vanna_rel', 'mode_b_delta_decay_rel')
# The BOUNDED no-regression rule, as a multiplier on the baseline value. Stated
# as the multiplier and not as `1 + fraction` because five layers in three
# languages re-evaluate this same comparison and `1.0 + 0.01` is not required to
# be the same double as `1.01`. Mirrors kRegressionBoundMultiplier in
# atx-vol/tools/oracle_convention_sweep.hpp, which carries the full rationale.
$regressionBoundMultiplier = 1.01

# ── the stage-1 candidate grid, stated ONCE ──────────────────────────────────
# The sweep searches the CROSS PRODUCT of three convention axes, so the candidate
# count is a PRODUCT and never a number anyone should be typing by hand. The grid
# has already moved twice — 8 -> 24 when the exercise-style axis landed (85797d0f
# pinned the 24-candidate registry) and 24 -> 48 when the time-decay axis landed
# (635f8bd8) — and this file tracked NEITHER, because Test-CandidatePrices
# restated the arithmetic as bare literals with nothing linking them to the grid.
# The three axis DOMAINS below are now the single
# source for both the enum checks in Test-ConventionMap and these counts.
#
# The AUTHORITATIVE values are the static_assert-guarded constants in
# atx-vol/tools/oracle_convention_sweep.cpp:
#   kCandidateCount        = kInputModels.size() * kExerciseStyleRules.size() * kTimeDecayMethods.size()
#   kTiedArmsPerInputModel = kExerciseStyleRules.size() * kTimeDecayMethods.size()
#   kFinalistCount         = 2 * kTiedArmsPerInputModel
#
# THESE MOVE TOGETHER, IN ONE COMMIT: the C++ grid, this block, the matching
# ORACLE_CANDIDATE_COUNT block in .claude/workflows/vol-oracle-iter.js, and
# `$expectedCandidateIds` in scripts/oracle-targeted-gate.ps1 (which pins the
# exact id SET rather than a count). Adding or widening an axis while any one of
# them lags does not fail loudly at the axis — it rejects a perfectly good
# receipt later, at whichever layer was missed.
$script:OracleInputModels = @('uprc_spot__rate__sdiv_yield', 'discrete_forward_pv__rate__sdiv_yield', 'discrete_forward_net_carry__rate__sdiv_yield', 'discrete_forward__rate__sdiv_yield', 'discrete_forward__rate_minus_sdiv__zero_carry', 'discrete_forward__zero_rate__zero_carry', 'discrete_forward_pv__rate_minus_sdiv__zero_carry', 'discrete_forward_pv__rate_plus_sdiv__zero_carry')
$script:OracleExerciseStyles = @('american_all', 'european_cash_settled_index', 'european_cash_settled_index_plus_empirical')
$script:OracleTimeDecayMethods = @('analytic_derivative', 'secant_252')
$script:OracleInputModelCount = $script:OracleInputModels.Count
$script:OracleExerciseStyleCount = $script:OracleExerciseStyles.Count
$script:OracleTimeDecayMethodCount = $script:OracleTimeDecayMethods.Count
# kTiedArmsPerInputModel: neither non-input-model axis can move a stage-1 PRICE
# (the smoke cohort is one unrouted underlier, and the decay method only changes
# how theta and delta decay are REPORTED), so one input model contributes this
# many candidates the price cut ranks bit-for-bit identically.
$script:OracleTiedArmsPerInputModel = $script:OracleExerciseStyleCount * $script:OracleTimeDecayMethodCount
# kCandidateCount = 8 x 3 x 2 = 48 at 635f8bd8.
$script:OracleCandidateCount = $script:OracleInputModelCount * $script:OracleTiedArmsPerInputModel
# kFinalistCount = 2 x 6 = 12 at 635f8bd8 — the FULL tied fan of the top TWO
# input models, never two candidates overall, and therefore exactly the number of
# candidates carrying a positive `tune_sample_count` on a sweep receipt.
$script:OracleFinalistCount = 2 * $script:OracleTiedArmsPerInputModel
# The grids this probe accepts, newest first: the CURRENT one, then the two
# narrower ones it grew from, each written as the same
# `<input models> x <tied arms>` / `2 x <tied arms>` arithmetic. Unlike the
# workflow validator — which only ever sees a gate receipt freshly produced at
# the tested SHA and is therefore pinned to the current grid alone — this probe
# validates receipts COMMITTED before an axis existed and MUST keep reporting
# them valid, for exactly the reason Test-ConventionMap keeps `exercise_style`
# and `time_decay_method` optional. Retiring a rung here means regenerating the
# committed bootstrap receipts in the same commit.
$script:OracleAcceptedCandidateGrids = @(
  @{ Candidates = $script:OracleCandidateCount; Finalists = $script:OracleFinalistCount },
  @{ Candidates = $script:OracleInputModelCount * $script:OracleExerciseStyleCount; Finalists = 2 * $script:OracleExerciseStyleCount },
  @{ Candidates = $script:OracleInputModelCount; Finalists = 2 }
)

function Invoke-GitText([string[]]$GitArgs) {
  $savedPreference = $ErrorActionPreference
  try {
    $ErrorActionPreference = 'SilentlyContinue'
    $output = @(& git -C $repoRoot @GitArgs 2>$null)
    $code = $LASTEXITCODE
  } finally { $ErrorActionPreference = $savedPreference }
  return [pscustomobject]@{ Code = $code; Text = (($output | ForEach-Object { [string]$_ }) -join "`n").Trim() }
}

function Resolve-Commit([string]$Ref) {
  $result = Invoke-GitText @('rev-parse', '--verify', ($Ref + '^{commit}'))
  if ($result.Code -ne 0 -or $result.Text -notmatch '^[0-9a-fA-F]{40}$') { throw ('cannot resolve commit: ' + $Ref) }
  return $result.Text.ToLowerInvariant()
}

function Resolve-Tree([string]$Sha) {
  $result = Invoke-GitText @('rev-parse', '--verify', ($Sha + '^{tree}'))
  if ($result.Code -ne 0 -or $result.Text -notmatch '^[0-9a-fA-F]{40}$') { throw ('cannot resolve tree: ' + $Sha) }
  return $result.Text.ToLowerInvariant()
}

function Test-CommittedPath([string]$Sha, [string]$Path) {
  return (Invoke-GitText @('cat-file', '-e', ($Sha + ':' + $Path))).Code -eq 0
}

function Get-BlobOid([string]$Sha, [string]$Path) {
  $result = Invoke-GitText @('rev-parse', '--verify', ($Sha + ':' + $Path))
  if ($result.Code -ne 0 -or $result.Text -notmatch '^[0-9a-fA-F]{40}$') { return '' }
  return $result.Text.ToLowerInvariant()
}

function Get-CommittedJson([string]$Sha, [string]$Path) {
  $result = Invoke-GitText @('show', ($Sha + ':' + $Path))
  if ($result.Code -ne 0 -or -not $result.Text) { return $null }
  try { return $result.Text | ConvertFrom-Json } catch { return $null }
}

function Test-ExactKeys($Value, [string[]]$Keys) {
  if (-not $Value) { return $false }
  $actual = @($Value.PSObject.Properties.Name | Sort-Object)
  $wanted = @($Keys | Sort-Object)
  return $actual.Count -eq $wanted.Count -and -not (Compare-Object $actual $wanted)
}

function Test-StringSet($Values, [string[]]$Expected) {
  $actual = @($Values)
  return $actual.Count -eq $Expected.Count -and @($actual | Select-Object -Unique).Count -eq $Expected.Count -and -not (Compare-Object ($actual | Sort-Object) ($Expected | Sort-Object))
}

function Test-Ancestor([string]$Ancestor, [string]$Descendant) {
  if ($Ancestor -notmatch '^[0-9a-f]{40}$' -or $Descendant -notmatch '^[0-9a-f]{40}$') { return $false }
  return (Invoke-GitText @('merge-base', '--is-ancestor', $Ancestor, $Descendant)).Code -eq 0
}

function Test-Provenance($Receipt, [string]$CurrentSha, [string]$PriorReceiptPath) {
  if (-not (Test-Ancestor ([string]$Receipt.base_sha) ([string]$Receipt.tested_sha)) -or -not (Test-Ancestor ([string]$Receipt.tested_sha) $CurrentSha)) { return $false }
  if ($PriorReceiptPath) {
    $baseOid = Get-BlobOid ([string]$Receipt.base_sha) $PriorReceiptPath
    if (-not $baseOid -or $baseOid -ne (Get-BlobOid $CurrentSha $PriorReceiptPath)) { return $false }
  }
  return $true
}

function Test-CohortJson($Cohort, [string]$ExpectedName) {
  if (-not (Test-ExactKeys $Cohort @('name', 'dates', 'underliers', 'buckets_et', 'notes')) -or $Cohort.name -ne $ExpectedName) { return $false }
  if (-not ($Cohort.notes -is [string]) -or @($Cohort.dates).Count -lt 1 -or @($Cohort.underliers).Count -lt 1 -or @($Cohort.buckets_et).Count -lt 1) { return $false }
  if (@($Cohort.dates | Where-Object { $_ -notmatch '^\d{4}-\d{2}-\d{2}$' }).Count -or
      @($Cohort.underliers | Where-Object { $_ -notmatch '^[A-Z0-9._-]{1,24}$' }).Count -or
      @($Cohort.buckets_et | Where-Object { $_ -notmatch '^(?:0[0-9]|1[0-9]|2[0-3])[0-5][0-9]$' -or $_ -eq '0930' }).Count) { return $false }
  return @($Cohort.dates | Select-Object -Unique).Count -eq @($Cohort.dates).Count -and
    @($Cohort.underliers | Select-Object -Unique).Count -eq @($Cohort.underliers).Count -and
    @($Cohort.buckets_et | Select-Object -Unique).Count -eq @($Cohort.buckets_et).Count
}

function Get-CohortMembershipDigest($Cohort) {
  if (-not $Cohort -or -not (Test-CohortJson $Cohort ([string]$Cohort.name))) { return '' }
  $canonical = [ordered]@{
    schema_version = 1
    name = [string]$Cohort.name
    dates = @($Cohort.dates | ForEach-Object { [string]$_ } | Sort-Object)
    underliers = @($Cohort.underliers | ForEach-Object { [string]$_ } | Sort-Object)
    buckets_et = @($Cohort.buckets_et | ForEach-Object { [string]$_ } | Sort-Object)
  } | ConvertTo-Json -Depth 4 -Compress
  $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($canonical)
  try {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
  } finally { if ($sha) { $sha.Dispose() } }
}

function Test-Disjoint($Left, $Right) {
  $leftSet = @($Left | ForEach-Object { [string]$_ } | Select-Object -Unique)
  $rightSet = @($Right | ForEach-Object { [string]$_ } | Select-Object -Unique)
  return @($leftSet | Where-Object { $rightSet -contains $_ }).Count -eq 0
}

function Test-IngestManifest([string]$Name, [string]$ExpectedSha256) {
  if (-not ($Name -match '^oracle_manifest_(\d{4}-\d{2}-\d{2})\.json$')) { return $false }
  $expectedDate = $Matches[1]
  if ($ExpectedSha256 -notmatch '^[0-9a-f]{64}$') { return $false }
  $path = Join-Path $dataRoot $Name
  if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $ExpectedSha256) { return $false }
  try { $manifest = [System.IO.File]::ReadAllText($path) | ConvertFrom-Json } catch { return $false }
  if (-not (Test-ExactKeys $manifest @('trading_date', 'source_tsv_bytes', 'total_rows', 'buckets', 'top_underliers_by_rows', 'ingested_at')) -or
      $manifest.trading_date -ne $expectedDate -or [long]$manifest.source_tsv_bytes -le 0 -or [long]$manifest.total_rows -le 0) { return $false }
  $bucketTotal = 0L
  foreach ($property in $manifest.buckets.PSObject.Properties) {
    if ($property.Name -notmatch '^(?:0[0-9]|1[0-9]|2[0-3])[0-5][0-9]$' -or $property.Name -eq '0930' -or [long]$property.Value -le 0) { return $false }
    $bucketTotal += [long]$property.Value
  }
  if ($bucketTotal -ne [long]$manifest.total_rows -or @($manifest.top_underliers_by_rows.PSObject.Properties).Count -lt 1) { return $false }
  foreach ($property in $manifest.top_underliers_by_rows.PSObject.Properties) {
    if ($property.Name -notmatch '^[A-Z0-9._-]{1,24}$' -or [long]$property.Value -le 0 -or [long]$property.Value -gt [long]$manifest.total_rows) { return $false }
  }
  $parsedTime = [datetime]::MinValue
  return [datetime]::TryParse([string]$manifest.ingested_at, [ref]$parsedTime)
}

function Test-DataReceipt([string]$Sha, [ref]$DigestOut) {
  $receipt = Get-CommittedJson $Sha ($oracleRoot + '/bootstrap/data.json')
  $keys = @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'ingest_manifest_name', 'ingest_manifest_sha256', 'smoke_blob_oid', 'tune_blob_oid', 'holdout_blob_oid', 'holdout_membership_sha256', 'smoke_schema_valid', 'tune_schema_valid', 'holdout_schema_valid', 'tune_holdout_underliers_disjoint', 'tune_holdout_buckets_disjoint')
  $dataCommandIds = @('oracle_existing_store_adoption', 'oracle_ingest_and_cohort_validate')
  if (-not (Test-ExactKeys $receipt $keys) -or $receipt.schema_version -ne 1 -or $receipt.transition -ne 'data' -or $dataCommandIds -notcontains $receipt.command_id -or $receipt.exit_code -ne 0 -or
      -not $receipt.smoke_schema_valid -or -not $receipt.tune_schema_valid -or -not $receipt.holdout_schema_valid -or -not $receipt.tune_holdout_underliers_disjoint -or -not $receipt.tune_holdout_buckets_disjoint -or -not (Test-Provenance $receipt $Sha '')) { return $false }
  if ($receipt.command_id -eq 'oracle_existing_store_adoption' -and $receipt.base_sha -ne $receipt.tested_sha) { return $false }
  $smokePath = $oracleRoot + '/cohorts/smoke.json'; $tunePath = $oracleRoot + '/cohorts/tune.json'
  $holdoutPath = $oracleRoot + '/cohorts/holdout.json'; $digestPath = $oracleRoot + '/cohorts/holdout.sha256'
  if ((Get-BlobOid $Sha $smokePath) -ne $receipt.smoke_blob_oid -or (Get-BlobOid $Sha $tunePath) -ne $receipt.tune_blob_oid -or
      (Get-BlobOid $Sha $holdoutPath) -ne $receipt.holdout_blob_oid -or -not (Test-CommittedPath $Sha $digestPath)) { return $false }
  $smoke = Get-CommittedJson $Sha $smokePath
  $tune = Get-CommittedJson $Sha $tunePath
  $holdout = Get-CommittedJson $Sha $holdoutPath
  if (-not (Test-CohortJson $smoke 'smoke') -or -not (Test-CohortJson $tune 'tune') -or -not (Test-CohortJson $holdout 'holdout')) { return $false }
  if (-not (Test-Disjoint $tune.underliers $holdout.underliers) -or -not (Test-Disjoint $tune.buckets_et $holdout.buckets_et)) { return $false }
  $digestText = (Invoke-GitText @('show', ($Sha + ':' + $digestPath))).Text.ToLowerInvariant()
  $computedDigest = Get-CohortMembershipDigest $holdout
  if ($digestText -notmatch '^[0-9a-f]{64}$' -or $computedDigest -notmatch '^[0-9a-f]{64}$' -or $digestText -ne $computedDigest -or
      $digestText -ne $receipt.holdout_membership_sha256 -or -not (Test-IngestManifest ([string]$receipt.ingest_manifest_name) ([string]$receipt.ingest_manifest_sha256))) { return $false }
  $DigestOut.Value = $digestText
  return $true
}

function Test-ModeReceipt([string]$Sha, [string]$Mode, $DataReceipt) {
  $slug = $Mode.ToLowerInvariant()
  $receipt = Get-CommittedJson $Sha ($oracleRoot + '/bootstrap/mode-' + $slug + '.json')
  $keys = if ($Mode -eq 'A') { @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'smoke_blob_oid', 'rows_processed', 'target_metric_ids') }
    else { @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'smoke_blob_oid', 'tune_blob_oid', 'rows_processed', 'target_metric_ids') }
  $expectedTargets = if ($Mode -eq 'A') { $targetA } else { $targetB }
  $priorPath = if ($Mode -eq 'A') { $oracleRoot + '/bootstrap/data.json' } else { $oracleRoot + '/bootstrap/conventions.json' }
  if (-not (Test-ExactKeys $receipt $keys) -or $receipt.schema_version -ne 1 -or $receipt.transition -ne ('mode_' + $slug) -or $receipt.command_id -ne ('oracle_mode_' + $slug + '_aggregate') -or
      $receipt.exit_code -ne 0 -or [long]$receipt.rows_processed -le 0 -or -not (Test-StringSet $receipt.target_metric_ids $expectedTargets) -or -not (Test-Provenance $receipt $Sha $priorPath)) { return $false }
  if ($receipt.smoke_blob_oid -ne $DataReceipt.smoke_blob_oid) { return $false }
  return $Mode -eq 'A' -or $receipt.tune_blob_oid -eq $DataReceipt.tune_blob_oid
}

function Test-FiniteNumber($Value) {
  if ($null -eq $Value) { return $false }
  $number = 0.0
  return [double]::TryParse(([string]$Value), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)
}

function Test-PositiveInteger($Value) {
  $number = 0L
  return $null -ne $Value -and [long]::TryParse(([string]$Value), [ref]$number) -and $number -gt 0 -and [double]$Value -eq [double]$number
}

function Test-FloorMetrics($Metrics) {
  $items = @($Metrics)
  if ($items.Count -ne $targetA.Count -or -not (Test-StringSet @($items.metric_id) $targetA)) { return $false }
  foreach ($metric in $items) {
    # selection_count is the population the scale search ran on; count is the
    # full reported population. The symmetric selection objective excludes
    # nothing, so the two are now equal — but both stay pinned, and the ratio
    # stays checked, so an objective that ever narrowed selection again would
    # have to move this committed number instead of doing it silently.
    if (-not (Test-ExactKeys $metric @('metric_id', 'value', 'count', 'selection_count', 'unit')) -or -not (Test-FiniteNumber $metric.value) -or [double]$metric.value -lt 0 -or
        -not (Test-PositiveInteger $metric.count) -or -not (Test-PositiveInteger $metric.selection_count) -or [long]$metric.selection_count -gt [long]$metric.count -or
        (10L * [long]$metric.selection_count) -lt [long]$metric.count) { return $false }
    $unit = if ($metric.metric_id -eq 'mode_a_price_mae') { 'ticks' } elseif ($metric.metric_id -eq 'mode_a_vol_mae') { 'bp' } else { 'relative' }
    if ($metric.unit -ne $unit) { return $false }
  }
  return $true
}

# Candidate and baseline floors must describe ONE row population per metric,
# otherwise the committed deltas compare two different samples.
function Test-FloorPopulationParity($Metrics, $BaselineMetrics) {
  $baselineById = @{}
  foreach ($metric in @($BaselineMetrics)) { $baselineById[[string]$metric.metric_id] = $metric }
  foreach ($metric in @($Metrics)) {
    $baseline = $baselineById[[string]$metric.metric_id]
    if (-not $baseline -or [long]$metric.count -ne [long]$baseline.count -or
        [long]$metric.selection_count -ne [long]$baseline.selection_count) { return $false }
  }
  return $true
}

# BOUNDED no-regression gate on the COMMITTED receipt, stated against the
# SYMMETRIC-RELATIVE arrays: a symmetric metric may be worse than its baseline
# only while `candidate <= baseline * $regressionBoundMultiplier`, and every
# regression the bound permits must appear in the receipt's
# `accepted_regressions`. Past the bound, or unpublished, this fails closed.
#
# Why a bound rather than `candidate <= baseline`: the convention fit is
# multi-objective over ELEVEN targets sharing ONE map, and no point in the
# closed candidate grid strictly dominates every other on all eleven, so a
# strict per-metric rule cannot be met by anything the search can reach. Why 1%:
# it is the charter's own Mode A Greek tolerance, so a regression inside it
# cannot flip a scorecard cell's verdict.
#
# The check runs in BOTH directions — no entry that is not a real within-bound
# regression, no within-bound regression without an entry — which is what stops
# the array from becoming a rubber stamp. There is still no bypass flag and no
# per-metric allowlist.
#
# The symmetric loss is the one the scale SELECTION minimises: bounded, with no
# smallest-scale gradient. The standard-relative array pins its denominator on
# near-zero-oracle rows, so gating it would systematically reward the smaller
# multiplier and contradict the selector rather than catch a defect. The
# standard array is still validated for shape, non-negativity, population parity
# and delta arithmetic; it is simply not the criterion. The committed symmetric
# array is also the RATCHET BASELINE a later iteration must not be worse than.
#
# Duplicated from the targeted gate on purpose: the targeted gate runs before the
# commit, this one runs after, and the one place a regression must not be
# recoverable-only-by-hand is the committed floor.
function Test-FloorNoRegression($Metrics, $BaselineMetrics, $AcceptedRegressions) {
  $baselineById = @{}
  foreach ($metric in @($BaselineMetrics)) { $baselineById[[string]$metric.metric_id] = $metric }
  # Metric ids that MUST be published: strictly worse, and within the bound. A
  # zero baseline needs no special case — `candidate <= 0 * 1.01` holds only at
  # candidate == 0, which is not a regression — so nothing divides by zero.
  $wanted = @{}
  foreach ($metric in @($Metrics)) {
    $baseline = $baselineById[[string]$metric.metric_id]
    if (-not $baseline) { return $false }
    $candidateValue = [double]$metric.value
    $baselineValue = [double]$baseline.value
    if ($candidateValue -gt ($baselineValue * $regressionBoundMultiplier)) { return $false }
    if ($candidateValue -gt $baselineValue) {
      $wanted[[string]$metric.metric_id] = [pscustomobject]@{
        Candidate = $candidateValue; Baseline = $baselineValue
        Pct = ($candidateValue - $baselineValue) / $baselineValue
      }
    }
  }
  $seen = @{}
  foreach ($entry in @($AcceptedRegressions)) {
    if (-not (Test-ExactKeys $entry @('metric_id', 'candidate', 'baseline', 'pct_of_baseline')) -or
        -not (Test-FiniteNumber $entry.candidate) -or -not (Test-FiniteNumber $entry.baseline) -or
        -not (Test-FiniteNumber $entry.pct_of_baseline)) { return $false }
    $id = [string]$entry.metric_id
    if ($seen.ContainsKey($id) -or -not $wanted.ContainsKey($id)) { return $false }
    $seen[$id] = $true
    $match = $wanted[$id]
    # `pct_of_baseline` gets the 1e-12 tolerance the delta arrays already use:
    # it is a derived quotient, not a copied value.
    if ([double]$entry.candidate -ne $match.Candidate -or [double]$entry.baseline -ne $match.Baseline -or
        [Math]::Abs([double]$entry.pct_of_baseline - $match.Pct) -gt 1.0e-12) { return $false }
  }
  return $seen.Count -eq $wanted.Count
}

# The nine Greek metric ids input_model_regressed_greeks may name. Price and vol
# are absolute floors and are not part of the input-model Greek comparison.
function Test-InputModelRegressedGreeks($Value) {
  if ($null -eq $Value) { return $false }
  $items = @($Value | ForEach-Object { [string]$_ })
  $greekIds = @($targetA | Where-Object { $_ -ne 'mode_a_price_mae' -and $_ -ne 'mode_a_vol_mae' })
  if (@($items | Select-Object -Unique).Count -ne $items.Count) { return $false }
  return @($items | Where-Object { $greekIds -notcontains $_ }).Count -eq 0
}

function Test-MetricDeltas($Deltas) {
  $items = @($Deltas)
  if ($items.Count -ne $targetA.Count -or -not (Test-StringSet @($items.metric_id) $targetA)) { return $false }
  foreach ($delta in $items) {
    if (-not (Test-ExactKeys $delta @('metric_id', 'candidate', 'baseline', 'delta', 'count', 'unit')) -or
        -not (Test-FiniteNumber $delta.candidate) -or -not (Test-FiniteNumber $delta.baseline) -or -not (Test-FiniteNumber $delta.delta) -or
        -not (Test-PositiveInteger $delta.count) -or [Math]::Abs(([double]$delta.candidate - [double]$delta.baseline) - [double]$delta.delta) -gt 1.0e-12) { return $false }
  }
  return $true
}

function Test-ConventionMap($Map) {
  $keys = @('input_model', 'forward_formula', 'rate_model', 'carry_model', 'dividend_model', 'day_count', 'dte_banding_day_count', 'price_scale', 'price_sign', 'vol_scale', 'delta_scale', 'delta_sign', 'gamma_scale', 'gamma_sign', 'theta_basis', 'theta_sign', 'vega_scale', 'vega_sign', 'rho_scale', 'rho_sign', 'phi_scale', 'phi_sign', 'volga_source', 'volga_scale', 'volga_sign', 'vanna_source', 'vanna_scale', 'vanna_sign', 'delta_decay_basis', 'delta_decay_day_count', 'delta_decay_sign')
  # `exercise_style` is OPTIONAL, mirroring Test-OracleConventionMap in
  # oracle-targeted-gate.ps1 (the domains are duplicated by design — see the
  # comment there): maps committed before the axis existed omit the key, and
  # absence means `american_all`, the historical American-everywhere default.
  # It must stay optional here because this probe validates committed receipts
  # that predate the axis and MUST keep reporting them valid.
  if ($Map -and @($Map.PSObject.Properties.Name) -contains 'exercise_style') {
    if ($script:OracleExerciseStyles -notcontains $Map.exercise_style) { return $false }
    $keys = @($keys) + 'exercise_style'
  }
  # `time_decay_method` is OPTIONAL for the same reason, one axis later, and
  # mirrors Test-OracleConventionMap in oracle-targeted-gate.ps1: maps committed
  # before the axis existed omit the key, and absence means
  # `analytic_derivative`, the historical analytic-jet default. It must stay
  # optional here because this probe validates committed receipts that predate
  # the axis and MUST keep reporting them valid.
  if ($Map -and @($Map.PSObject.Properties.Name) -contains 'time_decay_method') {
    if ($script:OracleTimeDecayMethods -notcontains $Map.time_decay_method) { return $false }
    $keys = @($keys) + 'time_decay_method'
  }
  if (-not (Test-ExactKeys $Map $keys)) { return $false }
  if ($script:OracleInputModels -notcontains $Map.input_model -or @('none', 'uprc_exp_rate_t_minus_ddiv') -notcontains $Map.forward_formula -or
      @('continuous_row_rate', 'continuous_rate_minus_sdiv', 'continuous_rate_plus_sdiv', 'zero') -notcontains $Map.rate_model -or
      @('sdiv_as_yield', 'zero') -notcontains $Map.carry_model -or @('continuous_yield_only', 'discrete_cash_forward') -notcontains $Map.dividend_model -or
      @('ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252') -notcontains $Map.day_count -or
      @('ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252') -notcontains $Map.dte_banding_day_count -or
      @('per_share', 'per_contract_100', 'per_share_from_contract') -notcontains $Map.price_scale -or
      $Map.vol_scale -ne 'decimal_identity' -or @('volga', 'vanna') -notcontains $Map.volga_source -or @('volga', 'vanna') -notcontains $Map.vanna_source -or
      @('per_day', 'per_year') -notcontains $Map.theta_basis -or @('per_day', 'per_year') -notcontains $Map.delta_decay_basis -or
      @('ACT_365F', 'ACT_365_25', 'ACT_360', 'BUS_252') -notcontains $Map.delta_decay_day_count) { return $false }
  foreach ($name in @('price_sign', 'delta_sign', 'gamma_sign', 'theta_sign', 'vega_sign', 'rho_sign', 'phi_sign', 'volga_sign', 'vanna_sign', 'delta_decay_sign')) {
    if (@('positive', 'negative') -notcontains $Map.$name) { return $false }
  }
  foreach ($name in @('delta_scale', 'gamma_scale', 'vega_scale', 'rho_scale', 'phi_scale', 'volga_scale', 'vanna_scale')) {
    if (@('per_unit', 'per_point', 'per_point_squared', 'per_contract_100') -notcontains $Map.$name) { return $false }
  }
  return $true
}

# The candidate registry must be a CLOSED cross product of the searched axes:
# `<input models> x <tied arms>` distinct candidates, of which exactly
# `2 x <tied arms>` carry a positive tune_sample_count — the full tied fan of the
# top TWO input models, never two candidates overall. Both numbers come from
# $script:OracleAcceptedCandidateGrids at the top of this file, which is the ONE
# place the grid arithmetic is written; see that block before changing anything
# here, including the note on why this probe accepts the narrower pre-axis grids
# and the workflow validator does not.
function Test-CandidatePrices($Candidates) {
  $items = @($Candidates)
  $finalists = -1
  foreach ($grid in $script:OracleAcceptedCandidateGrids) {
    if ($items.Count -eq [int]$grid.Candidates) { $finalists = [int]$grid.Finalists; break }
  }
  if ($finalists -lt 0 -or @($items.candidate_id | Select-Object -Unique).Count -ne $items.Count) { return $false }
  foreach ($candidate in $items) {
    if (-not (Test-ExactKeys $candidate @('candidate_id', 'smoke_price_mae_ticks', 'smoke_count', 'tune_sample_price_mae_ticks', 'tune_sample_count')) -or
        -not (Test-FiniteNumber $candidate.smoke_price_mae_ticks) -or -not (Test-PositiveInteger $candidate.smoke_count) -or
        -not (Test-FiniteNumber $candidate.tune_sample_price_mae_ticks)) { return $false }
    $tuneCount = 0L
    if (-not [long]::TryParse(([string]$candidate.tune_sample_count), [ref]$tuneCount) -or $tuneCount -lt 0) { return $false }
  }
  return @($items | Where-Object { [long]$_.tune_sample_count -gt 0 }).Count -eq $finalists
}

# The pin is DERIVED from the convention_speed_measure baseline as
# floor(baseline * 0.90), never copied from it: `pin -le baseline` admits
# pin == baseline, which turns the convention_speed re-measurement into a coin
# flip on ordinary run-to-run noise. A 5% margin is the loosest pin this accepts.
function Test-SpeedFloor($Speed) {
  return (Test-ExactKeys $Speed @('metric_id', 'baseline', 'pin', 'unit', 'preset', 'quiet_host')) -and
    $Speed.metric_id -eq 'rel_avx2_rows_per_second' -and $Speed.unit -eq 'rows_per_second' -and $Speed.preset -eq 'rel-avx2' -and $Speed.quiet_host -and
    (Test-FiniteNumber $Speed.baseline) -and [double]$Speed.baseline -gt 0 -and (Test-FiniteNumber $Speed.pin) -and [double]$Speed.pin -gt 0 -and
    [double]$Speed.pin -le ([double]$Speed.baseline * 0.95)
}

# Value-by-value comparison of two parsed JSON documents. Windows PowerShell
# 5.1's ConvertFrom-Json parses JSON numbers into System.Decimal and
# ConvertTo-Json re-emits the SOURCE DIGITS, so comparing re-serialized text made
# an authored `0.0` differ from the same number written by `%.17g` as `0`: a
# digit comparison wearing the costume of a value comparison.
function Test-JsonValueEqual($Left, $Right) {
  if ($null -eq $Left -or $null -eq $Right) { return ($null -eq $Left) -and ($null -eq $Right) }
  if ($Left -is [bool] -or $Right -is [bool]) { return ($Left -is [bool]) -and ($Right -is [bool]) -and ([bool]$Left -eq [bool]$Right) }
  if ($Left -is [string] -or $Right -is [string]) { return ($Left -is [string]) -and ($Right -is [string]) -and ([string]$Left -ceq [string]$Right) }
  if ($Left -is [array] -or $Right -is [array]) {
    if (-not ($Left -is [array]) -or -not ($Right -is [array]) -or $Left.Count -ne $Right.Count) { return $false }
    for ($index = 0; $index -lt $Left.Count; $index++) {
      if (-not (Test-JsonValueEqual $Left[$index] $Right[$index])) { return $false }
    }
    return $true
  }
  if ($Left -is [System.Management.Automation.PSCustomObject] -or $Right -is [System.Management.Automation.PSCustomObject]) {
    if (-not ($Left -is [System.Management.Automation.PSCustomObject]) -or -not ($Right -is [System.Management.Automation.PSCustomObject])) { return $false }
    $leftNames = @($Left.PSObject.Properties.Name)
    if (-not (Test-StringSet $leftNames @($Right.PSObject.Properties.Name))) { return $false }
    foreach ($name in $leftNames) {
      if (-not (Test-JsonValueEqual $Left.$name $Right.$name)) { return $false }
    }
    return $true
  }
  return (Test-FiniteNumber $Left) -and (Test-FiniteNumber $Right) -and ([double]$Left -eq [double]$Right)
}

function Test-ConventionsReceipt([string]$Sha, $DataReceipt) {
  $receipt = Get-CommittedJson $Sha ($oracleRoot + '/bootstrap/conventions.json')
  # `production_conventions` is part of both committed artifacts: without it the
  # receipt records the map the sweep RESOLVED but never the map production
  # actually prices with, and the two are only compared while a sweep is running.
  # Both floor arrays are committed. `symmetric_metrics` is the ratchet baseline
  # and the no-regression criterion (it is the loss selection minimises);
  # `metrics` is committed beside it so the floor stays directly comparable to
  # the charter's "greeks within 1% rel" target. Never unify them.
  $keys = @('schema_version', 'transition', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'smoke_blob_oid', 'tune_blob_oid', 'conventions_blob_oid', 'scorecard_blob_oid', 'rows_processed', 'target_metric_ids', 'baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'accepted_regressions', 'candidate_prices', 'input_model_regressed_greeks', 'speed')
  if (-not (Test-ExactKeys $receipt $keys) -or $receipt.schema_version -ne 2 -or $receipt.transition -ne 'conventions' -or $receipt.command_id -ne 'oracle_conventions_smoke_tune' -or $receipt.exit_code -ne 0 -or
      -not (Test-Provenance $receipt $Sha ($oracleRoot + '/bootstrap/mode-a.json')) -or $receipt.smoke_blob_oid -ne $DataReceipt.smoke_blob_oid -or $receipt.tune_blob_oid -ne $DataReceipt.tune_blob_oid) { return $false }
  if (-not (Test-ConventionMap $receipt.production_conventions) -or
      -not (Test-JsonValueEqual $receipt.production_conventions $receipt.conventions) -or
      -not (Test-ConventionMap $receipt.conventions) -or -not (Test-ConventionMap $receipt.baseline_conventions) -or
      -not (Test-FloorMetrics $receipt.metrics) -or -not (Test-FloorMetrics $receipt.baseline_metrics) -or
      -not (Test-FloorMetrics $receipt.symmetric_metrics) -or -not (Test-FloorMetrics $receipt.baseline_symmetric_metrics) -or
      -not (Test-FloorPopulationParity $receipt.metrics $receipt.baseline_metrics) -or
      -not (Test-FloorPopulationParity $receipt.symmetric_metrics $receipt.baseline_symmetric_metrics) -or
      -not (Test-FloorPopulationParity $receipt.symmetric_metrics $receipt.metrics) -or
      -not (Test-FloorNoRegression $receipt.symmetric_metrics $receipt.baseline_symmetric_metrics $receipt.accepted_regressions) -or
      -not (Test-InputModelRegressedGreeks $receipt.input_model_regressed_greeks) -or
      -not (Test-MetricDeltas $receipt.metric_deltas) -or -not (Test-MetricDeltas $receipt.symmetric_metric_deltas) -or
      -not (Test-CandidatePrices $receipt.candidate_prices) -or
      -not (Test-SpeedFloor $receipt.speed) -or -not (Test-PositiveInteger $receipt.rows_processed) -or
      -not (Test-StringSet $receipt.target_metric_ids $targetA)) { return $false }
  $conventionsPath = $oracleRoot + '/CONVENTIONS.md'; $scorecardPath = $oracleRoot + '/scorecards/iter-000.json'
  if ((Get-BlobOid $Sha $conventionsPath) -ne $receipt.conventions_blob_oid -or (Get-BlobOid $Sha $scorecardPath) -ne $receipt.scorecard_blob_oid) { return $false }
  $scorecard = Get-CommittedJson $Sha $scorecardPath
  $scorecardKeys = @('schema_version', 'kind', 'base_sha', 'tested_sha', 'command_id', 'exit_code', 'mode', 'cohorts', 'smoke_blob_oid', 'tune_blob_oid', 'rows_processed', 'target_metric_ids', 'baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'accepted_regressions', 'candidate_prices', 'input_model_regressed_greeks', 'oracle_suspect_candidates', 'market_evidence_status', 'diagnostic_speed', 'speed')
  if (-not (Test-ExactKeys $scorecard $scorecardKeys) -or $scorecard.schema_version -ne 2 -or
      $scorecard.kind -ne 'residual_floor' -or $scorecard.command_id -ne 'mode_a_residual_floor' -or $scorecard.exit_code -ne 0 -or $scorecard.mode -ne 'A' -or
      -not (Test-StringSet $scorecard.cohorts @('smoke', 'tune')) -or -not (Test-StringSet $scorecard.target_metric_ids $targetA) -or [long]$scorecard.rows_processed -le 0 -or
      -not (Test-Ancestor ([string]$scorecard.base_sha) ([string]$scorecard.tested_sha)) -or -not (Test-Ancestor ([string]$scorecard.tested_sha) $Sha) -or
      $scorecard.smoke_blob_oid -ne $DataReceipt.smoke_blob_oid -or $scorecard.tune_blob_oid -ne $DataReceipt.tune_blob_oid -or
      @($scorecard.oracle_suspect_candidates).Count -ne 0 -or $scorecard.market_evidence_status -ne 'not_evaluated_no_nbbo_gate' -or
      -not (Test-ConventionMap $scorecard.conventions) -or -not (Test-ConventionMap $scorecard.baseline_conventions) -or
      -not (Test-ConventionMap $scorecard.production_conventions) -or
      -not (Test-JsonValueEqual $scorecard.production_conventions $scorecard.conventions) -or
      -not (Test-FloorMetrics $scorecard.metrics) -or -not (Test-FloorMetrics $scorecard.baseline_metrics) -or
      -not (Test-FloorMetrics $scorecard.symmetric_metrics) -or -not (Test-FloorMetrics $scorecard.baseline_symmetric_metrics) -or
      -not (Test-FloorPopulationParity $scorecard.metrics $scorecard.baseline_metrics) -or
      -not (Test-FloorPopulationParity $scorecard.symmetric_metrics $scorecard.baseline_symmetric_metrics) -or
      -not (Test-FloorPopulationParity $scorecard.symmetric_metrics $scorecard.metrics) -or
      -not (Test-FloorNoRegression $scorecard.symmetric_metrics $scorecard.baseline_symmetric_metrics $scorecard.accepted_regressions) -or
      -not (Test-InputModelRegressedGreeks $scorecard.input_model_regressed_greeks) -or
      -not (Test-MetricDeltas $scorecard.metric_deltas) -or -not (Test-MetricDeltas $scorecard.symmetric_metric_deltas) -or
      -not (Test-CandidatePrices $scorecard.candidate_prices) -or -not (Test-SpeedFloor $scorecard.speed)) { return $false }
  foreach ($name in @('baseline_conventions', 'conventions', 'production_conventions', 'metrics', 'baseline_metrics', 'metric_deltas', 'symmetric_metrics', 'baseline_symmetric_metrics', 'symmetric_metric_deltas', 'accepted_regressions', 'candidate_prices', 'input_model_regressed_greeks', 'speed')) {
    if (-not (Test-JsonValueEqual $receipt.$name $scorecard.$name)) { return $false }
  }
  return [long]$receipt.rows_processed -eq [long]$scorecard.rows_processed
}

# Pester imports the closed validators without running command mode.
if ($MyInvocation.InvocationName -eq '.') { return }

$canonicalExists = $true
try { $baseSha = Resolve-Commit $canonicalRef } catch { $canonicalExists = $false; $baseSha = Resolve-Commit $baseRef }
$resolvedBaseRef = if ($canonicalExists) { $canonicalRef } else { $baseRef }
$baseTree = Resolve-Tree $baseSha
$holdoutDigest = ''
$dataValid = Test-DataReceipt $baseSha ([ref]$holdoutDigest)
$dataReceipt = if ($dataValid) { Get-CommittedJson $baseSha ($oracleRoot + '/bootstrap/data.json') } else { $null }
$modeAValid = $dataValid -and (Test-ModeReceipt $baseSha 'A' $dataReceipt)
$conventionsValid = $modeAValid -and (Test-ConventionsReceipt $baseSha $dataReceipt)
$modeBValid = $conventionsValid -and (Test-ModeReceipt $baseSha 'B' $dataReceipt)
if (-not $dataValid) { $state = 'missing_data' } elseif (-not $modeAValid) { $state = 'missing_mode_a' } elseif (-not $conventionsValid) { $state = 'missing_conventions' } elseif (-not $modeBValid) { $state = 'missing_mode_b' } else { $state = 'ready' }

$nextNumber = 0
if ($state -eq 'ready') {
  $names = (Invoke-GitText @('ls-tree', '-r', '--name-only', $baseSha, '--', ($oracleRoot + '/scorecards'))).Text -split "`n"
  foreach ($name in $names) { if ($name -match '/iter-([0-9]+)\.json$') { $nextNumber = [Math]::Max($nextNumber, [int]$Matches[1] + 1) } }
}
$probeOutput = 'state=' + $state + ' canonical_exists=' + $canonicalExists.ToString().ToLowerInvariant() + ' data_receipt_valid=' + $dataValid.ToString().ToLowerInvariant() +
  ' mode_a_receipt_valid=' + $modeAValid.ToString().ToLowerInvariant() + ' conventions_receipt_valid=' + $conventionsValid.ToString().ToLowerInvariant() + ' mode_b_receipt_valid=' + $modeBValid.ToString().ToLowerInvariant() +
  ' base_ref=' + $resolvedBaseRef + ' base_sha=' + $baseSha + ' base_tree=' + $baseTree
[ordered]@{
  state = $state; canonical_ref = $canonicalRef; canonical_exists = $canonicalExists; base_ref = $resolvedBaseRef; base_sha = $baseSha
  base_tree = $baseTree
  holdout_digest_receipt = $holdoutDigest; next_iter = 'iter-' + $nextNumber.ToString('000')
  evidence = @([ordered]@{ command = 'powershell scripts\oracle-capability.ps1'; exit_code = 0; output = $probeOutput })
} | ConvertTo-Json -Depth 5 -Compress
