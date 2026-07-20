# Remote capture agent for reVC-RTGI comparison captures.
#
# Runs ON the test box (console session, via the "rtgi-capture" scheduled
# task) so the game gets a real display/GPU. One invocation executes ONE
# job described by capture_job.txt in the game dir, writes its outputs to
# $FramesDir, then drops capture_done.txt and exits. The dev-PC
# orchestrator (docs/comparisons/capture.ps1) pushes the job file,
# triggers the task, polls for the marker over ssh and pulls the frames.
#
# Job file (key=value per line):
#   mode=raster|still|gif      raster = enabled=0 still; still = RTGI still
#   tp=x,y,z,heading  hour=N  weather=N
#   seconds=90  shotframes=250            (stills)
#   gifshotframes=2 gifsettle=8 gifseconds=25 gifmaxframes=54 gifeveryn=2
#   gifpollms=70                          (gif collection)
#   window=1920 1080
#
# PS 5.1 compatible. Self-contained: duplicates the window-restore and
# frame-dedup logic that used to live in capture.ps1 (the game-side half).

$ErrorActionPreference = 'Stop'

$GameDir = 'C:\Users\pc\Downloads\re3-game'
$FramesDir = 'C:\Users\pc\rtgi_frames'
$JobFile = Join-Path $GameDir 'capture_job.txt'
$DoneFile = Join-Path $FramesDir 'capture_done.txt'

