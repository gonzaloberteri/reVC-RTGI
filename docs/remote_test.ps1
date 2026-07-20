# Remote RTGI test harness: runs a build on the test box (192.168.0.209)
# so automated verification never touches the dev PC's desktop.
#
#   powershell -File docs/remote_test.ps1 -Config @"
#   tp=230,-1290,12,180
#   hour=12
#   weather=0
#   shotframes=250
#   "@ -Seconds 95 -OutDir C:\path\for\results
#
# Pushes the freshly built exe + config, triggers the "rtgi-run" scheduled
# task (runs in the remote console session so GL gets the RTX 3090 and a
# real display), waits, kills the game, and pulls back rtgi.log + shots
# converted to PNG. -SkipExe reuses the exe already on the box.
#
# One-time remote setup (already done): game assets at
# C:\Users\pc\Downloads\re3-game, launcher C:\Users\pc\rtgi_launch.cmd,
# schtasks rtgi-run pointing at it, key-based SSH for user pc.

param(
    [Parameter(Mandatory=$true)][string]$Config,
    [int]$Seconds = 95,
    [string]$OutDir = "$PSScriptRoot\..\out\remote",
    [string]$RemoteHost = "192.168.0.209",
    [string]$Window = "1280 720",
    [switch]$SkipExe
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\.."
$gameDir = "C:/Users/pc/Downloads/re3-game"
New-Item -ItemType Directory -Force $OutDir | Out-Null

# stage config files locally, then push (remote default shell is PS 5.1 —
# scp files instead of fighting nested ssh quoting)
$stage = Join-Path $env:TEMP "rtgi_remote_stage"
New-Item -ItemType Directory -Force $stage | Out-Null
Set-Content -Encoding ascii "$stage\rtgi_config.txt" $Config
Set-Content -Encoding ascii "$stage\rtgi_window.txt" $Window

# remote default shell is PowerShell 5.1. Inner quotes must survive TWO
# argv layers (local CreateProcess + remote sshd) — backslash-escape them.
$gd = $gameDir -replace '/', '\'
$clean = "Stop-Process -Name reVC -Force -EA SilentlyContinue; " +
    "Remove-Item $gd\rtgi_shot_*.bmp, $gd\rtgi.log -EA SilentlyContinue; exit 0"
ssh $RemoteHost "powershell -NoProfile -Command \`"$clean\`"" | Out-Null
if (-not $SkipExe) {
    Write-Host "pushing exe..."
    scp -q "$repo\bin\win-amd64-librw_gl3_glfw-oal\Release\reVC.exe" "${RemoteHost}:$gameDir/"
}
scp -q "$stage\rtgi_config.txt" "$stage\rtgi_window.txt" "${RemoteHost}:$gameDir/"

Write-Host "launching (rtgi-run task, $Seconds s)..."
ssh $RemoteHost "schtasks /Run /TN rtgi-run" | Out-Null
Start-Sleep -Seconds 15
$up = ssh $RemoteHost 'tasklist /FI \"IMAGENAME eq reVC.exe\" /NH'
if ("$up" -notmatch "reVC") { throw "reVC.exe did not start on $RemoteHost" }
Start-Sleep -Seconds ([Math]::Max(0, $Seconds - 15))
ssh $RemoteHost "taskkill /IM reVC.exe /F" | Out-Null
Start-Sleep -Seconds 3

Write-Host "fetching results..."
scp -q "${RemoteHost}:$gameDir/rtgi.log" $OutDir
# shots may not exist (shotframes=0 runs); tolerate the nonzero exit.
# no 2>$null here: PS5.1 wraps redirected native stderr in ErrorRecords
# which $ErrorActionPreference=Stop turns into a script abort
scp -q "${RemoteHost}:$gameDir/rtgi_shot_*.bmp" $OutDir
Get-ChildItem "$OutDir\rtgi_shot_*.bmp" -ErrorAction SilentlyContinue | ForEach-Object {
    # killed runs can truncate a rotating slot — skip non-max-size files
    $_
} | Group-Object Length | Sort-Object { [int]$_.Name } | Select-Object -Last 1 -ExpandProperty Group | ForEach-Object {
    $png = $_.FullName -replace '\.bmp$', '.png'
    py -c "from PIL import Image; Image.open(r'$($_.FullName)').save(r'$png')"
}
Write-Host "done -> $OutDir"
