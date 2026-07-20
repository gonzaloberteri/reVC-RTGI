<#
.SYNOPSIS
    Deterministic raster-vs-ray-traced comparison capture for reVC-RTGI,
    executed on the remote test box.

.DESCRIPTION
    Drives a built reVC.exe through a fixed manifest of scenes. For each scene
    it produces a matched pair from an identical camera pose:

      docs/comparisons/<slug>_raster.png  - RTGI off (enabled=0), a still.
      docs/comparisons/<slug>_rtgi.gif    - RTGI on  (enabled=1), a short loop.

    The GAME runs on the test box (192.168.0.209) so the dev PC's user is
    never interrupted: this orchestrator pushes the fresh exe + a job file,
    triggers the "rtgi-capture" scheduled task (console session - the game
    needs a real display/GPU), polls for the agent's done-marker over ssh,
    pulls the frames back and assembles PNG/GIF locally (ffmpeg preferred).
    The game-side half (off-screen window restore, hot-reload arming,
    content-hash frame dedup) lives in capture_agent.ps1, which this script
    pushes to the box and registers as the task action.

    Because the camera pose, time of day, weather and resolution are pinned
    by the manifest, the process is repeatable: re-run it after a rendering
    change and `git diff` the raster PNGs / eyeball the GIFs to see what
    moved. Ambient traffic/peds spawn randomly - that motion is the point of
    the GIF; if a car parks in front of the camera, re-run that scene
    (-Scenes <slug>).

.EXAMPLE
    powershell -File docs/comparisons/capture.ps1
.EXAMPLE
    powershell -File docs/comparisons/capture.ps1 -Scenes rain
.EXAMPLE
    powershell -File docs/comparisons/capture.ps1 -NoGif   # stills only
#>
[CmdletBinding()]
param(
    [string]$RemoteHost = '192.168.0.209',
    # Where the <slug>_raster.png / <slug>_rtgi.gif files land (local).
    # Defaults to this script's directory ($PSScriptRoot is empty at
    # param-binding time under Windows PowerShell -File, so resolve below).
    [string]$OutDir = '',
    # Subset of scene slugs to capture; empty = every scene in the manifest.
    [string[]]$Scenes = @(),
    # Seconds to render a still before grabbing it (>= ~85s => 4 dumps).
    [int]$Seconds = 90,
    [int]$ShotFrames = 250,
    # Produce a static <slug>_rtgi.png instead of the GIF.
    [switch]$NoGif,
    # Regenerate only the ray-traced GIF side, keeping existing raster PNGs.
    [switch]$GifOnly,
    # Skip pushing the exe (reuse whatever is on the box).
    [switch]$SkipExe,
    # --- GIF (ray-traced motion) knobs; see capture_agent.ps1 ---
    [int]$GifShotFrames = 2,
    [int]$GifSettle = 8,
    [int]$GifSeconds = 25,
    [int]$GifMaxFrames = 54,
    [int]$GifEveryN = 2,
    [int]$GifPollMs = 70,
    [int]$GifWidth = 720,
    [double]$GifFps = 12,
    [int]$GifColors = 256
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not $OutDir) { $OutDir = $PSScriptRoot }

$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$exe = Join-Path $repo 'bin\win-amd64-librw_gl3_glfw-oal\Release\reVC.exe'
$agent = Join-Path $PSScriptRoot 'capture_agent.ps1'
$assembler = Join-Path $PSScriptRoot 'assemble_gif.py'
$rGame = 'C:/Users/pc/Downloads/re3-game'
$rFrames = 'C:/Users/pc/rtgi_frames'
$rAgent = 'C:/Users/pc/capture_agent.ps1'

# --- Scene manifest: the single source of truth for the comparison set. ---
# Keep in sync with the table in the top-level README.md. Heading: degrees,
# 0 = north, CCW (90 = west, 270 = east); snaps the camera behind the
# player. Do not teleport into water (Tommy cannot swim).
$Manifest = @(
    @{ slug = 'rain';   tp = '250,-1283,12,0';   hour = 14; weather = 2; desc = 'Rain, Ocean Drive' }
    @{ slug = 'night';  tp = '250,-1283,12,90';  hour = 2;  weather = 0; desc = 'Night, Ocean View neon' }
    @{ slug = 'noon';   tp = '250,-1283,12,0';   hour = 12; weather = 0; desc = 'Noon, Ocean Drive' }
    @{ slug = 'sunset'; tp = '400,-1385,9,90';   hour = 19; weather = 0; desc = 'Golden sunset, beach to skyline' }
    @{ slug = 'dawn';   tp = '430,-1670,10,270'; hour = 7;  weather = 0; desc = 'Dawn, lighthouse channel' }
)

function Resolve-Ffmpeg {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $link = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ffmpeg.exe'
    if (Test-Path $link) { return $link }
    $pkg = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Gyan.FFmpeg*') -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pkg) { return $pkg.FullName }
    return $null
}
$ffmpeg = Resolve-Ffmpeg

if (-not (Test-Path $exe)) { throw "reVC.exe not found at $exe (build first)." }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$stage = Join-Path $env:TEMP 'rtgi_capture_stage'
New-Item -ItemType Directory -Force $stage | Out-Null

