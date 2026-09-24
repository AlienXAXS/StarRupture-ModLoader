<#
.SYNOPSIS
    Builds a configurable set of StarRupture plugin solutions and deploys the
    resulting DLL/PDB into the game and/or dedicated server ModLoader folders.

.DESCRIPTION
    Every StarRupture-Plugin-* repo has the same shape: one .sln at the repo
    root and output at build\<Configuration>\Plugins\<Name>.dll. This script
    walks a list of those repos (tools\build-plugins.config.json), runs MSBuild
    on each, then copies the DLL (and PDB) into

        <target root>\ModLoader\<destination>\

    where <target root> is the game's or server's Binaries\Win64 folder and
    <destination> defaults to "Plugins".

    On first run, if no config file exists, the example is copied into place
    and discovery is offered.

.PARAMETER Only
    Build only these plugins. Matches plugin name or repo folder name,
    case-insensitive, wildcards allowed. e.g. -Only Waila,Better*
    Passing this also skips the start menu.

.PARAMETER NoMenu
    Skip the start menu and build everything enabled. The menu only appears
    when the run is interactive and nothing on the command line has already
    said which plugins to build, so scripts and CI never see it.

.PARAMETER Skip
    Exclude these plugins (same matching rules as -Only).

.PARAMETER Target
    client, server, or both (default). A plugin is only built for the targets
    it declares in the config.

.PARAMETER Configuration
    Override the MSBuild configuration for every build in this run,
    e.g. -Configuration "Client Debug".

.PARAMETER List
    Show the configured plugins and what would be built, then exit.

.PARAMETER Discover
    Scan the GitHub root for StarRupture-Plugin-* repos and merge any new ones
    into the config file (added disabled, so nothing starts building itself).
    Existing entries are left alone.

.PARAMETER Rebuild
    Use the Rebuild target instead of Build.

.PARAMETER Clean
    Run the Clean target and exit (no build, no deploy).

.PARAMETER NoBuild
    Skip MSBuild and just deploy whatever is already in the build folders.

.PARAMETER NoDeploy
    Build only, do not copy anything.

.PARAMETER All
    Include entries marked "enabled": false.

.PARAMETER Parallel
    Build N solutions concurrently (PowerShell 7+). Default 1.

.PARAMETER NoMultiProc
    Do not pass /p:CL_MP=true. None of the plugin vcxproj files set
    MultiProcessorCompilation, so without that override each solution compiles
    one .cpp at a time - the SDK translation units make that the bulk of the
    wall clock. With -Parallel N the script also caps each MSBuild at
    cores/N compiler processes so N solutions do not oversubscribe the box.

.PARAMETER ShowBuildOutput
    Stream full MSBuild output instead of just errors/warnings.

.PARAMETER ConfigPath
    Use a different config file.

.EXAMPLE
    .\Build-Plugins.ps1
    Start menu: build everything enabled, or pick one plugin.

.EXAMPLE
    .\Build-Plugins.ps1 -NoMenu
    Build and deploy every enabled plugin to both targets, no prompt.

.EXAMPLE
    .\Build-Plugins.ps1 -Only Waila,Codex -Target client

.EXAMPLE
    .\Build-Plugins.ps1 -Target server -Parallel 4

.EXAMPLE
    .\Build-Plugins.ps1 -List
