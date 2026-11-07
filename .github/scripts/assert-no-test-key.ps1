<#
.SYNOPSIS
  Fails when a built DLL carries the marker of a build that trusts the TEST
  version-map key.

.DESCRIPTION
  A loader built with /p:VmapTrustTestKey=true (Shared.props) contains the
  string VMAP-TEST-KEY-TRUSTED, which it also logs at startup. A release must
  never ship one. The release and draft workflows call this after the builds
  and before anything is published.

  Every *.dll under -Path is searched, for the marker as plain bytes (ASCII)
  and as UTF-16. The script throws, naming each file found, when any contains
  it, and also when -Path holds no DLL at all (a wrong path would otherwise
  pass the check by finding nothing).

.PARAMETER Path
  The build output folder to search, searched recursively. Default: build.
#>
param([string]$Path = 'build')

$ErrorActionPreference = 'Stop'
$marker = 'VMAP-TEST-KEY-TRUSTED'

# Bytes read as ISO-8859-1 map one to one onto characters, so a plain string
# search finds the marker whatever surrounds it. The UTF-16 form is the same
# text with a NUL after every character.
$latin1  = [Text.Encoding]::GetEncoding(28591)
$needles = @($marker, (($marker.ToCharArray() -join [char]0) + [char]0))

$dlls = @(Get-ChildItem -Path $Path -Recurse -Filter *.dll -File)
if ($dlls.Count -eq 0) {
    throw "No DLL found under '$Path', so the test-key check proved nothing."
}

$found = @()
foreach ($dll in $dlls) {
    $text = $latin1.GetString([IO.File]::ReadAllBytes($dll.FullName))
    foreach ($needle in $needles) {
        if ($text.IndexOf($needle, [StringComparison]::Ordinal) -ge 0) { $found += $dll.FullName; break }
    }
}

if ($found.Count -gt 0) {
    $list = $found -join ', '
    throw "Found the test-key marker '$marker' in: $list. These DLLs were built with VmapTrustTestKey=true and trust the TEST feed key; a release must not contain it."
}
Write-Host "No test-key marker in $($dlls.Count) DLL(s) under '$Path'."
