<#
.SYNOPSIS
  Builds the per-version release notes history a manifest carries in its
  "release_notes" field.

.DESCRIPTION
  Each release tag keeps its own notes file (one change per line). This script
  puts the current release's notes (read from the working tree, since its tag
  may not exist yet) on top and the notes of earlier release tags below,
  newest first, each under a header line:

      ## v1.23.1
      - first change
      - second change

      ## v1.23.0
      - ...

  Nothing in it is specific to one repository: the repository, the notes file
  name, the tag pattern and the bounds are parameters.

  Tags that do not match -TagPattern (release candidates and other labels) are
  left out, tags are ordered by version number (not by date), tags newer than
  or equal to -Version are left out, and a tag with no notes file or an empty
  one is skipped and listed in the result's Skipped (and in one ::notice::
  line), never silently.

  The checkout must have the tags (actions/checkout with fetch-depth: 0). The
  script throws when the repository has no tags at all and -Tags was not given.

.OUTPUT
  An object with: Text (the release_notes string, "" when nothing at all),
  Included (tags in the text, newest first), Skipped (objects with Tag and
  Reason), Truncated (older tags dropped for
  the bounds).
#>
param(
  [string]$Repo = '.',
  [string]$NotesFile = 'UPDATE_NOTES.txt',
  [Parameter(Mandatory = $true)][string]$Version,
  [string[]]$Tags,
  [string]$TagPattern = '^v?\d+\.\d+\.\d+$',
  [int]$MaxVersions = 10,
  [int]$MaxBytes = 65536
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function ConvertTo-VersionKey([string]$tag) {
  $m = [regex]::Match($tag, '^[vV]?(\d+)\.(\d+)\.(\d+)')
  if (-not $m.Success) { return $null }
  return [int64]$m.Groups[1].Value * 1000000000000 + [int64]$m.Groups[2].Value * 1000000 + [int64]$m.Groups[3].Value
}

function Get-CleanLines([string[]]$raw) {
  $out = @()
  foreach ($l in $raw) {
    $t = $l.Trim([char]0xFEFF, ' ', "`t", "`r")
    if (-not $t) { continue }
    $out += $t
  }
  return ,$out
}

$curKey = ConvertTo-VersionKey $Version
if ($null -eq $curKey) { throw "Version '$Version' is not a version number" }

$skipped  = @()
$sections = @()   # each: @{ Tag; Lines }

# The current release: the working tree file.
$curPath = Join-Path $Repo $NotesFile
$curLines = @()
if (Test-Path $curPath) {
  $curLines = Get-CleanLines (Get-Content $curPath -Encoding UTF8)
}
if ($curLines.Count -gt 0) {
  $sections += @{ Tag = $Version; Lines = $curLines }
} else {
  $skipped += [pscustomobject]@{ Tag = $Version; Reason = "no $NotesFile text in the working tree" }
}

# Earlier release tags, newest version first.
if (-not $Tags) {
  $Tags = @(git -C $Repo tag)
  if ($Tags.Count -eq 0) { throw "The repository at '$Repo' has no tags; fetch them (actions/checkout with fetch-depth: 0) or pass -Tags." }
}
$release = @($Tags | Where-Object { $_ -match $TagPattern -and $null -ne (ConvertTo-VersionKey $_) } |
             Where-Object { (ConvertTo-VersionKey $_) -lt $curKey } |
             Sort-Object -Descending { ConvertTo-VersionKey $_ })

foreach ($tag in $release) {
  if (-not (git -C $Repo ls-tree --name-only $tag -- $NotesFile)) {
    $skipped += [pscustomobject]@{ Tag = $tag; Reason = "no $NotesFile at this tag" }
    continue
  }
  $lines = Get-CleanLines (git -C $Repo show "${tag}:${NotesFile}")
  if ($lines.Count -eq 0) {
    $skipped += [pscustomobject]@{ Tag = $tag; Reason = "$NotesFile is empty at this tag" }
    continue
  }
  $sections += @{ Tag = $tag; Lines = $lines }
}

# Bounds: at most MaxVersions sections and MaxBytes of UTF-8 text. The current
# release always stays; older ones are dropped from the oldest end.
$enc = New-Object System.Text.UTF8Encoding($false)
$kept = @()
$text = ''
$truncated = 0
foreach ($s in $sections) {
  $block = "## $($s.Tag)`n" + ($s.Lines -join "`n")
  $candidate = if ($text) { "$text`n`n$block" } else { $block }
  $isFirst = ($kept.Count -eq 0)
  if (-not $isFirst -and ($kept.Count -ge $MaxVersions -or $enc.GetByteCount($candidate) -gt $MaxBytes)) {
    $truncated++
    continue
  }
  if ($isFirst -and $enc.GetByteCount($candidate) -gt $MaxBytes) {
    Write-Host "::warning title=Release notes large::The notes of $($s.Tag) alone are over $MaxBytes bytes."
  }
  $text = $candidate
  $kept += $s.Tag
}

if ($skipped.Count -gt 0) {
  Write-Host "::notice title=Release notes::skipped $($skipped.Count) tag(s) without usable notes: $(($skipped | ForEach-Object { $_.Tag }) -join ', ')"
}

[pscustomobject]@{
  Text      = $text
  Included  = $kept
  Skipped   = $skipped
  Truncated = $truncated
}
