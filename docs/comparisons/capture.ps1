<#
.SYNOPSIS
    Deterministic raster-vs-ray-traced comparison capture for reVC-RTGI.

.DESCRIPTION
    Drives a built reVC.exe through a fixed manifest of scenes. For each scene
    it produces a matched pair from an identical camera pose:

      docs/comparisons/<slug>_raster.png  - RTGI off (enabled=0), a still.
      docs/comparisons/<slug>_rtgi.gif    - RTGI on  (enabled=1), a short loop.

    The ray-traced side is a GIF on purpose: with the camera held still, the
    loop captures the motion a still can't -- traffic sweeping through the
    wet-road reflections, rain, rippling water, pedestrians. Frames are grabbed
    from the game's rotating BMP dumps and assembled by assemble_gif.py
    (Python + Pillow).

    Because the camera pose, time of day, weather and resolution are all pinned
    by the manifest, the process is repeatable: re-run it after a rendering
    change and `git diff` the raster PNGs / eyeball the GIFs to see what moved.

    NOT deterministic: ambient traffic and pedestrians spawn randomly, so each
    run differs in the cars/peds present -- which is the point of the GIF. If a
    car parks in front of the camera, just re-run that scene (-Scenes <slug>).

    Requirements: a built reVC.exe + Vice City assets (NOT part of this repo --
    point -GameDir at your install), and `py` with Pillow for GIF assembly.

.EXAMPLE
    pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game
.EXAMPLE
    pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game -Scenes rain
.EXAMPLE
    # Raster stills only (skip the GIF pass):
    pwsh -File docs/comparisons/capture.ps1 -GameDir C:\path\to\re3-game -NoGif
#>
[CmdletBinding()]
param(
    # Local Vice City install holding a built reVC.exe (not tracked in this repo).
    [string]$GameDir = "C:\Users\PC\Downloads\re3-game",
    # Where the <slug>_raster.png / <slug>_rtgi.gif files land.
    [string]$OutDir = $PSScriptRoot,
    # Subset of scene slugs to capture; empty = every scene in the manifest.
    [string[]]$Scenes = @(),
    # Seconds to render the raster still before grabbing it (>= ~85s => 4 dumps).
    [int]$Seconds = 90,
    # Frames between BMP dumps for the raster still.
    [int]$ShotFrames = 250,
    # Produce a static <slug>_rtgi.png instead of the GIF (no Python needed).
    [switch]$NoGif,
    # Regenerate only the ray-traced GIF side, keeping existing raster PNGs.
    [switch]$GifOnly,
    # --- GIF (ray-traced motion) knobs ---
    [int]$GifShotFrames = 2,     # game dumps a BMP every N rendered frames (small = denser)
    [int]$GifSettle = 8,         # extra seconds after load for traffic/GI to settle
    [int]$GifSeconds = 25,       # max length of the frame-collection window
    [int]$GifMaxFrames = 54,     # cap on frames kept
    [int]$GifEveryN = 2,         # keep every Nth distinct dump (spreads motion over more world-time)
    [int]$GifPollMs = 70,        # collection poll interval (< the ~66ms dump interval)
    [int]$GifWidth = 720,        # output GIF width (px)
    [double]$GifFps = 12,        # playback frame rate
    [int]$GifColors = 256,       # palette size
    # Leave harness files in the game dir instead of cleaning up at the end.
    [switch]$KeepHarnessFiles
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# While ICONIFIED, the GL driver defers presents, and the GL half of the RTGI
# interop stops draining — the VK frame fence starves and RT disables itself
# ("frame fence wait failed"). For live capture the window must be RESTORED,
# yet stay off the user's desktop: set the restore position off-screen while
# still minimized, then show without activating (no flash, no focus steal,
# bottom of z-order).
Add-Type -Namespace RtgiCap -Name Win32 -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct RECT { public int L, T, R, B; }
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct POINT { public int X, Y; }
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct WINDOWPLACEMENT { public int length, flags, showCmd; public POINT ptMin, ptMax; public RECT rcNormal; }
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool GetWindowPlacement(System.IntPtr h, ref WINDOWPLACEMENT p);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetWindowPlacement(System.IntPtr h, ref WINDOWPLACEMENT p);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr h, System.IntPtr after, int x, int y, int w, int hh, uint flags);
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h);
'@