#>
[CmdletBinding()]
param(
    [string[]]$Only,
    [string[]]$Skip,
    [ValidateSet('client', 'server', 'both')]
    [string]$Target = 'both',
    [string]$Configuration,
    [switch]$List,
    [switch]$Discover,
    [switch]$Rebuild,
    [switch]$Clean,
    [switch]$NoBuild,
    [switch]$NoDeploy,
    [switch]$All,
    [int]$Parallel = 1,
    [switch]$NoMenu,
    [switch]$NoMultiProc,
    [switch]$ShowBuildOutput,
    [string]$ConfigPath
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$LogDir = Join-Path $env:TEMP 'starrupture-plugin-build'
$script:ErrCount = 0

# Repos that live under StarRupture-Plugin-* but are not buildable plugins.
$DiscoveryIgnore = @(
    'StarRupture-Plugin-SDK',
    'StarRupture-Plugin-MySDK'
)

# ---------------------------------------------------------------- helpers ---

function Write-Head([string]$text) {
    Write-Host ''
    Write-Host "=== $text" -ForegroundColor Cyan
}

function Write-Step([string]$text) { Write-Host "  $text" -ForegroundColor DarkGray }
function Write-Ok([string]$text) { Write-Host "  $text" -ForegroundColor Green }
function Write-Warn2([string]$text) { Write-Host "  $text" -ForegroundColor Yellow }
function Write-Err([string]$text) {
    $script:ErrCount++
    Write-Host "  $text" -ForegroundColor Red
}

# ConvertFrom-Json gives PSCustomObjects; missing properties throw under
# StrictMode and read as $null otherwise. One accessor keeps that tidy.
function Get-Prop($obj, [string]$name, $default = $null) {
    if ($null -eq $obj) { return $default }
    $p = $obj.PSObject.Properties[$name]
    if ($null -eq $p -or $null -eq $p.Value) { return $default }
    if ($p.Value -is [string] -and $p.Value -eq '') { return $default }
    return $p.Value
}

function Resolve-MSBuild([string]$explicit) {
    if ($explicit) {
        if (Test-Path -LiteralPath $explicit) { return (Resolve-Path -LiteralPath $explicit).Path }
        throw "msbuild path from config does not exist: $explicit"
    }

    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    foreach ($root in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not $root) { continue }
        $vswhere = Join-Path $root 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { continue }
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
        if ($found) { return $found }
    }

    throw 'MSBuild.exe not found. Install the VS 2022 C++ workload, or set "msbuild" in the config file.'
}

# Reads the SolutionConfigurationPlatforms block so a bad configuration name
# fails with a list of the real ones instead of a wall of MSBuild noise.
function Get-SolutionConfiguration([string]$sln) {
    $inside = $false
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in Get-Content -LiteralPath $sln) {
        if ($line -match 'GlobalSection\(SolutionConfigurationPlatforms\)') { $inside = $true; continue }
        if ($inside -and $line -match 'EndGlobalSection') { break }
        if ($inside -and $line -match '^\s*(.+?)\s*=\s*.+$') {
            $names.Add(($matches[1] -split '\|')[0].Trim())
        }
    }
    return ($names | Sort-Object -Unique)
}

