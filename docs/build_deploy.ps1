# Canonical RTGI build: MSBuild + deterministic deploy to the test box.
#
#   powershell -File docs/build_deploy.ps1              # build + push exe, verify hash
#   powershell -File docs/build_deploy.ps1 -NoDeploy    # build only (offline work)
#   powershell -File docs/build_deploy.ps1 -SyncAssets  # additionally sync changed game-dir assets
#
# This is THE build command. It guarantees the box at 192.168.0.209 always
# runs the exe that was just built: after scp it re-hashes the remote file
# and fails loudly on any mismatch (no more stale-exe verification runs).
# remote_test.ps1 still pushes the exe itself before a run; this script is
# what makes "build" and "deploy" one atomic, deterministic step.

param(
    [switch]$NoDeploy,
    [switch]$SyncAssets,
    [switch]$SkipBuild,          # deploy-only (exe must be <20 min old)
    [string]$RemoteHost = "192.168.0.209",
    [string]$RemoteGameDir = "C:/Users/pc/Downloads/re3-game",
    [string]$LocalGameDir = "C:\Users\PC\Downloads\re3-game"
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\.."
$exePath = "$repo\bin\win-amd64-librw_gl3_glfw-oal\Release\reVC.exe"

if (-not $SkipBuild) {
    $buildStart = Get-Date
    # MSBuild must run from the repo root (build\reVC.sln uses relative paths)
    Push-Location $repo
    try {
        & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
            build/reVC.sln -p:Configuration=Release `
            "-p:Platform=win-amd64-librw_gl3_glfw-oal" -p:PlatformToolset=v143 `
            -m -v:m -nologo
        if ($LASTEXITCODE -ne 0) { throw "MSBuild failed (exit $LASTEXITCODE) - nothing deployed" }
    } finally { Pop-Location }
    $exeItem = Get-Item $exePath
    if ($exeItem.LastWriteTime -lt $buildStart) {
        throw "reVC.exe was not rewritten by the build (up-to-date is fine, but timestamp predates this script start by more than the build) - refusing to deploy a possibly stale exe"
    }
} else {
    $exeItem = Get-Item $exePath
    $ageMin = ((Get-Date) - $exeItem.LastWriteTime).TotalMinutes
    if ($ageMin -gt 20) { throw "-SkipBuild: reVC.exe is $([int]$ageMin) min old - build first" }
}

if ($NoDeploy) { Write-Host "build OK (deploy skipped)"; exit 0 }

# --- deploy exe, verify by hash (the deterministic part) ---
$localHash = (Get-FileHash $exePath -Algorithm MD5).Hash
Write-Host "pushing reVC.exe ($([math]::Round($exeItem.Length/1MB,1)) MB, md5 $localHash)..."
# the game may be running from the exe (locked); stop it first
ssh $RemoteHost "powershell -NoProfile -Command \`"Stop-Process -Name reVC -Force -EA SilentlyContinue; exit 0\`"" | Out-Null
scp -q $exePath "${RemoteHost}:$RemoteGameDir/"
$remoteHash = ssh $RemoteHost "powershell -NoProfile -Command \`"(Get-FileHash '$($RemoteGameDir -replace '/', '\')\reVC.exe' -Algorithm MD5).Hash\`""
if ("$remoteHash".Trim() -ne $localHash) {
    throw "DEPLOY HASH MISMATCH: local $localHash vs remote $($remoteHash) - exe on the box is NOT this build"
}
Write-Host "deploy verified (remote md5 matches)"

# --- optional asset sync: push game-dir files whose size differs remotely ---
if ($SyncAssets) {
    Write-Host "comparing game-dir assets (name+size)..."
    # runtime artifacts never sync; saves are per-box
    $exclude = '^(reVC\.exe|rtgi_shot_.*|rtgi\.log|rtgi_config\.txt|rtgi_window\.txt|rtgi_autoload\.txt|imgui\.ini|reVC\.ini)$'
    $remoteList = ssh $RemoteHost "powershell -NoProfile -Command \`"Get-ChildItem -Recurse -File '$($RemoteGameDir -replace '/', '\')' | ForEach-Object { \`$_.FullName.Substring('$($RemoteGameDir -replace '/', '\')'.Length+1) + '|' + \`$_.Length }\`""
    $remote = @{}
    foreach ($line in $remoteList) {
        $p = $line.LastIndexOf('|')
        if ($p -gt 0) { $remote[$line.Substring(0, $p)] = [long]$line.Substring($p + 1) }
    }
    $pushed = 0
    Get-ChildItem -Recurse -File $LocalGameDir | ForEach-Object {
        $rel = $_.FullName.Substring($LocalGameDir.Length + 1)
        if ($rel -match $exclude) { return }
        if (-not $remote.ContainsKey($rel) -or $remote[$rel] -ne $_.Length) {
            $dest = "$RemoteGameDir/$($rel -replace '\\', '/')"
            Write-Host "  sync: $rel"
            $destDir = ($dest -replace '/[^/]+$', '') -replace '/', '\'
            ssh $RemoteHost "powershell -NoProfile -Command \`"New-Item -ItemType Directory -Force '$destDir' | Out-Null; exit 0\`"" | Out-Null
            scp -q $_.FullName "${RemoteHost}:$dest"
            $pushed++
        }
    }
    Write-Host "asset sync done ($pushed file(s) pushed)"
}
Write-Host "done"
