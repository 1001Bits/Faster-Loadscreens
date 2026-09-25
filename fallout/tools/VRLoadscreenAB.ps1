<#
.SYNOPSIS
    A/B load-time comparison between the new Faster Loadscreens plugin
    (LoadingScreens.dll) and the old VRLoadingScreens plugin, on Fallout 4 VR
    under Mod Organizer 2.

.DESCRIPTION
    The flat-game harness (Fallout4PreloadBenchmark.ps1) stages into a real
    Data directory and rewrites plugins.txt. That is wrong for VR: MO2
    virtualizes the filesystem, so the only correct way to swap plugins is the
    profile's modlist.txt. This script does that and nothing else to the game.

    Both plugins emit a comparable close line, which is what makes a log-based
    comparison honest rather than stopwatch guesswork:

        new: Loading screen #1 closed - duration: 14.43s
        old: Loading screen #1 closed - duration: 3.18s (3181ms), ...

    Workflow (one game launch per run):

        .\VRLoadscreenAB.ps1 -Action Stage   -Variant new
        <launch through MO2, run the route, quit>
        .\VRLoadscreenAB.ps1 -Action Archive -Label new-cold-1

        .\VRLoadscreenAB.ps1 -Action Stage   -Variant old
        <reboot if comparing cold, launch, same route, quit>
        .\VRLoadscreenAB.ps1 -Action Archive -Label old-cold-1

        .\VRLoadscreenAB.ps1 -Action Report

    Archive IMMEDIATELY after quitting. Both plugins truncate their log on
    launch and neither keeps a previous copy, so the next launch destroys the
    run you just did.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Stage', 'Archive', 'Report', 'Status')]
    [string]$Action,

    # 'vanilla' loads OUR plugin with iBenchmarkMode=0, which returns early
    # before the animation-loop NOP, the overlays, the load budgets,
    # DoorPrefetch, NativePreloadPolicy and the performance patches. The plugin
    # times the loads and changes nothing else, so it is the control run for
    # "is the mod responsible for this load duration".
    [ValidateSet('new', 'old', 'vanilla')]
    [string]$Variant,

    [string]$Label,

    [string]$MO2Root = 'C:\Games\MO2',
    [string]$Profile = 'Default',
    [string]$LogDir = "$env:USERPROFILE\Documents\My Games\Fallout4VR\F4SE",
    [string]$StateRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'benchmark-state-vr')
)

$ErrorActionPreference = 'Stop'

# Mod folder names in MO2, and the log each plugin writes.
$NewMod = 'VR Loading Screens'
$OldMod = 'Old Loadscreens'
$NewLog = 'LoadingScreens.log'
$OldLog = 'VRLoadingScreens.log'

$ModListPath  = Join-Path $MO2Root "profiles\$Profile\modlist.txt"
$NewSettings  = Join-Path $MO2Root 'overwrite\MCM\Settings\FasterLoadscreens.ini'
$SessionFile  = Join-Path $StateRoot 'active-session.json'

function Assert-GameClosed {
    $proc = Get-Process -Name 'Fallout4VR' -ErrorAction SilentlyContinue
    if ($proc) {
        throw "Fallout4VR.exe is running. Quit the game first: staging edits modlist.txt and archiving reads a log the game holds open."
    }
}