function Find-Solution([string]$repoPath, $plugin) {
    $explicit = Get-Prop $plugin 'solution'
    if ($explicit) {
        $p = Join-Path $repoPath $explicit
        if (-not (Test-Path -LiteralPath $p)) { throw "solution not found: $p" }
        return (Resolve-Path -LiteralPath $p).Path
    }
    $slns = @(Get-ChildItem -LiteralPath $repoPath -Filter '*.sln' -File -ErrorAction SilentlyContinue)
    if ($slns.Count -eq 0) { throw "no .sln in $repoPath" }
    if ($slns.Count -gt 1) {
        throw "$($slns.Count) .sln files in $repoPath - set `"solution`" for this plugin in the config"
    }
    return $slns[0].FullName
}

function Get-PluginName($repoName) {
    if ($repoName -like 'StarRupture-Plugin-*') { return $repoName.Substring('StarRupture-Plugin-'.Length) }
    return $repoName
}

function Test-NameMatch([string[]]$patterns, $plugin) {
    foreach ($pat in $patterns) {
        if ($plugin.name -like $pat) { return $true }
        if ($plugin.repo -like $pat) { return $true }
    }
    return $false
}

# ----------------------------------------------------------------- config ---

if (-not $ConfigPath) { $ConfigPath = Join-Path $ScriptDir 'build-plugins.config.json' }
$ExamplePath = Join-Path $ScriptDir 'build-plugins.config.example.json'

if (-not (Test-Path -LiteralPath $ConfigPath)) {
    if (-not (Test-Path -LiteralPath $ExamplePath)) {
        throw "No config at $ConfigPath and no example next to it."
    }
    Copy-Item -LiteralPath $ExamplePath -Destination $ConfigPath
    Write-Head 'First run'
    Write-Host "  Created $ConfigPath from the example."
    Write-Host '  Edit the "targets" paths (game / server Binaries\Win64 folders) before building.'
    Write-Host ''
    exit 0
}

$cfg = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json

$githubRoot = Get-Prop $cfg 'githubRoot' (Split-Path -Parent (Split-Path -Parent $ScriptDir))
if (-not (Test-Path -LiteralPath $githubRoot)) { throw "githubRoot does not exist: $githubRoot" }
$githubRoot = (Resolve-Path -LiteralPath $githubRoot).Path

$copyPdbDefault = [bool](Get-Prop $cfg 'copyPdb' $true)
$globalProps = Get-Prop $cfg 'msbuildProperties'

# -------------------------------------------------------------- discovery ---

if ($Discover) {
    Write-Head 'Discovering plugin repos'
    $existing = @{}
    foreach ($p in $cfg.plugins) { $existing[$p.repo] = $true }

    $added = 0
    $repos = Get-ChildItem -LiteralPath $githubRoot -Directory -Filter 'StarRupture-Plugin-*' |
        Sort-Object Name
    foreach ($repo in $repos) {
        if ($DiscoveryIgnore -contains $repo.Name) { continue }
        if ($existing.ContainsKey($repo.Name)) { continue }

        $sln = @(Get-ChildItem -LiteralPath $repo.FullName -Filter '*.sln' -File)
        if ($sln.Count -ne 1) {
            Write-Warn2 "skip $($repo.Name) ($($sln.Count) .sln files)"
            continue
        }

        $configs = Get-SolutionConfiguration $sln[0].FullName
        $targets = @()
        if ($configs -match '^Client ') { $targets += 'client' }
        if (($configs -match '^Server ') -or ($configs -contains 'Release')) { $targets += 'server' }
        if ($targets.Count -eq 0) { $targets = @('client') }

        $entry = [ordered]@{
            name    = Get-PluginName $repo.Name
            repo    = $repo.Name
            enabled = $false
            targets = $targets
        }
        $cfg.plugins += [pscustomobject]$entry
        Write-Ok "added $($repo.Name) [$($targets -join ', ')] (disabled)"
        $added++
    }

    if ($added -gt 0) {
        $cfg.plugins = @($cfg.plugins | Sort-Object name)
        $cfg | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $ConfigPath -Encoding UTF8
        Write-Host ''
        Write-Host "  $added new entries written to $ConfigPath (set enabled:true for the ones you want)."
    }
    else {
        Write-Host '  Nothing new.'
    }
    Write-Host ''
    exit 0
}

# ----------------------------------------------------------------- menu -----

# The menu answers one question -- which plugins -- so it only appears when
# nothing on the command line has already answered it. -Only/-Skip have, -List
# and -Discover are not builds, and a redirected stdin means nobody is there to
# type. Everything else about the run (-Target, -Parallel, -Configuration) still
# comes from the arguments, so the wrapper .cmd can pass those and still prompt.
function Test-CanPrompt {
    if ($NoMenu -or $List -or $Discover -or $Only -or $Skip) { return $false }
    if (-not [Environment]::UserInteractive) { return $false }
    try { if ([Console]::IsInputRedirected) { return $false } } catch { }
    return $true
}

# Returns $true to carry on, $false to quit. Sets $script:Only / $script:All
# when one plugin was picked.
function Show-StartMenu {
    $rows = @()
    foreach ($p in $cfg.plugins) {
        $rows += [pscustomobject]@{
            Name    = Get-Prop $p 'name' (Get-PluginName $p.repo)
            Targets = (@(Get-Prop $p 'targets' @('client')) -join ', ')
            Enabled = [bool](Get-Prop $p 'enabled' $true)
        }
    }
    $enabled = @($rows | Where-Object { $_.Enabled })

    while ($true) {
        Write-Host ''
        Write-Host '=== StarRupture plugin build' -ForegroundColor Cyan
        Write-Host ''
        Write-Host ("  [1] Build and deploy everything enabled ({0} plugin{1})" -f `
            $enabled.Count, $(if ($enabled.Count -eq 1) { '' } else { 's' }))
        Write-Host '  [2] Pick one plugin'
        Write-Host ''
        Write-Host '  [Q] Quit'
        Write-Host ''
        $choice = (Read-Host '  Choice [1]').Trim()
        if ($choice -eq '') { $choice = '1' }

        switch ($choice) {
            '1' { return $true }
            '2' {
                # Back from the picker falls out of the switch and the enclosing
                # loop redraws this menu.
                if (Show-PluginPicker $rows) { return $true }
            }
            'q' { Write-Host ''; return $false }
            default { Write-Warn2 "Not an option: $choice" }
        }
    }
}

