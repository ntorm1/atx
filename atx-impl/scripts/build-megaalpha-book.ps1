<#
.SYNOPSIS
  p9 mega-book build harness — the pipeline:
    discover (gated)  ->  combine  ->  metabook|optimize  ->  report

  Threads the S1-S5 mega-book knobs (risk-model, dead-alpha de-levering,
  group-neutralization, capacity/turnover discovery objectives, book-level
  turnover gate + participation cap, borrow financing, the full robustness
  battery, deflation) onto the stage that ACTUALLY EXECUTES each one under
  `-Profile prod`'s default `-Stage all` routing.

  S7 CORRECTION (read before touching prod argv again): pre-S7, the
  risk-model/dead-alpha/group-neutralize flags were attached only to
  `New-OptimizeArgv`'s prod branch, but `-Profile prod`'s default routing
  auto-excludes `optimize` (metabook is prod's book-build stage) -- so those
  flags parsed but never reached a stage that ran (a Potemkin book; see
  atx-engine/plans/p9/sprint-7-correct-prod-recipe.md). S7 re-attaches the
  live levers to `New-MetabookArgv`/`New-CombineArgv`/`New-DiscoverArgv`/
  `New-ReportArgv` -- the stages `-Stage all -Profile prod` actually runs --
  and fixes `Resolve-ActiveStages` so an explicit `-Stage optimize` request
  is never silently emptied either (see its own function-level comment).
  `New-OptimizeArgv`'s corrected argv (incl. S3's --gp-trading, which only
  bites via `optimize`) stays reachable via that explicit companion command.

.DESCRIPTION
  Modeled on the p6/p7 harness `scripts/build-tradeable-alphas.ps1` (root
  scripts/, not atx-impl/scripts): each stage's argv is produced by a dedicated
  testable New-*Argv function so Pester can dot-source this script and assert
  argv without invoking the binary or touching a panel.

  STAGE VOCABULARY DEVIATION (recorded, not silent): the sprint spec's stage
  list is augment|discover|riskmodel|combine|metabook|optimize|report. This
  harness's -Stage set is discover|combine|metabook|optimize|report (+ the
  pipeline/all meta-stages) — "augment" and "riskmodel" are NOT separate CLI
  subcommands in this codebase:
    * FINRA short-interest augment (stage_augment.hpp) has NO CLI stage yet
      (S5-0's ledger: it needs new ORATS-seg/symbology plumbing, deferred).
      The panel stage's OWN --augment-panel (Alpha101 panel augmentation, a
      DIFFERENT, already-CLI-reachable feature) is threaded into the discover
      panel dependency instead, where relevant.
    * --risk-model/--dead-alpha-factors/--group-neutralize reach the engine
      via stage_optimize.cpp's zero-arg run_optimize(cfg) (S5-0), so they are
      threaded into New-OptimizeArgv, not a separate "riskmodel" stage.
  --combine-method is likewise not a distinct flag: S3's --method already
  accepts stack|regime-stack end to end (config.cpp's method_from_string), so
  the prod profile passes --method stack rather than a nonexistent
  --combine-method.

.EXAMPLE
  # DryRun: print every stage's argv without invoking the binary
  pwsh -NoProfile -File atx-impl\scripts\build-megaalpha-book.ps1 -DryRun -Profile smoke

.EXAMPLE
  # V1 (operator-driven; NEVER run in-sprint — see the S5-5 research template):
  atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage discover -WorkDir work\megaalpha
  atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage combine,metabook,optimize,report -WorkDir work\megaalpha

.NOTES
  PROFILES:
    -Profile smoke (default): pop 40 / gen 4, LOOSE admission gates, on the cached
                              dev panel (work\dev\dev-panel.bin) — a WIRING smoke,
                              not an edge measurement. Every new flag is passed
                              at its INERT value (proving the CLI parses/accepts
                              them) EXCEPT the opt-in boolean switches, which stay
                              ABSENT (their absence IS the inert value). S7 adds
                              zero new flags to the smoke profile's argv (pinned
                              by the S7-3 Pester block) -- the prod-only routing
                              bug never touched smoke.
    -Profile prod           : pop 300 / gen 15, the full accept panel. Corrected
                              (S7) per-stage argv -- see each New-*Argv function's
                              own rationale comment for the engine file:line each
                              flag reaches:
                                discover: --deflate-selection --capacity-objective
                                  --turnover-objective --robustness-battery (+3
                                  sub-checks) --impact-in-selection --selection-aum
                                  --require-split-stable --blocking-pbo
                                combine:  --method stack --risk-model factor
                                metabook: --sleeve-method hrp --risk-model factor
                                  --dead-alpha-factors --dead-alpha-lib-dir
                                  --book-turnover-gate
                                report:   --capacity-curve --borrow-bps
                              --gp-trading (S3), --group-neutralize (S1/S2) and
                              --participation-cap (S5) are NOT on the metabook argv
                              above -- all three are consumed ONLY by stage_optimize's
                              MVO path (:191, :477, :414-438), which `-Stage all`'s
                              routing excludes for -Profile prod. metabook's HRP-sleeve
                              construction has no reader for them, so emitting them
                              there would be a parse-but-ignore Potemkin. They ARE
                              carried on the explicit `-Stage optimize -Profile prod`
                              companion command the S7-0 Resolve-ActiveStages fix
                              restores. Reachable via the
                              explicit `-Stage optimize -Profile prod` companion
                              command the S7-0 Resolve-ActiveStages fix restores.
                              S6 (ML-seeded discovery + NCO sleeve) was DEFERRED
                              by the operator -- `--ml-seeds`/`--ml-seed-model-dir`/
                              `--sleeve-method nco` are intentionally NOT emitted
                              anywhere in this script; the p9 binary does not
                              parse them. See New-DiscoverArgv's and
                              New-MetabookArgv's own comments for the exact cut
                              point. A future S6 sprint re-enables them behind
                              default-OFF switches.
                              Operator-driven; NOT run by this sprint (an
                              hour-long run is never a sprint gate).
