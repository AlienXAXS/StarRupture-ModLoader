<#
.SYNOPSIS
  Resolves the function names in a StarRupture ModLoader crash report offline.

.DESCRIPTION
  The crash dialog symbolizes its own stack in-process, but only from a PDB it
  can find next to each loaded module. When that PDB was not deployed -- or the
  report arrives from someone else's machine, which is the normal case -- every
  mod loader frame comes back named after the nearest preceding export with a
  five-digit displacement, e.g.

    #05 0x00007FFA5A5FDB24  StarRupture-ModLoader-Core.dll!Core_Detach + 0x362CFF  (module+0x36DB24)

  That name is not a near miss, it is an unrelated function; the only real
  information in the line is the (module+0x...) offset. This script feeds those
  offsets back through DbgHelp against a local build's PDB and prints what the
  report would have said:

    #05 Hooks::SaveLoaded::Install+0x34   [save_loaded.cpp:102]

  Reports produced by loader builds that include the module table pick their
  own build automatically: each module is matched by PE SizeOfImage, so a
  Client Debug report resolves against the Client Debug binaries even with
  every other configuration sitting in build\ alongside them. Older reports
  without that table fall back to newest-file-wins, and say so.

.PARAMETER Report
  Path to a file holding the pasted crash report. Omit to read stdin, so
  `Get-Clipboard | .\symbolize-crash.ps1` works.

.PARAMETER SearchPath
  Directories searched recursively for the DLL/EXE named in each frame; its
  .pdb must sit next to it. Defaults to the repo's build\ folder.

.PARAMETER Module
  One-off mode: skip report parsing and resolve -Offsets against this binary.

.PARAMETER Offsets
  Module-relative offsets in hex for -Module mode. Quote anything written with
  a 0x prefix -- unquoted, PowerShell parses it as a number literal and passes
  the decimal value on, which resolves to the wrong address without error.

.EXAMPLE
  .\tools\symbolize-crash.ps1 -Report crash.txt

.EXAMPLE
  Get-Clipboard | .\tools\symbolize-crash.ps1

.EXAMPLE
  .\tools\symbolize-crash.ps1 -Module "build\Client Release\ModLoader\StarRupture-ModLoader-Core.dll" -Offsets 31E1F,36DB24
#>
# Deliberately NOT an advanced script (no [CmdletBinding()], no parameter
# sets): an advanced script rejects pipeline input unless some parameter
# declares ValueFromPipeline, and $input is empty in it -- which would cost
# `Get-Clipboard | .\symbolize-crash.ps1`, the way this actually gets used.
# The two modes are separated by hand below instead.
param(
    [string]   $Report,
    [string[]] $SearchPath,
    [string]   $Module,
    [string[]] $Offsets
)

# $input is a one-shot enumerator: drain it before anything else touches it.
$PipedLines = @($input)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Module -and -not $Offsets) { throw "-Module needs -Offsets (module-relative offsets in hex)." }
if ($Offsets -and -not $Module) { throw "-Offsets needs -Module (the binary to resolve them against)." }