function Find-GameWindowHandle([int]$timeoutSec = 10) {
    # Process.MainWindowHandle is 0 for minimized windows on some frames, so
    # enumerate top-level windows by PID instead, retrying until one appears.
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $procIds = @(Get-Process reVC -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
        if ($procIds.Count) {
            $state = @{ found = [System.IntPtr]::Zero }
            $cb = [RtgiCap.Win32+EnumProc]{
                param($h, $l)
                $wpid = [uint32]0
                [void][RtgiCap.Win32]::GetWindowThreadProcessId($h, [ref]$wpid)
                # visibility filter: GLFW also owns hidden helper windows; the
                # real game window has WS_VISIBLE even while iconified
                if ($procIds -contains [int]$wpid -and [RtgiCap.Win32]::IsWindowVisible($h)) { $state.found = $h; return $false }
                return $true
            }.GetNewClosure()
            [void][RtgiCap.Win32]::EnumWindows($cb, [System.IntPtr]::Zero)
            if ($state.found -ne [System.IntPtr]::Zero) { return $state.found }
        }
        Start-Sleep -Milliseconds 500
    }
    return [System.IntPtr]::Zero
}

function Move-GameWindowOffscreenRestored([int]$timeoutSec = 25) {
    # Retry until the window is verifiably de-iconified (the game re-creates /
    # re-shows its window during startup, which can undo an early restore).
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $h = Find-GameWindowHandle 5
        if ($h -ne [System.IntPtr]::Zero) {
            $wp = New-Object RtgiCap.Win32+WINDOWPLACEMENT
            $wp.length = [System.Runtime.InteropServices.Marshal]::SizeOf([type][RtgiCap.Win32+WINDOWPLACEMENT])
            [void][RtgiCap.Win32]::GetWindowPlacement($h, [ref]$wp)
            $w = $wp.rcNormal.R - $wp.rcNormal.L; $ht = $wp.rcNormal.B - $wp.rcNormal.T
            $wp.rcNormal.L = -32000; $wp.rcNormal.T = 200
            $wp.rcNormal.R = -32000 + $w; $wp.rcNormal.B = 200 + $ht
            $wp.showCmd = 4   # SW_SHOWNOACTIVATE
            [void][RtgiCap.Win32]::SetWindowPlacement($h, [ref]$wp)
            [void][RtgiCap.Win32]::SetWindowPos($h, [System.IntPtr]1, 0, 0, 0, 0, 0x0413) # HWND_BOTTOM, NOSIZE|NOMOVE|NOACTIVATE|SHOWWINDOW
            Start-Sleep -Milliseconds 300
            if (-not [RtgiCap.Win32]::IsIconic($h)) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Warning 'off-screen restore did not stick within timeout'
    return $false
}

# --- Scene manifest: the single source of truth for the comparison set. ---
# Keep in sync with the table in the top-level README.md. Coordinates are
# proven-good; heading is degrees (0 = north, CCW, so 90 = west, 270 = east)
# and snaps the camera behind the player. Do not teleport into water (Tommy
# cannot swim). weather: 0 sunny, 2 rainy.
$Manifest = @(
    @{ slug = 'rain';   tp = '250,-1283,12,0';   hour = 14; weather = 2; desc = 'Rain, Ocean Drive' }
    @{ slug = 'night';  tp = '250,-1283,12,90';  hour = 2;  weather = 0; desc = 'Night, Ocean View neon' }
    @{ slug = 'noon';   tp = '250,-1283,12,0';   hour = 12; weather = 0; desc = 'Noon, Ocean Drive' }
    @{ slug = 'sunset'; tp = '400,-1385,9,90';   hour = 19; weather = 0; desc = 'Golden sunset, beach to skyline' }
    @{ slug = 'dawn';   tp = '430,-1670,10,270'; hour = 7;  weather = 0; desc = 'Dawn, lighthouse channel' }
)

$WindowRes = '1920 1080'   # also the screenshot resolution
$exe = Join-Path $GameDir 'reVC.exe'
$assembler = Join-Path $PSScriptRoot 'assemble_gif.py'

function Resolve-Ffmpeg {
    # Prefer ffmpeg (better palette + per-rect diff encoding than Pillow).
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $link = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ffmpeg.exe'
    if (Test-Path $link) { return $link }
    $pkg = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Gyan.FFmpeg*') -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pkg) { return $pkg.FullName }
    return $null
}
$ffmpeg = Resolve-Ffmpeg

if (-not (Test-Path $exe)) { throw "reVC.exe not found in $GameDir (build it first, or pass -GameDir)." }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

function Stop-Game {
    Get-Process reVC -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Write-Harness([string[]]$configLines, [string]$windowRes = $WindowRes) {
    # Written every run so the outcome does not depend on prior game-dir state.
    Set-Content -Path (Join-Path $GameDir 'rtgi_autoload.txt') -Value '1'        -Encoding ascii
    Set-Content -Path (Join-Path $GameDir 'rtgi_window.txt')   -Value $windowRes -Encoding ascii
    Set-Content -Path (Join-Path $GameDir 'rtgi_config.txt')   -Value $configLines -Encoding ascii
}

function Get-NewestCompleteShot {
    # The dumps rotate rtgi_shot_0..3.bmp; a run killed (or polled) mid-write can
    # leave one truncated file. Complete dumps share the same (largest) byte
    # length, so keep the newest file whose length equals that maximum.
    $shots = Get-ChildItem (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
    if (-not $shots) { return $null }
    $full = ($shots | Measure-Object -Property Length -Maximum).Maximum
    $shots | Where-Object { $_.Length -eq $full } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Wait-ForInit([int]$timeoutSec = 70) {
    # Init writes "forced weather ..." to rtgi.log once loaded/teleported/clocked.
    $log = Join-Path $GameDir 'rtgi.log'
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'forced weather' -Quiet)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Save-Png($srcBmp, $dest) {
    $img = [System.Drawing.Image]::FromFile($srcBmp)
    try { $img.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $img.Dispose() }
}

function Invoke-CaptureRaster($scene) {
    Write-Harness @("enabled=0", "tp=$($scene.tp)", "hour=$($scene.hour)", "weather=$($scene.weather)", "shotframes=$ShotFrames")
    Stop-Game
    Remove-Item (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
    Write-Host ("[{0}/raster] {1}" -f $scene.slug, $scene.desc)
    Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Minimized
    Start-Sleep -Seconds $Seconds
    Stop-Game
    $shot = Get-NewestCompleteShot
    if (-not $shot) { Write-Warning ("[{0}/raster] no screenshot produced" -f $scene.slug); return }
    $dest = Join-Path $OutDir ("{0}_raster.png" -f $scene.slug)
    Save-Png $shot.FullName $dest
    Write-Host ("[{0}/raster] {1} -> {2}" -f $scene.slug, $shot.Name, $dest)
}

function Invoke-CaptureRtgiStill($scene) {
    Write-Harness @("enabled=1", "tp=$($scene.tp)", "hour=$($scene.hour)", "weather=$($scene.weather)", "shotframes=$ShotFrames")
    Stop-Game
    Remove-Item (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
    Write-Host ("[{0}/rtgi] {1} (still)" -f $scene.slug, $scene.desc)
    Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Minimized
    Start-Sleep -Seconds $Seconds
    Stop-Game
    $shot = Get-NewestCompleteShot
    if (-not $shot) { Write-Warning ("[{0}/rtgi] no screenshot produced" -f $scene.slug); return }
    $dest = Join-Path $OutDir ("{0}_rtgi.png" -f $scene.slug)
    Save-Png $shot.FullName $dest
    Write-Host ("[{0}/rtgi] {1} -> {2}" -f $scene.slug, $shot.Name, $dest)
}

function Invoke-CaptureRtgiGif($scene) {
    # Boot with shotframes=0: dense glReadPixels dumps during the volatile
    # load/teleport/weather-force phase livelock the pipeline (world freezes,
    # dumps keep rewriting one stale frame). Dumping is armed only after the
    # scene settles, via the game's config hot-reload (devHarnessTick polls
    # rtgi_config.txt's mtime and re-applies it mid-run).
    Write-Harness @("enabled=1", "tp=$($scene.tp)", "hour=$($scene.hour)", "weather=$($scene.weather)", "shotframes=0")
    Stop-Game
    Remove-Item (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $GameDir 'rtgi.log') -ErrorAction SilentlyContinue

    $fdir = Join-Path ([System.IO.Path]::GetTempPath()) ("rtgigif_" + $scene.slug)
    if (Test-Path $fdir) { Remove-Item $fdir -Recurse -Force }
    New-Item -ItemType Directory -Path $fdir | Out-Null

    Write-Host ("[{0}/rtgi] {1} (gif: waiting for load...)" -f $scene.slug, $scene.desc)
    Start-Process -FilePath $exe -WorkingDirectory $GameDir -WindowStyle Minimized
    # Restore off-screen BEFORE ray tracing starts: with shotframes=0 there are
    # no glReadPixels calls to drain the iconified GL queue, and the VK frame
    # fence starves within seconds of RT frames flowing ("frame fence wait
    # failed"). Find-GameWindowHandle retries until the window exists, which is
    # well before the save finishes loading.
    [void](Move-GameWindowOffscreenRestored)
    if (-not (Wait-ForInit)) { Write-Warning ("[{0}/rtgi] init not detected; collecting anyway" -f $scene.slug) }
    Start-Sleep -Seconds $GifSettle          # scene stream-in, traffic, GI converge
    # Arm dumping mid-run. The armed config keeps the scene keys so a re-read
    # is idempotent (weather re-force is a no-op for an unchanged value), and
    # dumps start on the game's next mtime poll (~1 s).
    Set-Content -Path (Join-Path $GameDir 'rtgi_config.txt') -Value @(
        "enabled=1", "hour=$($scene.hour)", "weather=$($scene.weather)", "shotframes=$GifShotFrames") -Encoding ascii
    $armed = $false
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        if (Select-String -Path (Join-Path $GameDir 'rtgi.log') -Pattern 're-applied' -Quiet) { $armed = $true; break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $armed) { Write-Warning ("[{0}/rtgi] hot-reload not confirmed; collecting anyway" -f $scene.slug) }

    Write-Host ("[{0}/rtgi] collecting up to {1} frames" -f $scene.slug, $GifMaxFrames)
    # The game dumps ~15 BMPs/sec across the 4 rotating slots (0->1->2->3); a
    # slot mid-write is shorter than the complete size. NTFS only refreshes a
    # file's directory-entry LastWriteTime lazily (~6s) unless the file is
    # actually opened, so plain Get-ChildItem polling sees stale timestamps and
    # misses nearly every dump. We therefore OPEN each slot every poll, hash the
    # tail bytes, and dedup on content: new hash + complete length = new frame.
    $md5 = [System.Security.Cryptography.MD5]::Create()
    $seen = New-Object 'System.Collections.Generic.HashSet[string]'
    $kept = 0; $distinct = 0; $fullLen = 0L
    $buf = New-Object byte[] 4096
    $end = (Get-Date).AddSeconds($GifSeconds)
    while ((Get-Date) -lt $end -and $kept -lt $GifMaxFrames) {
        $batch = New-Object System.Collections.Generic.List[object]   # fresh complete dumps this poll
        foreach ($f in (Get-ChildItem (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue)) {
            try {
                $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
                try {
                    $len = $fs.Length
                    if ($len -gt $fullLen) { $fullLen = $len }
                    if ($len -ne $fullLen -or $len -lt 4096) { continue }  # mid-write dump
                    $null = $fs.Seek($len - 4096, 'Begin')
                    $null = $fs.Read($buf, 0, 4096)
                    $hash = [System.BitConverter]::ToString($md5.ComputeHash($buf))
                } finally { $fs.Close() }
                if ($seen.Add($hash)) {
                    # the open above forced a metadata refresh, so this is fresh
                    $batch.Add([pscustomobject]@{ Time = (Get-Item $f.FullName).LastWriteTime; Path = $f.FullName })
                }
            } catch { }  # slot being recreated under us; next poll gets it
        }
        foreach ($entry in ($batch | Sort-Object Time)) {
            $distinct++
            if ($entry.Path -and ($distinct % $GifEveryN) -eq 0 -and $kept -lt $GifMaxFrames) {
                $kept++
                Copy-Item $entry.Path (Join-Path $fdir ("frame_{0:D4}.bmp" -f $kept)) -Force -ErrorAction SilentlyContinue
            }
        }
        Start-Sleep -Milliseconds $GifPollMs
    }
    Stop-Game
    # Drop any frame torn by a truncate-under-copy race (size below the modal
    # size), then renumber so the sequence stays gapless for ffmpeg.
    $frames = Get-ChildItem (Join-Path $fdir 'frame_*.bmp') -ErrorAction SilentlyContinue
    if ($frames) {
        $good = ($frames | Measure-Object -Property Length -Maximum).Maximum
        $frames | Where-Object { $_.Length -ne $good } | Remove-Item -Force
        $i = 0
        Get-ChildItem (Join-Path $fdir 'frame_*.bmp') | Sort-Object Name | ForEach-Object {
            $i++
            $want = "frame_{0:D4}.bmp" -f $i
            if ($_.Name -ne $want) { Rename-Item $_.FullName $want }
        }
        $kept = $i
    }
    Write-Host ("[{0}/rtgi] {1} distinct dumps seen, {2} frames kept" -f $scene.slug, $distinct, $kept)

    if ($kept -lt 2) { Write-Warning ("[{0}/rtgi] too few frames collected; skipping GIF" -f $scene.slug); return }
    $dest = Join-Path $OutDir ("{0}_rtgi.gif" -f $scene.slug)
    Write-Host ("[{0}/rtgi] assembling {1} frames -> {2}" -f $scene.slug, $kept, $dest)
    if ($ffmpeg) {
        # High-quality GIF: per-clip palette weighted toward moving areas, bayer
        # dither, and rectangle diff mode so static regions aren't re-encoded.
        $vf = "scale=${GifWidth}:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=${GifColors}:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle"
        & $ffmpeg -y -loglevel error -framerate $GifFps -start_number 1 -i (Join-Path $fdir 'frame_%04d.bmp') -vf $vf -loop 0 $dest
        if ($LASTEXITCODE -eq 0) { Write-Host ("[{0}/rtgi] ffmpeg gif: {1:N2} MB" -f $scene.slug, ((Get-Item $dest).Length / 1MB)) }
        else { Write-Warning ("[{0}/rtgi] ffmpeg failed; falling back to Pillow" -f $scene.slug); & py $assembler $fdir $dest $GifWidth $GifFps $GifColors }
    } else {
        & py $assembler $fdir $dest $GifWidth $GifFps $GifColors
    }
    Remove-Item $fdir -Recurse -Force -ErrorAction SilentlyContinue
}

$todo = if ($Scenes.Count) { $Manifest | Where-Object { $Scenes -contains $_.slug } } else { $Manifest }
if (-not $todo) { throw "No matching scenes. Known slugs: $(( $Manifest.slug ) -join ', ')" }

foreach ($s in $todo) {
    if (-not $GifOnly) { Invoke-CaptureRaster $s }
    if ($NoGif) { Invoke-CaptureRtgiStill $s } else { Invoke-CaptureRtgiGif $s }
}

if (-not $KeepHarnessFiles) {
    'rtgi_config.txt', 'rtgi_window.txt', 'rtgi.log' | ForEach-Object {
        Remove-Item (Join-Path $GameDir $_) -ErrorAction SilentlyContinue
    }
    Remove-Item (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
}

Write-Host ("capture complete: {0} scene(s) into {1}" -f $todo.Count, $OutDir)