#>
param(
    [ValidateSet('discover', 'combine', 'metabook', 'optimize', 'report', 'pipeline', 'all')]
    [string[]] $Stage       = @('all'),
    [switch]   $DryRun,
    [ValidateSet('prod', 'smoke')]
    [string]   $Profile     = 'prod',
    [int]      $Population  = 0,        # 0 = profile default (prod 300 / smoke 40)
    [int]      $Generations = 0,        # 0 = profile default (prod 15  / smoke 4)
    [string]   $WorkDir     = 'work\megaalpha-build',
    [string]   $PanelBin    = 'work\accept\panel.bin',
    [string]   $SeedFile    = 'atx-impl\tests\fixtures\alpha101.txt',
    [int]      $Workers     = 0,        # 0 = auto: OMIT --workers; >0 emits --workers N
    # This harness lives in, and only ever builds/runs, the p9 WORKTREE
    # (C:\atx-wt\p9) -- NEVER the main repo (C:\atx), and NEVER a prior
    # sprint's worktree either. S7-1 fix: this default was still pointing at
    # C:\atx-wt\p8's build output (stale since this script moved into p9) --
    # an operator who forgot -AtxExe would silently invoke the p8 binary,
    # which cannot parse ANY of S1-S5's new flags (they'd fail loud at
    # startup, but only after a wasted invocation). Unlike the root-level
    # scripts\build-tradeable-alphas.ps1 (which hardcodes C:\atx's own build
    # dir, because it IS C:\atx's script), this default deliberately points at
    # the CURRENT worktree's own build output.
    [string]   $AtxExe      = 'C:\atx-wt\p9\build\bin\atx-impl.exe',
    [double]   $CostBps     = 10,
    # p8 final-wave (Item 5 honesty fix): --impact-in-selection is a NO-OP unless
    # --selection-aum is ALSO > 0 (CostSelectionConfig's own contract: 0 == off
    # regardless of the boolean flag). The prod profile passed --impact-in-
    # selection alone for several sprints -- genuinely inert even after the
    # engine-side wire landed. ILLUSTRATIVE default ($50M, matching the
    # atx-engine fitness_cost_selection_test.cpp convention); an operator MUST
    # override this for their book's actual target AUM.
    [double]   $SelectionAum = 5.0e7
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Testable argv-composition functions.
# Each returns [string[]] with the subcommand first, no exe path.
# ---------------------------------------------------------------------------

function New-DiscoverArgv {
    param(
        [string] $PanelBin,
        [string] $SeedFile,
        [string] $WorkDir,
        [int]    $Workers     = 0,
        [int]    $Population  = 300,
        [int]    $Generations = 15,
        [switch] $LooseGates,
        # Mega-book knobs (S5-0..S5-2). Inert defaults are passed EXPLICITLY on
        # the smoke profile (proving the CLI parses them without changing the
        # digest); the prod profile turns the opt-ins ON.
        [switch] $ImpactInSelection,
        # --selection-aum: the companion --impact-in-selection needs to actually
        # bite (0 == off regardless of the boolean flag -- CostSelectionConfig's
        # own contract). Only emitted when ImpactInSelection is set AND this is
        # > 0, so a caller that omits it (or passes 0) gets the documented inert
        # no-op rather than a silently-dead --impact-in-selection.
        [double] $SelectionAum = 0.0,
        [switch] $RequireSplitStable,
        [switch] $BlockingPbo,
        [double] $MinDsr = 0.0,
        [double] $MaxPbo = 1.0,
        # S7-1 (p8/R4): fully wired since config.cpp:40, stage_discover.cpp:600,1038
        # -- simply never turned on in the prod recipe until now (a pure recipe
        # omission, not an engine gap).
        [switch] $DeflateSelection,
        # S7-1 (S4): factory/fitness.hpp's NSGA objectives (kMaxObjectives 7->9).
        [switch] $CapacityObjective,
        [switch] $TurnoverObjective,
        # S7-1 (p8 robustness_battery, config.cpp:52, stage_discover.cpp:597, +
        # S5's 3 previously-unreachable sub-checks). NOTE: S7-2 couples the 3
        # sub-checks so they are structurally unreachable without the master
        # switch -- this function alone does not yet enforce that (see
        # New-DiscoverArgv's call site in the main body and the S7-2 Pester
        # block for the coupling contract).
        [switch] $RobustnessBattery,
        [switch] $RobustnessSubUniverse,
        [switch] $RobustnessAltNeutralization,
        [switch] $RobustnessParamPerturb
        # S6 DEFERRED (operator decision, not an omission): this is where
        # -MlSeeds/-MlSeedModelDir (-> --ml-seeds/--ml-seed-model-dir) would go.
        # The p9 binary does not parse them -- Sprint 6 (ML-seeded discovery)
        # was never landed. Intentionally absent; a future S6 sprint adds them
        # behind a default-OFF switch. See sprint-7-progress.md's S6 deferral
        # note and the S7-2/S7-3 Pester blocks that pin their absence.
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $outDir     = Join-Path $WorkDir 'alphas'

    if ($LooseGates) {
        $gateArgs = [string[]]@(
            '--min-dsr',            [string]$MinDsr,
            '--min-sharpe',         '0.0',
            '--min-fitness',        '0.0',
            '--max-turnover',       '1.0'
        )
    } else {
        $gateArgs = [string[]]@(
            '--min-dsr',            [string]$MinDsr,
            '--min-sharpe',         '0.25',
            '--min-fitness',        '1.0',
            '--max-turnover',       '0.50'
        )
    }

    $argv = [System.Collections.Generic.List[string]]::new()
    $argv.AddRange([string[]]@(
        'discover',
        '--panel',           $PanelBin,
        '--seed-file',       $SeedFile,
        '--gated',
        '--library-dir',     $libraryDir,
        '--max-pbo',         [string]$MaxPbo
    ))
    $argv.AddRange($gateArgs)
    $argv.AddRange([string[]]@(
        '--oos-fraction',    '0.25',
        '--alpha-out',       $outDir,
        '--population',      [string]$Population,
        '--generations',     [string]$Generations
    ))
    if ($ImpactInSelection)  {
        $argv.Add('--impact-in-selection')
        if ($SelectionAum -gt 0) { $argv.Add('--selection-aum'); $argv.Add([string]$SelectionAum) }
    }
    if ($RequireSplitStable) { $argv.Add('--require-split-stable') }
    if ($BlockingPbo)        { $argv.Add('--blocking-pbo') }
    if ($DeflateSelection)   { $argv.Add('--deflate-selection') }
    if ($CapacityObjective)  { $argv.Add('--capacity-objective') }
    if ($TurnoverObjective)  { $argv.Add('--turnover-objective') }
    # S7-2: the 3 sub-checks are gated UNDER the master switch -- structurally
    # incapable of being emitted without it, closing the "1 of 4 checks
    # reachable, no surface for the other 3" gap honestly (the sub-checks are
    # meaningless without the master; recreating a different half-wired state
    # via 3 more independently-optional flags would not be an improvement).
    if ($RobustnessBattery) {
        $argv.Add('--robustness-battery')
        if ($RobustnessSubUniverse)       { $argv.Add('--robustness-sub-universe') }
        if ($RobustnessAltNeutralization) { $argv.Add('--robustness-alt-neutralization') }
        if ($RobustnessParamPerturb)      { $argv.Add('--robustness-param-perturb') }
    }
    if ($Workers -gt 0)      { $argv.Add('--workers'); $argv.Add([string]$Workers) }

    [string[]]$argv.ToArray()
}

function New-CombineArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        # "stack"/"regime-stack" route through S3's EXISTING --method (already
        # end-to-end CLI-reachable) -- there is no separate --combine-method flag.
        [string] $Method = '',
        # S7-1 (S2): stage_combine.cpp's CLI entry (:760-772) hardcoded
        # risk::RiskModelConfig{} (Diagonal) pre-S2; S2 stops that, so this
        # reuses the same --risk-model taxonomy as metabook/optimize. ''
        # (default) omits the flag so combine stays byte-for-argv Diagonal,
        # matching pre-p9 behavior.
        [string] $RiskModel = ''
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $comboOut   = Join-Path $WorkDir 'combo.bin'

    $argv = [System.Collections.Generic.List[string]]::new()
    $argv.AddRange([string[]]@(
        'combine',
        '--panel',        $PanelBin,
        '--library-dir',  $libraryDir,
        '--combo-out',    $comboOut,
        '--holdout-frac', '0.25'
    ))
    if ($Method -ne '') { $argv.Add('--method'); $argv.Add($Method) }
    if ($RiskModel -ne '') { $argv.Add('--risk-model'); $argv.Add($RiskModel) }
    [string[]]$argv.ToArray()
}

function New-MetabookArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        # S6 DEFERRED (operator decision, not an omission): 'nco' is NOT wired
        # as a supported value here -- this stays a raw string pass-through
        # (as it always was), but no script-level switch offers 'nco' as a
        # choice, and the main body's prod/smoke switch arms only ever pass
        # 'hrp'/'invvol' literally. The p9 binary does not parse --sleeve-
        # method nco; Sprint 6 (NCO sleeve) was never landed. A future S6
        # sprint adds a proper -SleeveMethod script switch, default OFF/hrp.
        [string] $SleeveMethod = 'invvol',
        # S7-1 (S2): reuses the existing --risk-model taxonomy; '' (default)
        # omits the flag so the metabook subcommand stays byte-for-argv
        # Diagonal, matching pre-p9 behavior.
        [string] $RiskModel = '',
        # S7-1 (S1): opt-in dead-alpha crowding de-levering. Requires
        # DeadAlphaLibDir to actually bite (fail-open no-op otherwise, per
        # S1's own contract) -- the coupling is enforced here, not just
        # asserted in tests, so a caller can't silently ship a no-op
        # --dead-alpha-factors.
        [switch] $DeadAlphaFactors,
        [string] $DeadAlphaLibDir = '',
        # S7-1 (S5): book-level turnover gate. stage_metabook.cpp:776 reads
        # cfg.book_turnover_gate directly against the HRP-sleeve fund book's own
        # cross-sleeve-netted per-day turnover rate. 0.0 (default) => off.
        [double] $BookTurnoverGate = 0.0
        # S7 REVIEW CLOSEOUT — deliberately NO -GroupNeutralize / -ParticipationCap
        # here. The metabook subcommand's HRP-sleeve construction has no reader for
        # either lever: participation-cap is consumed ONLY by stage_optimize.cpp's
        # MVO QP (:414-438), and group-neutralize only by stage_optimize.cpp:477 /
        # stage_combine.cpp -- stage_metabook.cpp never copies cfg.group_neutralize
        # into its risk_cfg (:419-423, :641-645 set only .kind + .dead_alpha_factors).
        # Emitting either on the metabook argv would parse-but-ignore: a Potemkin
        # lever on the flagship book, the exact defect p9 exists to kill. Both are
        # correctly carried on New-OptimizeArgv (the MVO path that DOES consume them,
        # reachable via an explicit `-Stage optimize`). Actually giving the sleeve
        # path its own equivalents is future ENGINE work (S2-scope for group-neutralize;
        # new %ADV-sleeve-bound design for participation-cap), out of this DryRun
        # recipe sprint's scope.
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $comboIn    = Join-Path $WorkDir 'combo.bin'
    $booksOut   = Join-Path $WorkDir 'books.bin'

    $argv = [System.Collections.Generic.List[string]]::new()
    $argv.AddRange([string[]]@(
        'metabook',
        '--panel',         $PanelBin,
        '--combo',         $comboIn,
        '--library-dir',   $libraryDir,
        '--books-out',     $booksOut,
        '--sleeve-method', $SleeveMethod
    ))
    if ($RiskModel -ne '') { $argv.Add('--risk-model'); $argv.Add($RiskModel) }
    if ($DeadAlphaFactors) {
        $argv.Add('--dead-alpha-factors')
        # Coupling (S1's own fail-open contract): an empty lib dir here would
        # silently no-op the flag we just set, so default it to the shared
        # library dir when the caller didn't supply one explicitly.
        $libDir = if ($DeadAlphaLibDir -ne '') { $DeadAlphaLibDir } else { $libraryDir }
        $argv.Add('--dead-alpha-lib-dir'); $argv.Add($libDir)
    }
    if ($BookTurnoverGate -gt 0) { $argv.Add('--book-turnover-gate'); $argv.Add([string]$BookTurnoverGate) }
    [string[]]$argv.ToArray()
}