function Get-Session {
    if (Test-Path -LiteralPath $SessionFile) {
        return (Get-Content -LiteralPath $SessionFile -Raw | ConvertFrom-Json).SessionPath
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $path  = Join-Path $StateRoot $stamp
    New-Item -ItemType Directory -Force -Path (Join-Path $path 'runs') | Out-Null
    @{ SessionPath = $path; CreatedUtc = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $SessionFile -Encoding UTF8
    return $path
}

function Set-ModEnabled {
    param([string]$ModName, [bool]$Enabled)
    $lines = Get-Content -LiteralPath $ModListPath
    $found = $false
    $out = foreach ($line in $lines) {
        if ($line.Length -gt 1 -and $line.Substring(1) -eq $ModName) {
            $found = $true
            if ($Enabled) { "+$ModName" } else { "-$ModName" }
        } else {
            $line
        }
    }
    if (-not $found) { throw "Mod '$ModName' not found in $ModListPath" }
    # Order is priority in MO2; only the +/- prefix is touched.
    Set-Content -LiteralPath $ModListPath -Value $out -Encoding UTF8
}

# Any enabled mod can supply a LoadingScreens.dll, and MO2 resolves the winner
# by priority (earliest line in modlist.txt wins). A stale copy of the plugin
# in another mod silently overrides the one being tested: a benchmark run was
# lost to exactly that, with the log reporting v2.1.5 while v2.1.13 sat in the
# mod this script had staged. Never infer the DLL under test from what was
# deployed - read it back from the winning mod.
function Get-PluginProviders {
    $providers = @()
    $priority = 0
    foreach ($line in Get-Content -LiteralPath $ModListPath) {
        if ($line.Length -lt 2 -or ($line[0] -ne '+' -and $line[0] -ne '-')) { continue }
        $priority++
        if ($line[0] -ne '+') { continue }
        $name = $line.Substring(1)
        foreach ($dll in @('LoadingScreens.dll', 'VRLoadingScreens.dll')) {
            $p = Join-Path $MO2Root "mods\$name\F4SE\Plugins\$dll"
            if (Test-Path -LiteralPath $p) {
                $providers += [pscustomobject]@{
                    Priority = $priority
                    Mod      = $name
                    Dll      = $dll
                    Sha256   = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 16)
                }
            }
        }
    }
    return $providers
}

function Assert-SingleProvider {
    $providers = Get-PluginProviders
    if ($providers.Count -le 1) { return $providers }
    Write-Warning "More than one enabled mod supplies a loading-screen plugin. MO2 gives the win to the lowest Priority number:"
    foreach ($p in $providers) {
        $mark = if ($p.Priority -eq ($providers | Measure-Object -Property Priority -Minimum).Minimum) { ' <-- WINS' } else { '' }
        Write-Warning ("  [{0,3}] {1,-26} {2,-22} {3}{4}" -f $p.Priority, $p.Mod, $p.Dll, $p.Sha256, $mark)
    }
    return $providers
}

function Get-ModEnabled {
    param([string]$ModName)
    foreach ($line in Get-Content -LiteralPath $ModListPath) {
        if ($line.Length -gt 1 -and $line.Substring(1) -eq $ModName) {
            return $line.StartsWith('+')
        }
    }
    return $null
}

function Get-DllInfo {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $h = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    $i = Get-Item -LiteralPath $Path
    return @{ Path = $Path; Sha256 = $h; Length = $i.Length; Modified = $i.LastWriteTimeUtc.ToString('o') }
}

# Parses both log dialects. Returns ordered load records.
function Get-LoadDurations {
    param([string]$LogPath)
    if (-not (Test-Path -LiteralPath $LogPath)) { return @() }
    $records = @()
    foreach ($line in Get-Content -LiteralPath $LogPath) {
        # "Loading screen #3 closed - duration: 4.06s (4064ms), ..."  (old)
        # "Loading screen #1 closed - duration: 14.43s"               (new)
        $m = [regex]::Match($line, 'Loading screen #(\d+) closed.*?duration:\s*([0-9.]+)s(?:\s*\((\d+)ms\))?')
        if (-not $m.Success) { continue }
        $index = [int]$m.Groups[1].Value
        $ms = if ($m.Groups[3].Success) { [int]$m.Groups[3].Value }
              else { [int][math]::Round([double]$m.Groups[2].Value * 1000) }
        # Load #0 on the old plugin reports an uninitialised timer (14326.81s);
        # it is a startup artifact, never a real transition.
        if ($index -eq 0 -and $ms -gt 600000) { continue }
        $records += [pscustomobject]@{ Index = $index; Ms = $ms; Seconds = [math]::Round($ms / 1000.0, 2) }
    }
    return $records
}

function Get-PluginVersion {
    param([string]$LogPath)
    if (-not (Test-Path -LiteralPath $LogPath)) { return 'n/a' }
    foreach ($line in Get-Content -LiteralPath $LogPath -TotalCount 5) {
        $m = [regex]::Match($line, "(LoadingScreens|VRLoadingScreens)'? v([0-9.]+)")
        if ($m.Success) { return "$($m.Groups[1].Value) v$($m.Groups[2].Value)" }
    }
    return 'unknown'
}

switch ($Action) {

    'Status' {
        Write-Output "modlist : $ModListPath"
        Write-Output ("  {0,-22} {1}" -f $NewMod, $(if (Get-ModEnabled $NewMod) { 'ENABLED' } else { 'disabled' }))
        Write-Output ("  {0,-22} {1}" -f $OldMod, $(if (Get-ModEnabled $OldMod) { 'ENABLED' } else { 'disabled' }))
        if (Test-Path -LiteralPath $NewSettings) {
            Write-Output "`nnew-plugin MCM settings ($NewSettings):"
            Get-Content -LiteralPath $NewSettings | ForEach-Object { "  $_" }
        }
        foreach ($l in @($NewLog, $OldLog)) {
            $p = Join-Path $LogDir $l
            if (Test-Path -LiteralPath $p) {
                $i = Get-Item -LiteralPath $p
                Write-Output ("`nlog {0,-24} {1,8} bytes  {2}  [{3}]" -f $l, $i.Length, $i.LastWriteTime, (Get-PluginVersion $p))
            }
        }
    }

    'Stage' {
        if (-not $Variant) { throw "-Variant new|old is required for Stage" }
        Assert-GameClosed
        $session = Get-Session

        # Disable every OTHER mod that ships one of these plugins, so the
        # staged variant is unambiguously the one the game loads.
        foreach ($p in Get-PluginProviders) {
            if ($p.Mod -ne $NewMod -and $p.Mod -ne $OldMod) {
                Set-ModEnabled -ModName $p.Mod -Enabled $false
                Write-Output "    disabled conflicting provider: $($p.Mod) ($($p.Dll))"
            }
        }

        if ($Variant -eq 'vanilla') {
            Set-ModEnabled -ModName $NewMod -Enabled $true
            Set-ModEnabled -ModName $OldMod -Enabled $false
            @(
                '[Main]'
                '; Timing only: the plugin measures and applies NOTHING.'
                '; No animation-loop NOP, no overlays, no load budgets,'
                '; no DoorPrefetch, no NativePreloadPolicy, no perf patches.'
                'iBenchmarkMode=0'
                'iLoadingScreenMode=3'
                ''
                '[Diagnostics]'
                'bVRPresentationProbe=0'
            ) | Set-Content -LiteralPath $NewSettings -Encoding UTF8
            Write-Output "    NOTE: iBenchmarkMode is startup-only - this needs a full game restart."
        } elseif ($Variant -eq 'new') {
            Set-ModEnabled -ModName $NewMod -Enabled $true
            Set-ModEnabled -ModName $OldMod -Enabled $false
            # Mode 3 (Background+Tips) is the closest match to the old plugin's
            # mode 2 (bg + tips), so the two are showing the same thing.
            # The diagnostic probe MUST be off: it adds work to the OpenVR
            # submit thread and a blocking GPU readback at the end of every
            # load, which would show up directly in these numbers.
            @(
                '[Main]'
                'iLoadingScreenMode=3'
                ''
                '[Diagnostics]'
                '; Off for timing runs - see VRLoadscreenAB.ps1'
                'bVRPresentationProbe=0'
            ) | Set-Content -LiteralPath $NewSettings -Encoding UTF8
        } else {
            Set-ModEnabled -ModName $NewMod -Enabled $false
            Set-ModEnabled -ModName $OldMod -Enabled $true
        }

        # Remove both logs so an archived run can never contain a stale session.
        foreach ($l in @($NewLog, $OldLog)) {
            $p = Join-Path $LogDir $l
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
        }

        Assert-SingleProvider | Out-Null
        $winner = Get-PluginProviders | Sort-Object Priority | Select-Object -First 1
        Write-Output ">>> staged variant '$Variant'"
        if ($winner) {
            Write-Output ("    plugin under test: {0} / {1} sha {2}" -f $winner.Mod, $winner.Dll, $winner.Sha256)
        }
        Write-Output ("    {0,-22} {1}" -f $NewMod, $(if (Get-ModEnabled $NewMod) { 'ENABLED' } else { 'disabled' }))
        Write-Output ("    {0,-22} {1}" -f $OldMod, $(if (Get-ModEnabled $OldMod) { 'ENABLED' } else { 'disabled' }))
        Write-Output "    logs cleared; session $session"
        Write-Output "    launch through MO2, run the route, quit, then: -Action Archive -Label <name>"
    }

    'Archive' {
        if (-not $Label) { throw "-Label <name> is required for Archive" }
        Assert-GameClosed
        $session = Get-Session
        $runDir  = Join-Path $session "runs\$Label"
        New-Item -ItemType Directory -Force -Path $runDir | Out-Null

        $newActive = Get-ModEnabled $NewMod
        # Read the variant back from what the plugin was actually configured
        # with, not from what we think we staged.
        $benchmarkOff = (Test-Path -LiteralPath $NewSettings) -and
            ((Get-Content -LiteralPath $NewSettings -Raw) -match '(?m)^\s*iBenchmarkMode\s*=\s*0')
        $variant = if (-not $newActive) { 'old' }
                   elseif ($benchmarkOff) { 'vanilla' }
                   else { 'new' }
        $logName   = if ($newActive) { $NewLog } else { $OldLog }
        $logPath   = Join-Path $LogDir $logName
        if (-not (Test-Path -LiteralPath $logPath)) {
            throw "No $logName in $LogDir. Did the game run with the '$variant' variant staged?"
        }

        Copy-Item -LiteralPath $logPath -Destination (Join-Path $runDir $logName) -Force
        $durations = Get-LoadDurations $logPath
        $dll = if ($newActive) {
            Get-DllInfo (Join-Path $MO2Root "mods\$NewMod\F4SE\Plugins\LoadingScreens.dll")
        } else {
            Get-DllInfo (Join-Path $MO2Root "mods\$OldMod\F4SE\Plugins\VRLoadingScreens.dll")
        }

        $manifest = [ordered]@{
            Label       = $Label
            Variant     = $variant
            Version     = Get-PluginVersion $logPath
            ArchivedUtc = (Get-Date).ToUniversalTime().ToString('o')
            Dll         = $dll
            LoadCount   = $durations.Count
            TotalMs     = ($durations | Measure-Object -Property Ms -Sum).Sum
            Loads       = $durations
        }
        $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $runDir 'run.json') -Encoding UTF8

        Write-Output ">>> archived '$Label' ($variant, $($manifest.Version))"
        foreach ($d in $durations) { Write-Output ("    load #{0,-2} {1,8:N2} s" -f $d.Index, $d.Seconds) }
        Write-Output ("    {0} loads, total {1:N2} s" -f $durations.Count, ($manifest.TotalMs / 1000.0))
    }

    'Report' {
        $session = Get-Session
        $runs = Get-ChildItem -LiteralPath (Join-Path $session 'runs') -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Get-Content -LiteralPath (Join-Path $_.FullName 'run.json') -Raw | ConvertFrom-Json }
        if (-not $runs) { Write-Output "No archived runs in $session"; break }

        Write-Output "Session: $session`n"
        foreach ($r in $runs) {
            Write-Output ("{0,-18} {1,-4} {2,-28} {3,2} loads  total {4,8:N2} s" -f `
                $r.Label, $r.Variant, $r.Version, $r.LoadCount, ($r.TotalMs / 1000.0))
        }

        # Per-leg comparison only where a new/old pair exists at the same index.
        $new = $runs | Where-Object Variant -eq 'new' | Select-Object -Last 1
        $old = $runs | Where-Object Variant -eq 'old' | Select-Object -Last 1
        if ($new -and $old) {
            Write-Output "`nPer-load, $($new.Label) vs $($old.Label):"
            Write-Output ("  {0,-6} {1,>10} {2,>10} {3,>10}" -f 'load', 'new', 'old', 'delta')
            $count = [math]::Min($new.Loads.Count, $old.Loads.Count)
            for ($i = 0; $i -lt $count; $i++) {
                $n = $new.Loads[$i]; $o = $old.Loads[$i]
                $delta = if ($o.Ms -gt 0) { (($n.Ms - $o.Ms) / [double]$o.Ms) * 100.0 } else { 0 }
                Write-Output ("  #{0,-5} {1,8:N2}s {2,9:N2}s {3,9:N1}%" -f $i, $n.Seconds, $o.Seconds, $delta)
            }
            if ($new.Loads.Count -ne $old.Loads.Count) {
                Write-Output ("  NOTE: run lengths differ ({0} vs {1}) - only the first {2} are compared, and the routes may not match." -f `
                    $new.Loads.Count, $old.Loads.Count, $count)
            }
            $nt = $new.TotalMs / 1000.0; $ot = $old.TotalMs / 1000.0
            Write-Output ("`n  total {0,8:N2}s {1,9:N2}s {2,9:N1}%" -f $nt, $ot, ((($nt - $ot) / $ot) * 100.0))
        }
    }
}