# While ICONIFIED the GL driver defers presents and the VK frame fence
# starves ("frame fence wait failed"). For dense capture the window must be
# RESTORED but off-screen: set the restore position off-screen while still
# minimized, then show without activating.
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
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $procIds = @(Get-Process reVC -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id)
        if ($procIds.Count) {
            $state = @{ found = [System.IntPtr]::Zero }
            $cb = [RtgiCap.Win32+EnumProc]{
                param($h, $l)
                $wpid = [uint32]0
                [void][RtgiCap.Win32]::GetWindowThreadProcessId($h, [ref]$wpid)
                # GLFW owns hidden helper windows; the real one is WS_VISIBLE
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
            [void][RtgiCap.Win32]::SetWindowPos($h, [System.IntPtr]1, 0, 0, 0, 0, 0x0413)
            Start-Sleep -Milliseconds 300
            if (-not [RtgiCap.Win32]::IsIconic($h)) { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Stop-Game {
    Get-Process reVC -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Get-NewestCompleteShot {
    $shots = Get-ChildItem (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
    if (-not $shots) { return $null }
    $full = ($shots | Measure-Object -Property Length -Maximum).Maximum
    $shots | Where-Object { $_.Length -eq $full } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Wait-ForInit([int]$timeoutSec = 70) {
    $log = Join-Path $GameDir 'rtgi.log'
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'forced weather' -Quiet)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# ---- read job -------------------------------------------------------------

# transcript for post-mortem (the orchestrator only sees the done marker)
try { Start-Transcript -Path 'C:\Users\pc\capture_agent.log' -Force | Out-Null } catch {}

if (Test-Path $FramesDir) { Remove-Item $FramesDir -Recurse -Force }
New-Item -ItemType Directory -Path $FramesDir | Out-Null

$result = 'fail'
$detail = ''
try {
    $job = @{}
    foreach ($line in (Get-Content $JobFile)) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { $job[$line.Substring(0, $i)] = $line.Substring($i + 1) }
    }
    Write-Host ('job mode=' + $job['mode'])
    $mode = $job['mode']
    $seconds = 90; if ($job['seconds']) { $seconds = [int]$job['seconds'] }

    Stop-Game
    Remove-Item (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $GameDir 'rtgi.log') -ErrorAction SilentlyContinue

    $window = $job['window']; if (-not $window) { $window = '1920 1080' }
    Set-Content -Path (Join-Path $GameDir 'rtgi_autoload.txt') -Value '1' -Encoding ascii
    Set-Content -Path (Join-Path $GameDir 'rtgi_window.txt') -Value $window -Encoding ascii

    $sceneKeys = @("tp=$($job['tp'])", "hour=$($job['hour'])", "weather=$($job['weather'])")
    if ($job['extra']) { $sceneKeys += ($job['extra'] -split ';') }

    if ($mode -eq 'raster' -or $mode -eq 'still') {
        $en = 1; if ($mode -eq 'raster') { $en = 0 }
        $sf = 250; if ($job['shotframes']) { $sf = [int]$job['shotframes'] }
        Set-Content -Path (Join-Path $GameDir 'rtgi_config.txt') -Value (@("enabled=$en") + $sceneKeys + @("shotframes=$sf")) -Encoding ascii
        Write-Host 'launching game (still)'
        Start-Process -FilePath (Join-Path $GameDir 'reVC.exe') -WorkingDirectory $GameDir -WindowStyle Minimized
        Write-Host 'launched; sleeping'
        Start-Sleep -Seconds $seconds
        Stop-Game
        Start-Sleep -Seconds 2
        $shot = Get-NewestCompleteShot
        if ($shot) {
            Copy-Item $shot.FullName (Join-Path $FramesDir 'still.bmp') -Force
            $result = 'ok'; $detail = $shot.Name
        } else { $detail = 'no screenshot produced' }
    }
    elseif ($mode -eq 'gif') {
        $gifShotFrames = 2; if ($job['gifshotframes']) { $gifShotFrames = [int]$job['gifshotframes'] }
        $gifSettle = 8; if ($job['gifsettle']) { $gifSettle = [int]$job['gifsettle'] }
        $gifSeconds = 25; if ($job['gifseconds']) { $gifSeconds = [int]$job['gifseconds'] }
        $gifMaxFrames = 54; if ($job['gifmaxframes']) { $gifMaxFrames = [int]$job['gifmaxframes'] }
        $gifEveryN = 2; if ($job['gifeveryn']) { $gifEveryN = [int]$job['gifeveryn'] }
        $gifPollMs = 70; if ($job['gifpollms']) { $gifPollMs = [int]$job['gifpollms'] }

        # boot with shotframes=0 (dense dumps at boot livelock the game);
        # arm dumping via config hot-reload once the scene settles
        Set-Content -Path (Join-Path $GameDir 'rtgi_config.txt') -Value (@('enabled=1') + $sceneKeys + @('shotframes=0')) -Encoding ascii
        Start-Process -FilePath (Join-Path $GameDir 'reVC.exe') -WorkingDirectory $GameDir -WindowStyle Minimized
        [void](Move-GameWindowOffscreenRestored)
        [void](Wait-ForInit)
        Start-Sleep -Seconds $gifSettle
        $armKeys = @('enabled=1', "hour=$($job['hour'])", "weather=$($job['weather'])")
        if ($job['extra']) { $armKeys += ($job['extra'] -split ';') }
        Set-Content -Path (Join-Path $GameDir 'rtgi_config.txt') -Value ($armKeys + @("shotframes=$gifShotFrames")) -Encoding ascii

        $md5 = [System.Security.Cryptography.MD5]::Create()
        $seen = New-Object 'System.Collections.Generic.HashSet[string]'
        $kept = 0; $distinct = 0; $fullLen = 0L
        $buf = New-Object byte[] 4096
        $end = (Get-Date).AddSeconds($gifSeconds + $gifSettle)
        while ((Get-Date) -lt $end -and $kept -lt $gifMaxFrames) {
            $batch = New-Object System.Collections.Generic.List[object]
            foreach ($f in (Get-ChildItem (Join-Path $GameDir 'rtgi_shot_*.bmp') -ErrorAction SilentlyContinue)) {
                try {
                    $fs = [System.IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
                    try {
                        $len = $fs.Length
                        if ($len -gt $fullLen) { $fullLen = $len }
                        if ($len -ne $fullLen -or $len -lt 4096) { continue }
                        $null = $fs.Seek($len - 4096, 'Begin')
                        $null = $fs.Read($buf, 0, 4096)
                        $hash = [System.BitConverter]::ToString($md5.ComputeHash($buf))
                    } finally { $fs.Close() }
                    if ($seen.Add($hash)) {
                        $batch.Add([pscustomobject]@{ Time = (Get-Item $f.FullName).LastWriteTime; Path = $f.FullName })
                    }
                } catch { }
            }
            foreach ($entry in ($batch | Sort-Object Time)) {
                $distinct++
                if ($entry.Path -and ($distinct % $gifEveryN) -eq 0 -and $kept -lt $gifMaxFrames) {
                    $kept++
                    Copy-Item $entry.Path (Join-Path $FramesDir ('frame_{0:D4}.bmp' -f $kept)) -Force -ErrorAction SilentlyContinue
                }
            }
            Start-Sleep -Milliseconds $gifPollMs
        }
        Stop-Game
        # drop torn frames, renumber gapless
        $frames = Get-ChildItem (Join-Path $FramesDir 'frame_*.bmp') -ErrorAction SilentlyContinue
        if ($frames) {
            $good = ($frames | Measure-Object -Property Length -Maximum).Maximum
            $frames | Where-Object { $_.Length -ne $good } | Remove-Item -Force
            $i = 0
            Get-ChildItem (Join-Path $FramesDir 'frame_*.bmp') | Sort-Object Name | ForEach-Object {
                $i++
                $want = 'frame_{0:D4}.bmp' -f $i
                if ($_.Name -ne $want) { Rename-Item $_.FullName $want }
            }
            $kept = $i
        }
        if ($kept -ge 2) { $result = 'ok' }
        $detail = "$distinct distinct, $kept kept"
    }
    else { $detail = "unknown mode '$mode'" }
} catch {
    $detail = $_.Exception.Message
} finally {
    Stop-Game
    if (-not (Test-Path $FramesDir)) { New-Item -ItemType Directory -Path $FramesDir | Out-Null }
    Set-Content -Path $DoneFile -Value @("result=$result", "detail=$detail") -Encoding ascii
    try { Stop-Transcript | Out-Null } catch {}
}