function New-OptimizeArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        [double] $CostBps,
        [string] $RiskModel = 'diagonal',
        [switch] $DeadAlphaFactors,
        # S7-1 (S1): same fail-open coupling as New-MetabookArgv -- see below.
        [string] $DeadAlphaLibDir = '',
        [switch] $GroupNeutralize,
        # S7-1 (S3): stage_optimize.cpp:191's position-mode blend ONLY -- S3's
        # ROADMAP roots exclude stage_metabook.cpp, so --gp-trading does NOT
        # reach the prod book via `metabook`; it only bites through this
        # function's own `optimize` invocation (reachable post-S7-0's
        # Resolve-ActiveStages fix via an explicit -Stage optimize command).
        # Independently gated for now (S7-2 couples all three under
        # -GpTrading so the risk-aversion/cost-scale numerics never travel
        # without the master switch).
        # S7-2: coupled under -GpTrading (illustrative defaults, matching the
        # -SelectionAum convention) -- risk-aversion/cost-scale never travel
        # as orphan numerics without the master switch, and the master switch
        # never emits a bare --gp-trading missing its own two companions.
        [switch] $GpTrading,
        [double] $GpRiskAversion = 1.0,
        [double] $GpTradeCostScale = 1.0
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $comboIn    = Join-Path $WorkDir 'combo.bin'
    $booksOut   = Join-Path $WorkDir 'books.bin'

    $argv = [System.Collections.Generic.List[string]]::new()
    $argv.AddRange([string[]]@(
        'optimize',
        '--panel',       $PanelBin,
        '--combo',       $comboIn,
        '--books-out',   $booksOut,
        '--position-mode',
        '--cost-bps',    [string]$CostBps,
        '--risk-model',  $RiskModel
    ))
    if ($DeadAlphaFactors) {
        $argv.Add('--dead-alpha-factors')
        $libDir = if ($DeadAlphaLibDir -ne '') { $DeadAlphaLibDir } else { $libraryDir }
        $argv.Add('--dead-alpha-lib-dir'); $argv.Add($libDir)
    }
    if ($GroupNeutralize)  { $argv.Add('--group-neutralize') }
    if ($GpTrading) {
        $argv.Add('--gp-trading')
        $argv.Add('--gp-risk-aversion');   $argv.Add([string]$GpRiskAversion)
        $argv.Add('--gp-trade-cost-scale'); $argv.Add([string]$GpTradeCostScale)
    }
    [string[]]$argv.ToArray()
}

