<#
.SYNOPSIS
    Regenerates the layout table of one game build,
    StarRupture-ModLoader-Core\core\layout_table_<CL>.generated.h, from the
    Game-SDK.

.DESCRIPTION
    The layout table is the list of SDK classes the loader uses, with each
    class's size and each member's offset, size and bool mask as the SDK has
    them. At startup the loader compares it with the running game's reflection
    data (core\layout_check.cpp) and logs every class whose layout moved.

    The loader carries one table per game build it supports (core\layout_tables.h
    lists them); the running game's own CL picks the table it is checked against.
    The newest table must be the one for the SDK the loader is built against, so
    run this after every "Update SDK.bat" and commit the result together with the
    submodule bump. To refresh the table of an older build, point -SdkClient at
    the Client folder of the Game-SDK commit for that build and give -Cl. A table
    for a new build also has to be listed in core\layout_tables.h (include, Read
    function, kTables entry, newest last) and in the two Core project files and
    the LayoutCheckTests project. The generator is layout-table.mjs from the starrupture-ci
    repo (https://github.com/msantoro12/starrupture-ci, ci\layout-table.mjs);
    it needs Node.js. Its format and the checks the loader makes are described
    in ci\LAYOUT-TABLE.md there.

    Every client table in core\ lists the same classes. The class list follows
    the loader's sources, which have grown since those tables were made, so a run
    on today's sources lists more classes than the tables hold; widen the client
    tables together or not at all.

    The dedicated server has its own table (-Server), made from the Game-SDK's
    Server folder and checked at the server's start the same way. It lists the
    classes the server build reads, not the client's: see -Server.

    The classes come from the loader's own sources (every SDK type they name)
    plus their reflected base classes. A UClass keeps only the members those
    sources read (--granularity 1); script structs and function parameters
    keep every member. The table is written under namespace SrLayoutTable_<CL>,
    so the tables of several builds can be compiled together.

    Each class is also tagged with what reads its layout, so a class that
    changed on a game build turns off only what reads it
    (core\layout_check.h, LayoutCheck::Decide):
      core        the loader's own code; a change switches plugins off
      ui-menus    only the menu-layer read (hooks\game\game_menu\menu_layer.cpp)
      keys        named only as a version-map or pattern key; the loader
                  reads nothing at its compiled offsets
      plugin-sdk  a layout plugins keep compiled in (the Plugin SDK's rule-3
                  allow list); a change switches plugins off
    A class with no tag is unknown, and a change to it switches plugins off.

.PARAMETER Generator
    Path to layout-table.mjs.

.PARAMETER PluginSdk
    Path to the Plugin SDK's tools\rule3-allow.json.

.PARAMETER SdkClient
    The Game-SDK's Client folder (the one with Assertions.inl and SDK). Default:
    the Game-SDK submodule's. Needed with -Cl.

.PARAMETER Cl
    The game build the SDK is for. Default: kBuiltCL in
    hooks\game\version_map.generated.h (the build the submodule is on). With
    -Cl, -SdkClient is required, and a build_cl.txt in that folder must say the
    same number.

.PARAMETER Server
    Make the dedicated server's table, core\layout_table_server_<CL>.generated.h
    in namespace SrLayoutTable_Server_<CL>, from the Game-SDK's Server folder
    (its build_cl.txt gives the CL). The server build compiles fewer files than
    the client build (the Core project excludes the UI, the input hooks, the
    menu layer and the compatibility report from its Server configurations), so
    only the files it compiles are read: a class only that client code uses is
    not in the server table, and so cannot switch a server's plugins off. The
    list of files comes from the project file. Cannot be combined with
    -SdkClient or -Cl.

.EXAMPLE
    .\tools\Update-LayoutTable.ps1 -Generator ..\starrupture-ci\ci\layout-table.mjs `
        -PluginSdk ..\StarRupture-Plugin-SDK\tools\rule3-allow.json

.EXAMPLE
    .\tools\Update-LayoutTable.ps1 -Server -Generator ..\starrupture-ci\ci\layout-table.mjs `
        -PluginSdk ..\StarRupture-Plugin-SDK\tools\rule3-allow.json
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Generator,

    [Parameter(Mandatory = $true)]
    [string]$PluginSdk,

    [string]$SdkClient,

    [string]$Cl,

    [switch]$Server
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if ($Server -and ($SdkClient -or $Cl)) { throw '-Server reads the Server folder of the Game-SDK submodule: do not combine it with -SdkClient or -Cl' }
$sdk = if ($SdkClient) { $SdkClient } elseif ($Server) { Join-Path $root 'StarRupture SDK\Server' } else { Join-Path $root 'StarRupture SDK\Client' }
$core = Join-Path $root 'StarRupture-ModLoader-Core'

if (-not (Test-Path $Generator)) { throw "Generator not found: $Generator" }
if (-not (Test-Path $PluginSdk)) { throw "Plugin SDK allow list not found: $PluginSdk" }
if (-not (Test-Path (Join-Path $sdk 'Assertions.inl'))) {
    throw "No Assertions.inl under '$sdk'. Run: git submodule update --init `"StarRupture SDK`", or pass -SdkClient"
}

if ($Cl -and -not $SdkClient) { throw '-Cl needs -SdkClient: the Client folder of the Game-SDK commit for that build' }
$buildClFile = Join-Path $sdk 'build_cl.txt'
if ($Cl -and (Test-Path $buildClFile) -and ((Get-Content -Raw $buildClFile).Trim() -ne $Cl)) {
    throw "-Cl $Cl is not the build in '$buildClFile'"
}

if (-not $Cl) {
    if ($Server) {
        if (-not (Test-Path $buildClFile)) { throw "No build_cl.txt under '$sdk'" }
        $Cl = (Get-Content -Raw $buildClFile).Trim()
    } else {
        $versionMap = Get-Content -Raw (Join-Path $core 'hooks\game\version_map.generated.h')
        if ($versionMap -notmatch 'kBuiltCL\s*=\s*(\d+)\s*;') {
            throw 'Could not read kBuiltCL from hooks\game\version_map.generated.h'
        }
        $Cl = $Matches[1]
    }
}
if ($Cl -notmatch '^\d+$') { throw "-Cl must be a number, got '$Cl'" }

if ($Server) {
    $out = Join-Path $core "core\layout_table_server_$Cl.generated.h"
    $namespace = "SrLayoutTable_Server_$Cl"
} else {
    $out = Join-Path $core "core\layout_table_$Cl.generated.h"
    $namespace = "SrLayoutTable_$Cl"
}

# A copy of the sources without the files no Server configuration compiles, for
# the generator to read in place of the source tree. The Core project says which
# files a Server configuration leaves out (ExcludedFromBuild on a ClCompile
# item). A folder in which every file is left out is client-only whole (UI\,
# hooks\input\), headers included; elsewhere a header is client-only when the
# .cpp of its name is.
function New-ServerSourceCopy {
    param([string]$Root, [string]$Core, [string]$Destination)

    [xml]$project = Get-Content -Raw (Join-Path $Core 'StarRupture-ModLoader-Core.vcxproj')
    $namespaces = New-Object System.Xml.XmlNamespaceManager($project.NameTable)
    $namespaces.AddNamespace('m', 'http://schemas.microsoft.com/developer/msbuild/2003')

    # A condition is a chain of '$(Configuration)' != 'X' tests joined by And.
    $excludedInServer = {
        param([string]$Condition)
        if ($Condition -match '\bOr\b') { throw "Cannot read the ExcludedFromBuild condition: $Condition" }
        foreach ($clause in ($Condition -split '\bAnd\b')) {
            if ($clause.Trim() -notmatch '^''\$\(Configuration\)''\s*(!=|==)\s*''([^'']*)''$') {
                throw "Cannot read the ExcludedFromBuild condition: $Condition"
            }
            $same = $Matches[2] -eq 'Server Release'
            if (($Matches[1] -eq '==') -ne $same) { return $false }
        }
        return $true
    }

    $compiledPerDir = @{}
    $clientOnlyFiles = @{}
    foreach ($item in $project.SelectNodes('//m:ClCompile[@Include]', $namespaces)) {
        if ($item.Include.StartsWith('$(')) { continue }   # the SDK's own sources
        $path = [IO.Path]::GetFullPath((Join-Path $Core $item.Include))
        $excluded = $false
        foreach ($node in $item.SelectNodes('m:ExcludedFromBuild', $namespaces)) {
            if (& $excludedInServer $node.GetAttribute('Condition')) { $excluded = $true }
        }
        $dir = Split-Path -Parent $path
        if (-not $compiledPerDir.ContainsKey($dir)) { $compiledPerDir[$dir] = 0 }
        if ($excluded) { $clientOnlyFiles[$path] = $true } else { $compiledPerDir[$dir]++ }
    }
    $clientOnlyDirs = @($compiledPerDir.Keys | Where-Object { $compiledPerDir[$_] -eq 0 })
    $clientOnlyStems = @($clientOnlyFiles.Keys | ForEach-Object { Join-Path (Split-Path -Parent $_) ([IO.Path]::GetFileNameWithoutExtension($_)) })

    $kept = 0
    $left = 0
    foreach ($relative in (& git -C $Root ls-files --cached --others --exclude-standard)) {
        if ($relative -notmatch '\.(cpp|cc|cxx|c|h|hpp|hxx|inl)$') { continue }
        $path = [IO.Path]::GetFullPath((Join-Path $Root $relative))
        $dir = Split-Path -Parent $path
        $stem = Join-Path $dir ([IO.Path]::GetFileNameWithoutExtension($path))
        if ($clientOnlyFiles.ContainsKey($path) -or ($clientOnlyDirs -contains $dir) -or ($clientOnlyStems -contains $stem)) { $left++; continue }
        $copy = Join-Path $Destination $relative
        New-Item -ItemType Directory -Force (Split-Path -Parent $copy) | Out-Null
        Copy-Item -LiteralPath $path -Destination $copy
        $kept++
    }
    Write-Host "Server sources: $kept files read, $left client-only files left out"
}

$src = $root
$consumers = @(
    "core=$core",
    "ui-menus=$(Join-Path $core 'hooks\game\game_menu\menu_layer.cpp')",
    "keys=$(Join-Path $core 'hooks\game\version_map.generated.h')",
    "keys=$(Join-Path $core 'hooks\game\scan_patterns.h')"
)
$stage = $null
try {
    if ($Server) {
        # Only the server-compiled files, so only what the server reads is
        # tagged (and no ui-menus: its menu layer is not in a server build). The
        # server's own rows file names classes as keys, as the client's does.
        $stage = Join-Path ([IO.Path]::GetTempPath()) "layout-table-server-$([guid]::NewGuid().ToString('N'))"
        New-ServerSourceCopy -Root $root -Core $core -Destination $stage
        $src = $stage
        $stageCore = Join-Path $stage 'StarRupture-ModLoader-Core'
        $consumers = @(
            "core=$stageCore",
            "keys=$(Join-Path $stageCore 'hooks\game\version_map.generated.h')",
            "keys=$(Join-Path $stageCore 'hooks\game\version_map.server.h')",
            "keys=$(Join-Path $stageCore 'hooks\game\scan_patterns.h')"
        )
    }

    $generatorArgs = @('gen', (Join-Path $sdk 'Assertions.inl'), '--cl', $Cl, '--src', $src,
        '--sdk', (Join-Path $sdk 'SDK'), '--granularity', '1', '--out', $out, '--namespace', $namespace)
    foreach ($consumer in $consumers) { $generatorArgs += @('--consumer', $consumer) }
    $generatorArgs += @('--plugin-sdk', $PluginSdk)
    & node $Generator @generatorArgs
    if ($LASTEXITCODE -ne 0) { throw "layout-table.mjs failed (exit $LASTEXITCODE)" }
}
finally {
    if ($stage -and (Test-Path $stage)) { Remove-Item -Recurse -Force $stage }
}

Write-Host "Wrote $out for CL $Cl"
