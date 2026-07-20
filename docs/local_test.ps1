# Local RTGI test harness: same interface as remote_test.ps1 but runs the
# game on THIS machine (for when the test box is in use by the user).
#
#   powershell -File docs/local_test.ps1 -Config "<key=value lines>" -Seconds 95 -OutDir <dir>
#
# Copies the fresh exe into the local game dir, writes the config files,
# launches windowed, waits, kills, converts shots to PNG. -SkipExe reuses
# the exe already in the game dir.

param(
    [Parameter(Mandatory=$true)][string]$Config,
    [int]$Seconds = 95,
    [string]$OutDir = "$PSScriptRoot\..\out\local",
    [string]$GameDir = "C:\Users\PC\Downloads\re3-game",
    [string]$Window = "1280 720",
    [switch]$SkipExe
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\.."
New-Item -ItemType Directory -Force $OutDir | Out-Null

Stop-Process -Name reVC -Force -ErrorAction SilentlyContinue
Remove-Item "$GameDir\rtgi_shot_*.bmp", "$GameDir\rtgi.log" -ErrorAction SilentlyContinue

if (-not $SkipExe) {
    $exeItem = Get-Item "$repo\bin\win-amd64-librw_gl3_glfw-oal\Release\reVC.exe"
    $ageMin = ((Get-Date) - $exeItem.LastWriteTime).TotalMinutes
    if ($ageMin -gt 20) { throw "reVC.exe is $([int]$ageMin) min old - did the build fail? (-SkipExe to override)" }
    Copy-Item $exeItem.FullName "$GameDir\reVC.exe" -Force
}
Set-Content -Encoding ascii "$GameDir\rtgi_config.txt" $Config
Set-Content -Encoding ascii "$GameDir\rtgi_window.txt" $Window

Write-Host "launching locally ($Seconds s)..."
$p = Start-Process -FilePath "$GameDir\reVC.exe" -WorkingDirectory $GameDir -PassThru
Start-Sleep -Seconds 15
if ($p.HasExited) { throw "reVC.exe exited early (code $($p.ExitCode)) - check $GameDir\rtgi.log" }
Start-Sleep -Seconds ([Math]::Max(0, $Seconds - 15))
Stop-Process -Name reVC -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Host "collecting results..."
Copy-Item "$GameDir\rtgi.log" $OutDir -ErrorAction SilentlyContinue
Get-ChildItem "$GameDir\rtgi_shot_*.bmp" -ErrorAction SilentlyContinue | Copy-Item -Destination $OutDir
Get-ChildItem "$OutDir\rtgi_shot_*.bmp" -ErrorAction SilentlyContinue |
    Group-Object Length | Sort-Object { [int]$_.Name } | Select-Object -Last 1 -ExpandProperty Group | ForEach-Object {
    $png = $_.FullName -replace '\.bmp$', '.png'
    py -c "from PIL import Image; Image.open(r'$($_.FullName)').save(r'$png')"
}
# session hygiene: never leave harness files behind in the game dir
Remove-Item "$GameDir\rtgi_config.txt", "$GameDir\rtgi_window.txt" -ErrorAction SilentlyContinue
Write-Host "done -> $OutDir"
