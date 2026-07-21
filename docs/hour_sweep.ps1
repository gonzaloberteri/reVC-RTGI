# One game launch, config hot-reloaded through a list of hours; the newest
# screenshot after each settle is stashed per hour. Local dev-PC use.
param(
    [string]$GameDir = "C:\Users\PC\Downloads\re3-game",
    [string]$OutDir,
    [string]$BaseConfig = "tp=230,-1290,12,180`nweather=0",
    [int[]]$Hours = @(6, 9, 12, 15, 18, 21, 0, 3),
    [int]$SettleSeconds = 22,
    [switch]$Vanilla
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null

Stop-Process -Name reVC -Force -ErrorAction SilentlyContinue
Remove-Item "$GameDir\rtgi_shot_*.bmp", "$GameDir\rtgi.log" -ErrorAction SilentlyContinue
$enable = if ($Vanilla) { "enabled=0" } else { "enabled=1" }
Set-Content -Encoding ascii "$GameDir\rtgi_config.txt" "$BaseConfig`n$enable`nhour=$($Hours[0])`nshotframes=0"
Set-Content -Encoding ascii "$GameDir\rtgi_window.txt" "1920 1080"

$p = Start-Process -FilePath "$GameDir\reVC.exe" -WorkingDirectory $GameDir -PassThru
Start-Sleep -Seconds 25   # boot + load + first teleport
if ($p.HasExited) { throw "reVC exited early (code $($p.ExitCode))" }

foreach ($h in $Hours) {
    # arm the hour, let weather/lighting settle without dumps, then dump
    Set-Content -Encoding ascii "$GameDir\rtgi_config.txt" "$BaseConfig`n$enable`nhour=$h`nshotframes=0"
    Start-Sleep -Seconds ($SettleSeconds - 8)
    Set-Content -Encoding ascii "$GameDir\rtgi_config.txt" "$BaseConfig`n$enable`nhour=$h`nshotframes=150"
    Start-Sleep -Seconds 8
    # newest dump = this hour (open files to defeat stale NTFS timestamps)
    $newest = Get-ChildItem "$GameDir\rtgi_shot_*.bmp" -ErrorAction SilentlyContinue |
        ForEach-Object { $s = [IO.File]::OpenRead($_.FullName); $s.Close(); $_ } |
        Sort-Object LastWriteTime | Select-Object -Last 1
    if ($newest) {
        $png = Join-Path $OutDir ("hour_{0:d2}.png" -f $h)
        py -c "from PIL import Image; Image.open(r'$($newest.FullName)').save(r'$png')"
        Write-Host "hour $h -> $png"
    } else {
        Write-Host "hour $h -> NO SHOT"
    }
}

Stop-Process -Name reVC -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Copy-Item "$GameDir\rtgi.log" $OutDir -ErrorAction SilentlyContinue
Remove-Item "$GameDir\rtgi_config.txt", "$GameDir\rtgi_window.txt", "$GameDir\rtgi_shot_*.bmp" -ErrorAction SilentlyContinue
Write-Host "sweep done -> $OutDir"