function Resolve-ActiveStages {
    <#
    .SYNOPSIS
      Pure stage-list resolver (S7-0 extraction of the former inline block at
      :285-305). Expands the 'all'/'pipeline' shorthands, de-dupes, restores
      canonical execution order, then applies the metabook/optimize mutual
      exclusion (both stages write books.bin -- never run both into the same
      file) -- but ONLY when the caller used a SHORTHAND. An explicit -Stage
      list is never second-guessed.

    .NOTES
      S7-0 BUG FIX: the pre-S7 inline block computed `$useMetabook =
      ($Profile -eq 'prod')` and applied it unconditionally to whatever
      `$activeStages` held, regardless of whether the caller asked for 'all'
      (ambiguous -- the profile must decide) or explicitly for '-Stage
      optimize' (unambiguous -- the caller said exactly what they meant).
      The old code silently returned an EMPTY list for `-Stage optimize
      -Profile prod`. This is the routing bug the whole S7 sprint rationale
      depends on: it is what made `New-OptimizeArgv`'s prod branch dead code
      and let the risk-model flags parse without ever reaching a stage that
      runs. Fixing it here also makes S3's --gp-trading reachable at all (it
      only bites via `optimize`, which S3's ROADMAP roots deliberately keep
      separate from `metabook`).
    #>
    param(
        [string[]] $Stage,
        [string]   $Profile
    )
    $expandedStages = @()
    foreach ($s in $Stage) {
        if ($s -eq 'all')         { $expandedStages += @('discover', 'combine', 'metabook', 'optimize', 'report') }
        elseif ($s -eq 'pipeline') { $expandedStages += @('combine', 'metabook', 'optimize', 'report') }
        else                       { $expandedStages += $s }
    }
    $seen = @{}; $orderedStages = @()
    foreach ($s in $expandedStages) { if (-not $seen.ContainsKey($s)) { $seen[$s] = $true; $orderedStages += $s } }
    $canonicalOrder = @('discover', 'combine', 'metabook', 'optimize', 'report')
    $activeStages   = $canonicalOrder | Where-Object { $orderedStages -contains $_ }

    # metabook/optimize are ALTERNATIVES (both produce books.bin) ONLY when the
    # caller used the 'all'/'pipeline' SHORTHAND -- that is the one genuinely
    # ambiguous case (the shorthand can't know which book-build stage the
    # caller wants, so -Profile decides: prod -> metabook, smoke -> optimize).
    # An EXPLICIT -Stage list (e.g. -Stage optimize, or -Stage metabook,optimize)
    # is never second-guessed: the caller said exactly what they meant.
    $isShorthand = ($Stage -contains 'all') -or ($Stage -contains 'pipeline')
    if ($isShorthand) {
        if ($Profile -eq 'prod') { $activeStages = $activeStages | Where-Object { $_ -ne 'optimize' } }
        else                     { $activeStages = $activeStages | Where-Object { $_ -ne 'metabook' } }
    }
    [string[]]$activeStages
}

