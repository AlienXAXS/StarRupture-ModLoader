<#
.SYNOPSIS
    Regenerates StarRupture-ModLoader-Core\core\layout_table.generated.h from
    the Game-SDK submodule.

.DESCRIPTION
    The layout table is the list of SDK classes the loader uses, with each
    class's size and each member's offset, size and bool mask as the SDK has
    them. At startup the loader compares it with the running game's reflection
    data (core\layout_check.cpp) and logs every class whose layout moved.

    The table must match the SDK the loader is built against, so run this
    after every "Update SDK.bat" and commit the result together with the
    submodule bump. The generator is layout-table.mjs from the starrupture-ci
    repo (https://github.com/msantoro12/starrupture-ci, ci\layout-table.mjs);
    it needs Node.js. Its format and the checks the loader makes are described
    in ci\LAYOUT-TABLE.md there.

    The classes come from the loader's own sources (every SDK type they name)
    plus their reflected base classes. A UClass keeps only the members those
    sources read (--granularity 1); script structs and function parameters
    keep every member. The built CL is read from kBuiltCL in
    hooks\game\version_map.generated.h.

.PARAMETER Generator
    Path to layout-table.mjs.

.EXAMPLE
    .\tools\Update-LayoutTable.ps1 -Generator ..\starrupture-ci\ci\layout-table.mjs
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Generator
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root 'StarRupture SDK\Client'
$out = Join-Path $root 'StarRupture-ModLoader-Core\core\layout_table.generated.h'

if (-not (Test-Path $Generator)) { throw "Generator not found: $Generator" }
if (-not (Test-Path (Join-Path $sdk 'Assertions.inl'))) {
    throw "No Client\Assertions.inl under '$sdk'. Run: git submodule update --init `"StarRupture SDK`""
}

$versionMap = Get-Content -Raw (Join-Path $root 'StarRupture-ModLoader-Core\hooks\game\version_map.generated.h')
if ($versionMap -notmatch 'kBuiltCL\s*=\s*(\d+)\s*;') {
    throw 'Could not read kBuiltCL from hooks\game\version_map.generated.h'
}
$cl = $Matches[1]

& node $Generator gen (Join-Path $sdk 'Assertions.inl') --cl $cl `
    --src $root --sdk (Join-Path $sdk 'SDK') --granularity 1 --out $out
if ($LASTEXITCODE -ne 0) { throw "layout-table.mjs failed (exit $LASTEXITCODE)" }

Write-Host "Wrote $out for CL $cl"
