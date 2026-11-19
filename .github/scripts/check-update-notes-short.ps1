<#
.SYNOPSIS
  Checks UPDATE_NOTES_SHORT.txt against the limits of the Windows update box.

.DESCRIPTION
  The Windows "Update Available" box prints the manifest's "notes" field in
  full, so that field carries UPDATE_NOTES_SHORT.txt: one change per line, at
  most 6 lines and 600 characters (trimmed, empty lines left out, lines joined
  by a newline).

  release.yml, draft.yml and the pull request check all call this script, so
  the limits live here and nowhere else. The script only reports; the caller
  decides what a problem means (release.yml stops a release run, and only
  warns on a preview; draft.yml only warns).

.OUTPUT
  An object with: Text (the lines joined by a newline, "" when none), Lines,
  Chars, MaxLines, MaxChars, Kind ('Empty', 'TooLong' or $null) and Problem
  (a sentence without a final period, $null when the file is fine).
#>
param(
  [string]$Path = 'UPDATE_NOTES_SHORT.txt'
)

$ErrorActionPreference = 'Stop'

$maxLines = 6
$maxChars = 600

$lines = @()
if (Test-Path $Path) {
  $lines = @(Get-Content $Path | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
$text = $lines -join "`n"

$kind = $null
$problem = $null
if ($lines.Count -eq 0) {
  $kind = 'Empty'
  $problem = "UPDATE_NOTES_SHORT.txt is missing or empty"
} elseif ($lines.Count -gt $maxLines -or $text.Length -gt $maxChars) {
  $kind = 'TooLong'
  $problem = "UPDATE_NOTES_SHORT.txt has $($lines.Count) lines and $($text.Length) characters (limit $maxLines and $maxChars)"
}

[pscustomobject]@{
  Text     = $text
  Lines    = $lines.Count
  Chars    = $text.Length
  MaxLines = $maxLines
  MaxChars = $maxChars
  Kind     = $kind
  Problem  = $problem
}