# ---------------------------------------------------------------------------
# DbgHelp interop
#
# SYMBOL_INFO is a variable-length struct (the name is written past its end),
# so it is allocated by hand rather than marshalled: SizeOfStruct is 88 --
# the size of the fixed part only, NOT of the buffer -- MaxNameLen sits at
# offset 80, and the name begins at offset 84.
# ---------------------------------------------------------------------------
if (-not ('SrSym' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SrSym
{
    [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool SymCleanup(IntPtr hProcess);

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern uint SymSetOptions(uint SymOptions);

    [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern ulong SymLoadModuleEx(IntPtr hProcess, IntPtr hFile, string ImageName,
        string ModuleName, ulong BaseOfDll, uint DllSize, IntPtr Data, uint Flags);

    [StructLayout(LayoutKind.Sequential)]
    public struct IMAGEHLP_LINE64
    {
        public uint SizeOfStruct;
        public IntPtr Key;
        public uint LineNumber;
        public IntPtr FileName;
        public ulong Address;
    }

    [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern bool SymGetLineFromAddr64(IntPtr hProcess, ulong dwAddr,
        out uint pdwDisplacement, ref IMAGEHLP_LINE64 Line);

    [DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Ansi)]
    public static extern bool SymFromAddr(IntPtr hProcess, ulong Address, out ulong Displacement, IntPtr Symbol);

    const int FixedSize = 88;   // sizeof(SYMBOL_INFO) without the name
    const int MaxName   = 1024;

    public static string Resolve(IntPtr h, ulong addr)
    {
        IntPtr buf = Marshal.AllocHGlobal(FixedSize + MaxName);
        try
        {
            for (int i = 0; i < FixedSize + MaxName; i++) Marshal.WriteByte(buf, i, 0);
            Marshal.WriteInt32(buf, 0,  FixedSize);
            Marshal.WriteInt32(buf, 80, MaxName);

            ulong disp;
            if (!SymFromAddr(h, addr, out disp, buf))
                return null;

            string name = Marshal.PtrToStringAnsi(IntPtr.Add(buf, 84));
            if (disp != 0)
                name += "+0x" + disp.ToString("X");

            IMAGEHLP_LINE64 line = new IMAGEHLP_LINE64();
            line.SizeOfStruct = (uint)Marshal.SizeOf(typeof(IMAGEHLP_LINE64));
            uint ldisp;
            if (SymGetLineFromAddr64(h, addr, out ldisp, ref line) && line.FileName != IntPtr.Zero)
            {
                string file = Marshal.PtrToStringAnsi(line.FileName);
                int slash = file.LastIndexOf('\\');
                if (slash >= 0) file = file.Substring(slash + 1);
                name += "   [" + file + ":" + line.LineNumber + "]";
            }
            return name;
        }
        finally { Marshal.FreeHGlobal(buf); }
    }
}
'@
}

$script:SymHandle = [IntPtr]0x5253    # any unique non-null value; no real process is opened
$script:SymReady  = $false

function Initialize-Sym {
    if ($script:SymReady) { return }
    # DbgHelp's state is per-process, not per-script, so a second run in the
    # same PowerShell session would otherwise inherit the first run's loaded
    # modules and reject every SymInitialize/SymLoadModuleEx after it. Failure
    # here is expected and ignored on the first run, where there is nothing to
    # clean up.
    [void][SrSym]::SymCleanup($script:SymHandle)
    # SYMOPT_UNDNAME (0x02) | SYMOPT_LOAD_LINES (0x10)
    [void][SrSym]::SymSetOptions(0x12)
    [void][SrSym]::SymInitialize($script:SymHandle, $null, $false)
    $script:SymReady = $true
}

# Distinct fake load address per module so several can be loaded at once.
$script:NextBase = [uint64]0x10000000
function Add-SymModule([string] $imagePath) {
    Initialize-Sym
    $base = $script:NextBase
    $script:NextBase += [uint64]0x10000000
    $loaded = [SrSym]::SymLoadModuleEx($script:SymHandle, [IntPtr]::Zero, $imagePath, $null, $base, 0, [IntPtr]::Zero, 0)
    if ($loaded -ne 0) { return $loaded }
    # SymLoadModuleEx signals "already loaded at that base" as a 0 return with
    # ERROR_SUCCESS, which is a success for our purposes and not a failure.
    if ([System.Runtime.InteropServices.Marshal]::GetLastWin32Error() -eq 0) { return $base }
    return $null
}

# SizeOfImage from the PE optional header. Used to tell one configuration's
# copy of a DLL from another's, which is the only way to pick the right
# binary automatically when build\ holds eight of them.
function Get-PESizeOfImage([string] $path) {
    try {
        $fs = [System.IO.File]::OpenRead($path)
        try {
            $br = New-Object System.IO.BinaryReader($fs)
            $fs.Position = 0x3C
            $peOff = $br.ReadUInt32()
            $fs.Position = $peOff
            if ($br.ReadUInt32() -ne 0x00004550) { return $null }   # "PE\0\0"
            $fs.Position = $peOff + 24 + 56                          # optional header + SizeOfImage
            return [uint64]$br.ReadUInt32()
        } finally { $fs.Dispose() }
    } catch { return $null }
}

# ---------------------------------------------------------------------------
# Direct mode
# ---------------------------------------------------------------------------
if ($Module) {
    if (-not (Test-Path -LiteralPath $Module)) { throw "No such file: $Module" }
    $full = (Resolve-Path -LiteralPath $Module).Path
    $base = Add-SymModule $full
    if (-not $base) { throw "SymLoadModuleEx failed for $full" }

    Write-Host "Module: $full" -ForegroundColor Cyan
    foreach ($o in $Offsets) {
        $hex = $o -replace '^0x', ''
        $off = [Convert]::ToUInt64($hex, 16)
        $name = [SrSym]::Resolve($script:SymHandle, $base + $off)
        if (-not $name) { $name = '<no symbol>' }
        Write-Host ("  module+0x{0}  ->  {1}" -f $hex.ToUpper(), $name)
    }
    return
}

# ---------------------------------------------------------------------------
# Report mode
# ---------------------------------------------------------------------------
if ($Report) {
    if (-not (Test-Path -LiteralPath $Report)) { throw "No such file: $Report" }
    $lines = Get-Content -LiteralPath $Report
} else {
    $lines = $PipedLines
    if ($lines.Count -eq 0) {
        throw "No report supplied. Pass -Report <file>, or pipe one in (Get-Clipboard | .\symbolize-crash.ps1)."
    }
}

if (-not $SearchPath -or $SearchPath.Count -eq 0) {
    $SearchPath = @(Join-Path (Split-Path -Parent $PSScriptRoot) 'build')
}
$SearchPath = @($SearchPath | Where-Object { Test-Path -LiteralPath $_ })
if ($SearchPath.Count -eq 0) { throw "None of the given -SearchPath directories exist." }

# Module table from the report, when the loader build that produced it emitted
# one: "  StarRupture-ModLoader-Core.dll  base=0x...  size=0x0076A000  symbols=..."
$reportedSize = @{}
foreach ($l in $lines) {
    if ($l -match '^\s{2,}(\S+\.(?:dll|exe|DLL|EXE))\s+base=0x[0-9A-Fa-f]+\s+size=0x([0-9A-Fa-f]+)') {
        $reportedSize[$Matches[1].ToLower()] = [Convert]::ToUInt64($Matches[2], 16)
    }
}

$script:ModuleBase = @{}   # module file name (lower) -> fake base, or $null when unresolvable
$script:Ambiguous  = @{}

function Get-ModuleBase([string] $name) {
    $key = $name.ToLower()
    if ($script:ModuleBase.ContainsKey($key)) { return $script:ModuleBase[$key] }

    $candidates = @()
    foreach ($dir in $SearchPath) {
        $candidates += Get-ChildItem -LiteralPath $dir -Recurse -File -Filter $name -ErrorAction SilentlyContinue
    }
    # Only a binary with its PDB beside it is any use here.
    $candidates = @($candidates | Where-Object {
        Test-Path -LiteralPath ([System.IO.Path]::ChangeExtension($_.FullName, '.pdb'))
    })

    if ($reportedSize.ContainsKey($key) -and $candidates.Count -gt 1) {
        $want = $reportedSize[$key]
        $bySize = @($candidates | Where-Object { (Get-PESizeOfImage $_.FullName) -eq $want })
        if ($bySize.Count -ge 1) { $candidates = $bySize }
    }

    if ($candidates.Count -eq 0) { $script:ModuleBase[$key] = $null; return $null }
    if ($candidates.Count -gt 1) {
        $candidates = @($candidates | Sort-Object LastWriteTime -Descending)
        $script:Ambiguous[$key] = [pscustomobject]@{
            Count   = $candidates.Count
            HadSize = $reportedSize.ContainsKey($key)
        }
    }

    $chosen = $candidates[0]
    Write-Host ("  using {0}" -f $chosen.FullName) -ForegroundColor DarkGray
    $base = Add-SymModule $chosen.FullName
    $script:ModuleBase[$key] = $base
    return $base
}

Write-Host "Symbolizing against:" -ForegroundColor Cyan
foreach ($d in $SearchPath) { Write-Host ("  {0}" -f (Resolve-Path -LiteralPath $d).Path) -ForegroundColor DarkGray }
Write-Host ""

$resolvedAny = $false

foreach ($line in $lines) {
    # Both frame shapes the crash reporter emits:
    #   #NN 0xADDR  module.dll!Symbol + 0xD  (module+0xM)[  extras]
    #   #NN 0xADDR  module.dll + 0xM
    $modName = $null
    $offHex  = $null

    if ($line -match '^\s*#\d+\s+0x[0-9A-Fa-f]+\s+(\S+?)!.*\(module\+0x([0-9A-Fa-f]+)\)') {
        $modName = $Matches[1]; $offHex = $Matches[2]
    }
    elseif ($line -match '^\s*#\d+\s+0x[0-9A-Fa-f]+\s+(\S+?)\s+\+\s+0x([0-9A-Fa-f]+)\s*$') {
        $modName = $Matches[1]; $offHex = $Matches[2]
    }

    if (-not $modName) { Write-Output $line; continue }

    $base = Get-ModuleBase $modName
    if (-not $base) { Write-Output "$line   <- no local build of $modName with a .pdb"; continue }

    $name = [SrSym]::Resolve($script:SymHandle, $base + [Convert]::ToUInt64($offHex, 16))
    if (-not $name) { Write-Output "$line   <- no symbol at that offset"; continue }

    $resolvedAny = $true
    $prefix = if ($line -match '^\s*(#\d+)') { $Matches[1] } else { '#??' }
    Write-Output ("{0} {1}!{2}" -f $prefix, $modName, $name)
}

Write-Host ""
foreach ($k in $script:Ambiguous.Keys) {
    $a = $script:Ambiguous[$k]
    $why = if ($a.HadSize) { "none of which matches the size in the report" } else { "and the report carries no module table to choose by" }
    Write-Warning ("{0}: {1} local builds matched, {2}. Used the newest, so the names above may be from the wrong configuration -- pass -SearchPath to narrow it." -f $k, $a.Count, $why)
}
if (-not $resolvedAny) {
    Write-Warning "Nothing resolved. Check that -SearchPath holds a build of the same configuration as the crash, with its .pdb files intact."
}