# Returns $true once a plugin was picked, $false for Back.
function Show-PluginPicker($rows) {
    while ($true) {
        Write-Host ''
        Write-Host '  Which plugin?' -ForegroundColor Cyan
        Write-Host ''
        for ($i = 0; $i -lt $rows.Count; $i++) {
            $r = $rows[$i]
            $line = "   {0,2}  {1,-20} {2}" -f ($i + 1), $r.Name, $r.Targets
            if ($r.Enabled) { Write-Host $line }
            else { Write-Host ($line + '   (disabled in config)') -ForegroundColor DarkGray }
        }
        Write-Host ''
        Write-Host '    0  Back'
        Write-Host ''
        $pick = (Read-Host "  Plugin [1-$($rows.Count)] or a name").Trim()

        if ($pick -eq '' -or $pick -eq '0') { return $false }

        # A number picks from the list; anything else is handed to -Only as a
        # name pattern, so "better*" works here as well as on the command line.
        $n = 0
        if ([int]::TryParse($pick, [ref]$n)) {
            if ($n -lt 1 -or $n -gt $rows.Count) {
                Write-Warn2 "Out of range: $pick"
                continue
            }
            $row = $rows[$n - 1]
            $script:Only = @($row.Name)
            # Picking a disabled entry is a deliberate act, so honour it rather
            # than selecting nothing and reporting that nothing matched.
            if (-not $row.Enabled) {
                $script:All = $true
                Write-Step "$($row.Name) is disabled in the config; building it anyway for this run."
            }
            return $true
        }

        $script:Only = @($pick)
        $script:All  = $true      # let a name match a disabled entry too
        return $true
    }
}

if (Test-CanPrompt) {
    # A host that claims to be interactive but cannot actually read a line
    # should build, not crash: the menu is a convenience, not the point of the
    # script.
    try {
        if (-not (Show-StartMenu)) { exit 0 }
    }
    catch {
        Write-Warn2 "Menu unavailable ($($_.Exception.Message)); building everything enabled."
    }
}

# ------------------------------------------------------------- selection ----

$wantedTargets = if ($Target -eq 'both') { @('client', 'server') } else { @($Target) }

$selected = @()
foreach ($p in $cfg.plugins) {
    $name = Get-Prop $p 'name' (Get-PluginName $p.repo)
    $p | Add-Member -NotePropertyName name -NotePropertyValue $name -Force

    if (-not $All -and -not [bool](Get-Prop $p 'enabled' $true)) { continue }
    if ($Only -and -not (Test-NameMatch $Only $p)) { continue }
    if ($Skip -and (Test-NameMatch $Skip $p)) { continue }

    $pTargets = @(Get-Prop $p 'targets' @('client'))
    $use = @($pTargets | Where-Object { $wantedTargets -contains $_ })
    if ($use.Count -eq 0) { continue }

    $selected += [pscustomobject]@{ Plugin = $p; Targets = $use }
}