function New-ReportArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        # NOT an S1-S6 fix target: stage_report.cpp's capacity_point compute
        # runs unconditionally on `has_volume && report_aum>0`, regardless of
        # this flag -- a dead marker (design-spec-documented). Kept, not
        # dropped: it records the intended future gate and costs nothing.
        [switch] $CapacityCurve,
        # S7-1 (S5): non-zero borrow/financing debit reaching the honest
        # book-level cost numbers stage_report.cpp already assembles
        # (:644-777, capacity_point_aum et al). 0.0 (default) => off.
        [double] $BorrowBps = 0.0
    )
    $comboIn   = Join-Path $WorkDir 'combo.bin'
    $booksIn   = Join-Path $WorkDir 'books.bin'
    $reportOut = Join-Path $WorkDir 'report'

    $argv = [System.Collections.Generic.List[string]]::new()
    $argv.AddRange([string[]]@(
        'report',
        '--panel',      $PanelBin,
        '--books',      $booksIn,
        '--combo',      $comboIn,
        '--report-out', $reportOut
    ))
    if ($CapacityCurve) { $argv.Add('--capacity-curve') }
    if ($BorrowBps -gt 0) { $argv.Add('--borrow-bps'); $argv.Add([string]$BorrowBps) }
    [string[]]$argv.ToArray()
}

