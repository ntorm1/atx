<#
.SYNOPSIS
  Sprint-5 capstone harness — the p8 mega-book pipeline:
    discover (gated)  ->  combine  ->  metabook|optimize  ->  report

  Threads the S1-S4 mega-book knobs (risk-model, metabook/sleeve-method, the
  stack combine method, impact-in-selection, capacity-curve, deflation) that
  S5-0 exposed on the CLI, plus the S5-1/S5-2 deflation-blocking flags.

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
                              not an edge measurement. Every new p8 flag is passed
                              at its INERT value (proving the CLI parses/accepts
                              them) EXCEPT the opt-in boolean switches, which stay
                              ABSENT (their absence IS the inert value).
    -Profile prod           : pop 300 / gen 15, the full accept panel, and the
                              OPT-IN mega-book profile: --risk-model factor
                              --dead-alpha-factors --group-neutralize --metabook
                              --sleeve-method hrp --method stack
                              --impact-in-selection --selection-aum <-SelectionAum,
                              default $50M -- p8 final-wave: --impact-in-selection
                              alone is a documented no-op, CostSelectionConfig
                              contract> --capacity-curve --min-dsr 0.5 --max-pbo 0.5
                              --require-split-stable --blocking-pbo.
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
    # This harness lives in, and only ever builds/runs, the p8 WORKTREE
    # (C:\atx-wt\p8) -- NEVER the main repo (C:\atx). Unlike the root-level
    # scripts\build-tradeable-alphas.ps1 (which hardcodes C:\atx's own build
    # dir, because it IS C:\atx's script), this default deliberately points at
    # the p8 worktree's own build output so an operator who forgets -AtxExe
    # never silently invokes the main repo's (pre-S5) binary.
    [string]   $AtxExe      = 'C:\atx-wt\p8\build\bin\atx-impl.exe',
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
        [double] $MaxPbo = 1.0
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
    if ($Workers -gt 0)      { $argv.Add('--workers'); $argv.Add([string]$Workers) }

    [string[]]$argv.ToArray()
}

function New-CombineArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        # "stack"/"regime-stack" route through S3's EXISTING --method (already
        # end-to-end CLI-reachable) -- there is no separate --combine-method flag.
        [string] $Method = ''
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
    [string[]]$argv.ToArray()
}

function New-MetabookArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        [string] $SleeveMethod = 'invvol'
    )
    $libraryDir = Join-Path $WorkDir '_library'
    $comboIn    = Join-Path $WorkDir 'combo.bin'
    $booksOut   = Join-Path $WorkDir 'books.bin'
    [string[]]@(
        'metabook',
        '--panel',         $PanelBin,
        '--combo',         $comboIn,
        '--library-dir',   $libraryDir,
        '--books-out',     $booksOut,
        '--sleeve-method', $SleeveMethod
    )
}

function New-OptimizeArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        [double] $CostBps,
        [string] $RiskModel = 'diagonal',
        [switch] $DeadAlphaFactors,
        [switch] $GroupNeutralize
    )
    $comboIn  = Join-Path $WorkDir 'combo.bin'
    $booksOut = Join-Path $WorkDir 'books.bin'

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
    if ($DeadAlphaFactors) { $argv.Add('--dead-alpha-factors') }
    if ($GroupNeutralize)  { $argv.Add('--group-neutralize') }
    [string[]]$argv.ToArray()
}

function New-ReportArgv {
    param(
        [string] $PanelBin,
        [string] $WorkDir,
        [switch] $CapacityCurve
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

    $expandedStages = @()
    foreach ($s in $Stage) {
        if ($s -eq 'all')      { $expandedStages += @('discover', 'combine', 'metabook', 'optimize', 'report') }
        elseif ($s -eq 'pipeline') { $expandedStages += @('combine', 'metabook', 'optimize', 'report') }
        else { $expandedStages += $s }
    }
    $seen = @{}; $orderedStages = @()
    foreach ($s in $expandedStages) { if (-not $seen.ContainsKey($s)) { $seen[$s] = $true; $orderedStages += $s } }
    $canonicalOrder = @('discover', 'combine', 'metabook', 'optimize', 'report')
    $activeStages   = $canonicalOrder | Where-Object { $orderedStages -contains $_ }

    # metabook and optimize are ALTERNATIVES (both produce books.bin) -- the prod
    # profile's --metabook selects metabook; smoke's default (no --metabook)
    # selects optimize. Drop whichever one the profile did not select from the
    # active list so DryRun/real execution never runs BOTH into the same books.bin.
    $useMetabook = ($Profile -eq 'prod')
    $activeStages = $activeStages | Where-Object {
        if ($_ -eq 'metabook') { $useMetabook }
        elseif ($_ -eq 'optimize') { -not $useMetabook }
        else { $true }
    }

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

    foreach ($s in $activeStages) {
        switch ($s) {
            'discover' {
                if ($Profile -eq 'prod') {
                    $argv = New-DiscoverArgv -PanelBin $PanelBin -SeedFile $SeedFile -WorkDir $WorkDir `
                        -Workers $Workers -Population $effPopulation -Generations $effGenerations `
                        -ImpactInSelection -SelectionAum $SelectionAum `
                        -RequireSplitStable -BlockingPbo -MinDsr 0.5 -MaxPbo 0.5
                } else {
                    $argv = New-DiscoverArgv -PanelBin $PanelBin -SeedFile $SeedFile -WorkDir $WorkDir `
                        -Workers $Workers -Population $effPopulation -Generations $effGenerations -LooseGates
                }
                Invoke-Stage 'discover' $argv
            }
            'combine' {
                $method = if ($Profile -eq 'prod') { 'stack' } else { '' }
                $argv = New-CombineArgv -PanelBin $PanelBin -WorkDir $WorkDir -Method $method
                Invoke-Stage 'combine' $argv
            }
            'metabook' {
                $sleeve = if ($Profile -eq 'prod') { 'hrp' } else { 'invvol' }
                $argv = New-MetabookArgv -PanelBin $PanelBin -WorkDir $WorkDir -SleeveMethod $sleeve
                Invoke-Stage 'metabook' $argv
            }
            'optimize' {
                if ($Profile -eq 'prod') {
                    $argv = New-OptimizeArgv -PanelBin $PanelBin -WorkDir $WorkDir -CostBps $CostBps `
                        -RiskModel 'factor' -DeadAlphaFactors -GroupNeutralize
                } else {
                    $argv = New-OptimizeArgv -PanelBin $PanelBin -WorkDir $WorkDir -CostBps $CostBps -RiskModel 'diagonal'
                }
                Invoke-Stage 'optimize' $argv
            }
            'report' {
                $argv = New-ReportArgv -PanelBin $PanelBin -WorkDir $WorkDir -CapacityCurve:($Profile -eq 'prod')
                Invoke-Stage 'report' $argv
            }
        }
    }

    if ($DryRun) {
        Write-Host "`nDryRun complete - no binary was invoked." -ForegroundColor Green
    }
}