if ($selected.Count -eq 0) {
    Write-Warn2 'Nothing selected. Try -List, or -All to include disabled entries.'
    exit 0
}

# Resolve every task up front so a typo in one entry fails before MSBuild has
# spent four minutes on the others.
$tasks = @()
foreach ($sel in $selected) {
    $p = $sel.Plugin
    $repoPath = Join-Path $githubRoot $p.repo
    if (-not (Test-Path -LiteralPath $repoPath)) {
        Write-Err "$($p.name): repo not found: $repoPath"
        continue
    }

    try { $sln = Find-Solution $repoPath $p }
    catch { Write-Err "$($p.name): $_"; continue }

    $slnConfigs = Get-SolutionConfiguration $sln

    foreach ($t in $sel.Targets) {
        $targetCfg = Get-Prop $cfg.targets $t
        if (-not $targetCfg) { Write-Err "$($p.name): no `"$t`" target in the config file"; continue }

        $conf = $Configuration
        if (-not $conf) { $conf = Get-Prop (Get-Prop $p 'configurations') $t }
        if (-not $conf) { $conf = Get-Prop $targetCfg 'configuration' }
        if (-not $conf) { Write-Err "$($p.name): no configuration for target $t"; continue }

        if ($slnConfigs -notcontains $conf) {
            Write-Err "$($p.name): solution has no configuration '$conf'. Available: $($slnConfigs -join ', ')"
            continue
        }

        $outDir = Get-Prop $p 'outputDir' "build\$conf\Plugins"
        $destName = Get-Prop $p 'destination' 'Plugins'
        $root = Get-Prop $targetCfg 'root'
        if (-not $root) { Write-Err "$($p.name): target $t has no `"root`" path"; continue }

        $tasks += [pscustomobject]@{
            Name          = $p.name
            Repo          = $p.repo
            Target        = $t
            Solution      = $sln
            Configuration = $conf
            SourceDir     = Join-Path $repoPath $outDir
            DestDir       = Join-Path (Join-Path $root 'ModLoader') $destName
            CopyPdb       = [bool](Get-Prop $p 'copyPdb' $copyPdbDefault)
            Props         = Get-Prop $p 'msbuildProperties' $globalProps
            Built         = $false
            Status        = 'pending'
            Log           = $null
            Copied        = @()
        }
    }
}

# Errors raised while resolving tasks (bad repo, unknown configuration) still
# count as problems even when the rest of the run succeeds.
$resolveErrors = $script:ErrCount

if ($tasks.Count -eq 0) {
    Write-Warn2 'No buildable tasks.'
    if ($resolveErrors -gt 0) { exit 1 }
    exit 0
}

if ($List) {
    Write-Head "Configured plugins ($($tasks.Count) build tasks)"
    $tasks |
        Select-Object Name, Target, Configuration,
            @{n = 'Solution'; e = { Split-Path -Leaf $_.Solution } },
            @{n = 'Deploy to'; e = { $_.DestDir } } |
        Format-Table -AutoSize | Out-String -Width 400 | Write-Host
    Write-Host "  config: $ConfigPath"
    Write-Host ''
    exit 0
}

# ------------------------------------------------------------------ build ---

$msbuild = Resolve-MSBuild (Get-Prop $cfg 'msbuild')
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$msTarget = 'Build'
if ($Rebuild) { $msTarget = 'Rebuild' }
if ($Clean) { $msTarget = 'Clean' }

# /m parallelises PROJECTS, and every plugin solution holds exactly one, so on
# its own it buys nothing. cl's own /MP is what matters here, and no plugin
# vcxproj sets MultiProcessorCompilation - but Microsoft.Cl.Common.props turns
# it on from the CL_MP property whenever the project left the metadata blank,
# which is exactly this case. CL_MPCount then bounds the processes per
# solution so -Parallel does not multiply them by N.
$mpArgs = @()
if (-not $NoMultiProc) {
    $mpArgs += '/p:CL_MP=true'
    if ($Parallel -gt 1) {
        $per = [Math]::Max(1, [int][Math]::Floor([Environment]::ProcessorCount / $Parallel))
        $mpArgs += "/p:CL_MPCount=$per"
    }
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()

if (-not $NoBuild) {
    Write-Head "$msTarget ($($tasks.Count) tasks, msbuild: $msbuild)"
    if ($NoMultiProc) { Write-Step '/MP disabled (-NoMultiProc): one .cpp at a time per solution' }
    else { Write-Step "compiler parallelism: $($mpArgs -join ' ')" }

    # Extra /p: pairs, flattened once so the parallel block stays free of
    # script-scope function calls.
    foreach ($t in $tasks) {
        $extra = @()
        if ($t.Props) {
            foreach ($prop in $t.Props.PSObject.Properties) { $extra += "/p:$($prop.Name)=$($prop.Value)" }
        }
        $t | Add-Member -NotePropertyName ExtraArgs -NotePropertyValue $extra -Force
        $t | Add-Member -NotePropertyName LogPath -NotePropertyValue `
        (Join-Path $LogDir ("{0}.{1}.{2}.log" -f $t.Name, $t.Target, ($t.Configuration -replace '[^A-Za-z0-9]', '_'))) -Force
    }

    $useParallel = $Parallel -gt 1 -and $PSVersionTable.PSVersion.Major -ge 7
    if ($Parallel -gt 1 -and -not $useParallel) {
        Write-Warn2 '-Parallel needs PowerShell 7+, building sequentially.'
    }

    if ($useParallel) {
        Write-Step "running up to $Parallel concurrent builds, output goes to the logs"
        $results = $tasks | ForEach-Object -ThrottleLimit $Parallel -Parallel {
            # $using: is hoisted into plain locals - it is not expanded inside
            # strings, and the runspace has no access to anything else here.
            $exe = $using:msbuild
            $tgt = $using:msTarget
            $mp = $using:mpArgs
            $t = $_
            $msArgs = @(
                $t.Solution
                "/t:$tgt"
                "/p:Configuration=$($t.Configuration)"
                '/p:Platform=x64'
                '/m'
                '/nologo'
                '/v:minimal'
            ) + $mp + $t.ExtraArgs
            $out = & $exe @msArgs 2>&1
            $code = $LASTEXITCODE
            $out | Out-String | Set-Content -LiteralPath $t.LogPath -Encoding UTF8
            [pscustomobject]@{ Name = $t.Name; Target = $t.Target; ExitCode = $code }
        }
        foreach ($t in $tasks) {
            $r = $results | Where-Object { $_.Name -eq $t.Name -and $_.Target -eq $t.Target } | Select-Object -First 1
            $t.Log = $t.LogPath
            if ($r -and $r.ExitCode -eq 0) {
                $t.Built = $true; $t.Status = 'built'
                Write-Ok "OK   $($t.Name) [$($t.Target)] $($t.Configuration)"
            }
            else {
                $t.Status = 'build failed'
                Write-Err "FAIL $($t.Name) [$($t.Target)] $($t.Configuration) - log: $($t.LogPath)"
            }
        }
    }
    else {
        foreach ($t in $tasks) {
            Write-Host ("  {0,-22} {1,-7} {2}" -f $t.Name, $t.Target, $t.Configuration) -NoNewline
            $msArgs = @(
                $t.Solution
                "/t:$msTarget"
                "/p:Configuration=$($t.Configuration)"
                '/p:Platform=x64'
                '/m'
                '/nologo'
                '/v:minimal'
            ) + $mpArgs + $t.ExtraArgs

            $out = & $msbuild @msArgs 2>&1
            $code = $LASTEXITCODE
            $t.Log = $t.LogPath
            $out | Out-String | Set-Content -LiteralPath $t.LogPath -Encoding UTF8

            if ($code -eq 0) {
                $t.Built = $true; $t.Status = 'built'
                Write-Host '  OK' -ForegroundColor Green
            }
            else {
                $t.Status = 'build failed'
                Write-Host '  FAILED' -ForegroundColor Red
            }

            if ($ShowBuildOutput) {
                $out | ForEach-Object { Write-Host "      $_" -ForegroundColor DarkGray }
            }
            elseif ($code -ne 0) {
                $out | Select-String -Pattern 'error|warning' |
                    Select-Object -First 25 |
                    ForEach-Object { Write-Host "      $_" -ForegroundColor Red }
                Write-Host "      full log: $($t.LogPath)" -ForegroundColor DarkGray
            }
        }
    }
}
else {
    foreach ($t in $tasks) { $t.Built = $true; $t.Status = 'not built' }
}

if ($Clean) {
    Write-Host ''
    Write-Host "  Clean finished in $([int]$sw.Elapsed.TotalSeconds)s."
    Write-Host ''
    if ($script:ErrCount -gt 0) { exit 1 }
    exit 0
}

# ----------------------------------------------------------------- deploy ---

if (-not $NoDeploy) {
    Write-Head 'Deploy'

    foreach ($t in $tasks) {
        if (-not $t.Built) { continue }

        if (-not (Test-Path -LiteralPath $t.SourceDir)) {
            $t.Status = 'no output'
            Write-Err "$($t.Name) [$($t.Target)]: no build output at $($t.SourceDir)"
            continue
        }

        $dlls = @(Get-ChildItem -LiteralPath $t.SourceDir -Filter '*.dll' -File -ErrorAction SilentlyContinue)
        if ($dlls.Count -eq 0) {
            $t.Status = 'no output'
            Write-Err "$($t.Name) [$($t.Target)]: no DLL in $($t.SourceDir)"
            continue
        }

        if (-not (Test-Path -LiteralPath $t.DestDir)) {
            New-Item -ItemType Directory -Force -Path $t.DestDir | Out-Null
        }

        $files = @($dlls)
        if ($t.CopyPdb) {
            foreach ($dll in $dlls) {
                $pdb = Join-Path $t.SourceDir ([IO.Path]::GetFileNameWithoutExtension($dll.Name) + '.pdb')
                if (Test-Path -LiteralPath $pdb) { $files += Get-Item -LiteralPath $pdb }
            }
        }

        $copied = @()
        $failed = $false
        foreach ($f in $files) {
            $dest = Join-Path $t.DestDir $f.Name
            try {
                Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
                $copied += $f.Name
            }
            catch {
                $failed = $true
                Write-Err "$($t.Name) [$($t.Target)]: cannot write $dest"
                Write-Err "    $($_.Exception.Message)"
                if ($_.Exception.Message -match 'being used by another process|access') {
                    Write-Warn2 '    -> the game or server is probably running and has this DLL loaded.'
                }
                break
            }
        }

        $t.Copied = $copied
        if ($failed) { $t.Status = 'deploy failed' }
        else {
            $t.Status = 'deployed'
            $names = ($copied | Where-Object { $_ -like '*.dll' }) -join ', '
            Write-Ok "$($t.Name) [$($t.Target)] -> $($t.DestDir)  ($names)"
        }
    }
}

# ---------------------------------------------------------------- summary ---

$sw.Stop()
Write-Head 'Summary'
$tasks |
    Select-Object Name, Target, Configuration, Status,
        @{n = 'Files'; e = { $_.Copied.Count } } |
    Format-Table -AutoSize | Out-String -Width 200 | Write-Host

$bad = @($tasks | Where-Object { $_.Status -like '*failed*' -or $_.Status -eq 'no output' })
$problems = $bad.Count + $resolveErrors
Write-Host ("  {0} task(s) in {1}s, {2} problem(s)." -f $tasks.Count, [int]$sw.Elapsed.TotalSeconds, $problems)
if ($problems -gt 0) {
    Write-Host "  logs: $LogDir" -ForegroundColor DarkGray
    Write-Host ''
    exit 1
}
Write-Host ''
# Explicit, because a .ps1 that just falls off the end leaves $LASTEXITCODE
# holding whatever the previous command set.
exit 0