# ---------------------------------------------------------------------------
# Main execution body — GUARDED: runs only when invoked directly, NOT
# dot-sourced (the Pester convention this file's tests rely on).
# ---------------------------------------------------------------------------

if ($MyInvocation.InvocationName -ne '.') {

    $profileDefaults = @{
        prod  = @{ Pop = 300; Gen = 15 }
        smoke = @{ Pop = 40;  Gen = 4  }
    }
    $effPopulation  = if ($Population  -gt 0) { $Population }  else { $profileDefaults[$Profile].Pop }
    $effGenerations = if ($Generations -gt 0) { $Generations } else { $profileDefaults[$Profile].Gen }

    if ($Profile -eq 'smoke') {
        if (-not $PSBoundParameters.ContainsKey('PanelBin')) { $PanelBin = 'work\dev\dev-panel.bin' }
        if (-not $PSBoundParameters.ContainsKey('WorkDir'))  { $WorkDir  = 'work\dev\megaalpha-smoke' }
    }
    Write-Host "[profile=$Profile] population=$effPopulation generations=$effGenerations panel=$PanelBin workdir=$WorkDir" -ForegroundColor DarkCyan

    if (-not [System.IO.Path]::IsPathRooted($WorkDir)) {
        $WorkDir = Join-Path (Get-Location).Path $WorkDir
    }

    $activeStages = Resolve-ActiveStages -Stage $Stage -Profile $Profile

    if (-not (Test-Path $WorkDir)) { New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null }

    function Invoke-Stage {
        param([string] $Name, [string[]] $Argv)
        if ($DryRun) {
            Write-Host "=== [DryRun] $Name ===" -ForegroundColor Yellow
            Write-Host "  $AtxExe $($Argv -join ' ')" -ForegroundColor Gray
        } else {
            Write-Host "=== $Name ===" -ForegroundColor Cyan
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            & $AtxExe @Argv
            $sw.Stop()
            Write-Host "  ($Name wall time: $($sw.Elapsed.TotalSeconds.ToString('F1'))s)" -ForegroundColor DarkGray
            if ($LASTEXITCODE -ne 0) { throw "STAGE FAILED: $Name (exit $LASTEXITCODE)" }
        }
    }

    # S7-1 ILLUSTRATIVE defaults (mirrors the pre-existing -SelectionAum
    # doc-comment convention): --participation-cap, --borrow-bps, and
    # --gp-risk-aversion/--gp-trade-cost-scale below exist so the CLI parses
    # non-degenerate values, NOT as tuned production numbers. An operator MUST
    # override them for the book's actual ADV/target AUM/financing terms.
    foreach ($s in $activeStages) {
        switch ($s) {
            'discover' {
                if ($Profile -eq 'prod') {
                    $argv = New-DiscoverArgv -PanelBin $PanelBin -SeedFile $SeedFile -WorkDir $WorkDir `
                        -Workers $Workers -Population $effPopulation -Generations $effGenerations `
                        -ImpactInSelection -SelectionAum $SelectionAum `
                        -RequireSplitStable -BlockingPbo -MinDsr 0.5 -MaxPbo 0.5 `
                        -DeflateSelection -CapacityObjective -TurnoverObjective `
                        -RobustnessBattery -RobustnessSubUniverse -RobustnessAltNeutralization -RobustnessParamPerturb
                } else {
                    $argv = New-DiscoverArgv -PanelBin $PanelBin -SeedFile $SeedFile -WorkDir $WorkDir `
                        -Workers $Workers -Population $effPopulation -Generations $effGenerations -LooseGates
                }
                Invoke-Stage 'discover' $argv
            }
            'combine' {
                $method    = if ($Profile -eq 'prod') { 'stack' }  else { '' }
                $riskModel = if ($Profile -eq 'prod') { 'factor' } else { '' }
                $argv = New-CombineArgv -PanelBin $PanelBin -WorkDir $WorkDir -Method $method -RiskModel $riskModel
                Invoke-Stage 'combine' $argv
            }
            'metabook' {
                if ($Profile -eq 'prod') {
                    $argv = New-MetabookArgv -PanelBin $PanelBin -WorkDir $WorkDir -SleeveMethod 'hrp' `
                        -RiskModel 'factor' -DeadAlphaFactors `
                        -BookTurnoverGate 0.20
                } else {
                    $argv = New-MetabookArgv -PanelBin $PanelBin -WorkDir $WorkDir -SleeveMethod 'invvol'
                }
                Invoke-Stage 'metabook' $argv
            }
            'optimize' {
                if ($Profile -eq 'prod') {
                    $argv = New-OptimizeArgv -PanelBin $PanelBin -WorkDir $WorkDir -CostBps $CostBps `
                        -RiskModel 'factor' -DeadAlphaFactors -GroupNeutralize `
                        -GpTrading -GpRiskAversion 1.0 -GpTradeCostScale 1.0
                } else {
                    $argv = New-OptimizeArgv -PanelBin $PanelBin -WorkDir $WorkDir -CostBps $CostBps -RiskModel 'diagonal'
                }
                Invoke-Stage 'optimize' $argv
            }
            'report' {
                $borrowBps = if ($Profile -eq 'prod') { 25 } else { 0 }
                $argv = New-ReportArgv -PanelBin $PanelBin -WorkDir $WorkDir -CapacityCurve:($Profile -eq 'prod') `
                    -BorrowBps $borrowBps
                Invoke-Stage 'report' $argv
            }
        }
    }

    if ($DryRun) {
        Write-Host "`nDryRun complete - no binary was invoked." -ForegroundColor Green
    }
}