function Invoke-Remote([string]$psCommand) {
    # inner quotes must survive two argv layers (local CreateProcess + sshd)
    ssh $RemoteHost "powershell -NoProfile -Command \`"$psCommand\`""
}

# one-time per run: push exe + agent, (re)register the capture task
Invoke-Remote "Stop-Process -Name reVC -Force -EA SilentlyContinue; exit 0" | Out-Null
if (-not $SkipExe) {
    Write-Host 'pushing exe...'
    scp -q $exe "${RemoteHost}:$rGame/"
}
scp -q $agent "${RemoteHost}:$rAgent"
ssh $RemoteHost "schtasks /Create /TN rtgi-capture /TR `"powershell -ExecutionPolicy Bypass -File C:\Users\pc\capture_agent.ps1`" /SC ONCE /ST 00:00 /F" | Out-Null

function Invoke-RemoteJob([string[]]$jobLines, [int]$timeoutSec) {
    Set-Content -Path (Join-Path $stage 'capture_job.txt') -Value $jobLines -Encoding ascii
    scp -q (Join-Path $stage 'capture_job.txt') "${RemoteHost}:$rGame/capture_job.txt"
    Invoke-Remote "Remove-Item -Recurse -Force $rFrames -EA SilentlyContinue; exit 0" | Out-Null
    ssh $RemoteHost 'schtasks /Run /TN rtgi-capture' | Out-Null
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 8
        $done = Invoke-Remote "Test-Path $rFrames/capture_done.txt"
        if ("$done" -match 'True') {
            $marker = Invoke-Remote "Get-Content $rFrames/capture_done.txt"
            return ("$marker" -match 'result=ok')
        }
    }
    Write-Warning 'remote job timed out'
    Invoke-Remote 'Stop-Process -Name reVC -Force -EA SilentlyContinue; exit 0' | Out-Null
    return $false
}

function Save-Png($srcBmp, $dest) {
    $img = [System.Drawing.Image]::FromFile($srcBmp)
    try { $img.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $img.Dispose() }
}

function Invoke-CaptureStill($scene, [int]$enabled) {
    $kind = 'rtgi'; $mode = 'still'
    if ($enabled -eq 0) { $kind = 'raster'; $mode = 'raster' }
    Write-Host ("[{0}/{1}] {2}" -f $scene.slug, $kind, $scene.desc)
    $ok = Invoke-RemoteJob @(
        "mode=$mode", "tp=$($scene.tp)", "hour=$($scene.hour)", "weather=$($scene.weather)",
        "seconds=$Seconds", "shotframes=$ShotFrames"
    ) ($Seconds + 60)
    if (-not $ok) { Write-Warning ("[{0}/{1}] remote capture failed" -f $scene.slug, $kind); return }
    $local = Join-Path $stage 'still.bmp'
    Remove-Item $local -ErrorAction SilentlyContinue
    scp -q "${RemoteHost}:$rFrames/still.bmp" $local
    $dest = Join-Path $OutDir ("{0}_{1}.png" -f $scene.slug, $kind)
    Save-Png $local $dest
    Write-Host ("[{0}/{1}] -> {2}" -f $scene.slug, $kind, $dest)
}

function Invoke-CaptureGif($scene) {
    Write-Host ("[{0}/rtgi] {1} (gif)" -f $scene.slug, $scene.desc)
    $ok = Invoke-RemoteJob @(
        'mode=gif', "tp=$($scene.tp)", "hour=$($scene.hour)", "weather=$($scene.weather)",
        "gifshotframes=$GifShotFrames", "gifsettle=$GifSettle", "gifseconds=$GifSeconds",
        "gifmaxframes=$GifMaxFrames", "gifeveryn=$GifEveryN", "gifpollms=$GifPollMs"
    ) ($GifSettle + $GifSeconds + 150)
    if (-not $ok) { Write-Warning ("[{0}/rtgi] remote gif capture failed" -f $scene.slug); return }

    $fdir = Join-Path $stage ("frames_" + $scene.slug)
    if (Test-Path $fdir) { Remove-Item $fdir -Recurse -Force }
    New-Item -ItemType Directory -Path $fdir | Out-Null
    Write-Host ("[{0}/rtgi] pulling frames..." -f $scene.slug)
    scp -q "${RemoteHost}:$rFrames/frame_*.bmp" $fdir
    $kept = @(Get-ChildItem (Join-Path $fdir 'frame_*.bmp') -ErrorAction SilentlyContinue).Count
    if ($kept -lt 2) { Write-Warning ("[{0}/rtgi] too few frames pulled" -f $scene.slug); return }

    $dest = Join-Path $OutDir ("{0}_rtgi.gif" -f $scene.slug)
    Write-Host ("[{0}/rtgi] assembling {1} frames -> {2}" -f $scene.slug, $kept, $dest)
    if ($ffmpeg) {
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
    if (-not $GifOnly) { Invoke-CaptureStill $s 0 }
    if ($NoGif) { Invoke-CaptureStill $s 1 } else { Invoke-CaptureGif $s }
}

# session hygiene on the box: keep only rtgi_autoload.txt
Invoke-Remote "Remove-Item $rGame/rtgi_config.txt, $rGame/rtgi_window.txt, $rGame/rtgi.log, $rGame/capture_job.txt, $rGame/rtgi_shot_*.bmp -EA SilentlyContinue; Remove-Item -Recurse -Force $rFrames -EA SilentlyContinue; exit 0" | Out-Null

Write-Host ("capture complete: {0} scene(s) into {1}" -f $todo.Count, $OutDir)
