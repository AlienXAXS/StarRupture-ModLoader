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
    the Client folder of the Game-SDK commit for that build and give -Cl. The
    generator is layout-table.mjs from the starrupture-ci
    repo (https://github.com/msantoro12/starrupture-ci, ci\layout-table.mjs);
    it needs Node.js. Its format and the checks the loader makes are described
    in ci\LAYOUT-TABLE.md there.

    Every table in core\ lists the same classes. The class list follows the
    loader's sources, which have grown since those tables were made, so a run on
    today's sources lists more classes than the tables hold; widen the tables
    together or not at all.

    The classes come from the loader's own sources (every SDK type they name)
    plus their reflected base classes. A UClass keeps only the members those
    sources read (--granularity 1); script structs and function parameters
    keep every member. The built CL is read from kBuiltCL in
    hooks\game\version_map.generated.h.

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

.EXAMPLE
    .\tools\Update-LayoutTable.ps1 -Generator ..\starrupture-ci\ci\layout-table.mjs `
        -PluginSdk ..\StarRupture-Plugin-SDK\tools\rule3-allow.json
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Generator,

    [Parameter(Mandatory = $true)]
    [string]$PluginSdk,

    [string]$SdkClient,

    [string]$Cl
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sdk = if ($SdkClient) { $SdkClient } else { Join-Path $root 'StarRupture SDK\Client' }
$core = Join-Path $root 'StarRupture-ModLoader-Core'

if (-not (Test-Path $Generator)) { throw "Generator not found: $Generator" }
if (-not (Test-Path $PluginSdk)) { throw "Plugin SDK allow list not found: $PluginSdk" }
if (-not (Test-Path (Join-Path $sdk 'Assertions.inl'))) {
    throw "No Assertions.inl under '$sdk'. Run: git submodule update --init `"StarRupture SDK`", or pass -SdkClient"
}

if (-not $Cl) {
    $versionMap = Get-Content -Raw (Join-Path $core 'hooks\game\version_map.generated.h')
    if ($versionMap -notmatch 'kBuiltCL\s*=\s*(\d+)\s*;') {
        throw 'Could not read kBuiltCL from hooks\game\version_map.generated.h'
    }
    $Cl = $Matches[1]
}
if ($Cl -notmatch '^\d+$') { throw "-Cl must be a number, got '$Cl'" }

$out = Join-Path $core "core\layout_table_$Cl.generated.h"

& node $Generator gen (Join-Path $sdk 'Assertions.inl') --cl $Cl `
    --src $root --sdk (Join-Path $sdk 'SDK') --granularity 1 --out $out --namespace "SrLayoutTable_$Cl" `
    --consumer "core=$core" `
    --consumer "ui-menus=$(Join-Path $core 'hooks\game\game_menu\menu_layer.cpp')" `
    --consumer "keys=$(Join-Path $core 'hooks\game\version_map.generated.h')" `
    --consumer "keys=$(Join-Path $core 'hooks\game\scan_patterns.h')" `
    --plugin-sdk $PluginSdk
if ($LASTEXITCODE -ne 0) { throw "layout-table.mjs failed (exit $LASTEXITCODE)" }

Write-Host "Wrote $out for CL $Cl"
