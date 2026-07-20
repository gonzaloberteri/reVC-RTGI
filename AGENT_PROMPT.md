# Goal: continuously improve ray-traced lighting in reVC-RTGI

You are working unsupervised, indefinitely. Nobody will answer questions — decide and act. Improve the ray-traced lighting of this project forever: implement new lighting features, then revisit and raise the quality of existing ones, then repeat. **There is no "done".** When the backlog below is empty, invent the next one (there is always a surface that could be lit better) and keep going.

Prime directive: **the game must look and behave like original Vice City — only the lighting improves. With `enabled=0` (or a build without `--with-rtgi`) the game must stay pixel-identical vanilla.**

## Project map

Fork of re3 (`miami` = GTA Vice City decompile). The librw OpenGL 3.3 raster pipeline draws every frame; a Vulkan ray-query side-car (RTX 3090) computes AO, 2-bounce GI, sun shadows, emissive/point/spot lights, and reflections, composited via GL/VK interop. **Read `RTGI.md` first** — architecture, per-file map, every config key, tuned constants, open observations. `git log --oneline -30` shows the recent arc.

- Repo: `C:\Users\PC\Downloads\re3`, branch `rtgi`. Published at `github.com/gonzaloberteri/reVC-RTGI` (private; `origin`).
- RTGI code: `src/extras/rtgi/` + `src/extras/shaders/rtgi*`; all gated behind `#ifdef RTGI` + runtime toggles.
- `vendor/librw` is a **submodule** → `github.com/gonzaloberteri/librw` branch `rtgi` (aap/librw + the `im3dOverrideShader` hook, archived at `docs/librw-rtgi.patch`). If you must change librw: commit inside the submodule, push its `rtgi` branch, then commit the gitlink bump in the main repo.
- Game dir (assets + save slot 1): `C:\Users\PC\Downloads\re3-game` locally; a copy lives on the **test box** at `192.168.0.209:C:\Users\pc\Downloads\re3-game`. **Never touch the Steam VC install. Never commit game assets.**
- **This PC is for development only** (edit/build/commit). **All game runs happen on the test box** (RTX 3090, user `pc`, key-based SSH + VNC) so the person using this PC is never interrupted — see verification below.

## Build & deploy

```
cd C:\Users\PC\Downloads\re3          # MSBuild MUST run from repo root; a persisted cd breaks build\reVC.sln relative paths
./premake5.exe vs2019 --with-librw --with-rtgi     # only when premake5.lua or file lists change
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" build/reVC.sln -p:Configuration=Release "-p:Platform=win-amd64-librw_gl3_glfw-oal" -p:PlatformToolset=v143 -m -v:m -nologo
```

Deployment is part of the remote test run (`docs/remote_test.ps1` pushes the fresh exe); don't copy the exe into the local game dir.

GL shaders: `cd src/extras/shaders && sh makeinc_glsl.sh <file>`. RT shaders: `cd src/extras/rtgi/shaders && GLSLANG="C:/VulkanSDK/1.4.350.0/Bin/glslangValidator.exe" sh make_spirv.sh`. Generated `.inc` are gitignored by pattern — `git add -f` them. Occasionally verify a vanilla premake (no `--with-rtgi`) still compiles.

## Autonomous in-game verification (after EVERY change — never commit unverified)

**All runs happen on the test box `192.168.0.209`** — use `powershell -File docs/remote_test.ps1 -Config "<key=value lines>" -Seconds 95 -OutDir <dir>` (no `pwsh` on the dev PC). It pushes the fresh exe + config, triggers the `rtgi-run` scheduled task (launches `C:\Users\pc\rtgi_launch.cmd` minimized **in the remote console session** — SSH-spawned GUI processes get no display/GPU), waits, kills `reVC`, and pulls back `rtgi.log` + shots as PNG. Then **look at the images** (Read tool). Judge like a graphics programmer: silhouettes, light direction, noise, ghosting, brightness vs vanilla art direction. Judge lighting changes from **multiple camera angles** — a single angle once hid whole facades flooding the street pink.

Remote box facts: user `pc`, key-based SSH, default remote shell is **PowerShell 5.1** (no `&&`; scp staged files instead of fighting nested quoting), RTX 3090 (+ an Intel iGPU — GL correctly lands on NVIDIA in the console session), VNC available if a live look is ever needed. Display is **4K@60** — pass `-Window "3840 2160"` when a shot needs real detail (glass close-ups, showcase); keep 720p for routine runs (shots scp back 9× smaller).

Config reference (files land in the remote game dir):

1. `rtgi_autoload.txt` = `1` → auto-loads save 1. Already on the box; leave it alone.
2. `rtgi_window.txt` = `1280 720` (or `1920 1080` for showcase, `2560 1440` for perf soak) → windowed background run (remote_test.ps1 `-Window`).
3. `rtgi_config.txt`, one `key=value` per line — full key list in RTGI.md. Teleports: `tp=x,y,z,heading` (deg, 0=N, CCW: 90=W, 270=E; snaps camera behind player). `weather=`: 0 sunny, 1 cloudy, 2 rainy, 3 foggy. **The config is hot-reloaded** (mtime polled every 30 frames): scp a new one mid-run to change toggles; `tp=` re-teleports on every reload; re-forcing the same weather is a no-op.
4. Crash triage: early death → `Get-WinEvent` Application log id 1000 for `reVC.exe` **on the remote box** (via ssh); `rtgi.log` shows how far init got.
5. Matrix (relevant subset per change): noon `tp=230,-1290,12 hour=12 weather=0`; night `hour=2`; rain `weather=2`; an `enabled=0` run to confirm vanilla untouched. Compare against `docs/baseline/*.png`; refresh baselines when a change intentionally improves the look.

### Harness traps (hard-won — respect these or lose hours)

- **Dense dumps at boot livelock the game.** `shotframes=2` from boot freezes the world at the teleport/weather tick (~160) while dumps keep rewriting one stale frame (timestamps advance, content frozen — deeply misleading). Stills: use `shotframes=250`. Dense capture: boot `shotframes=0`, arm via hot-reload after settle.
- **Iconified window + no dumps starves the VK fence** → `frame fence wait failed — disabling ray tracing`. Sparse glReadPixels dumps are what keep minimized runs alive (each one drains GL). For dense capture the window must be **restored off-screen** — `docs/comparisons/capture.ps1` does this correctly (EnumWindows by PID + `IsWindowVisible` filter — GLFW owns hidden helper windows; `Process.MainWindowHandle` is flakily 0 — verify with `IsIconic`, retry).
- **Polling `rtgi_shot_*.bmp` with `Get-ChildItem` alone sees stale NTFS timestamps** (lazy dir-entry flush ~6 s). Dedup dump frames by content hash after opening each file.
- Shot slots rotate `rtgi_shot_0..3.bmp`; a killed run can leave one truncated (keep only max-size files).
- Tommy **cannot swim** — never teleport into water (hospital respawn ruins the run). Beach waterline ≈ x=640–690.
- Minimized-run GPU timings are inflated (power state); compare like with like.
- PowerShell 5.1: nested arrays unroll through pipelines — use `[pscustomobject]`. ffmpeg lives under `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*`; `py` has Pillow.

## Working rules

- One improvement at a time: implement → build → verify in-game visually → commit (imperative message: what + why) → update `RTGI.md` (features, constants, observations) and this file's backlog.
- Every feature gets: a runtime toggle (debug menu in `rtgi.cpp AddDebugMenuEntries` + config key), a debug view if it produces a buffer, telemetry in `rtgi.log` if it has counts/timings.
- Never leave the tree broken. Failed approach → revert, record under "attempted" in RTGI.md so it isn't retried blindly.
- Frame budget: if a feature costs >~2 ms at 1440p, add a quality knob with a sane default. Log per-pass timings.
- Every ~3 features: full regression sweep (matrix + `enabled=0` + vanilla-compile check).
- **Push `origin rtgi` after each verified, regression-green commit** (private repo — this is the backup). Never force-push, never change repo settings/visibility, never push assets.
- **Showcase upkeep:** after a visibly-improved feature, re-run `pwsh -File docs/comparisons/capture.ps1` (regenerates the README's raster PNGs + ray-traced GIFs deterministically; see `docs/comparisons/README.md`) and commit the refreshed set. The README table uses fixed-width HTML `<img width="435">` cells — keep that format. NOTE: capture.ps1 still runs the game **locally** (off-screen dense capture) — it has not been ported to the test box yet; porting it is on the backlog. Until then only run it when a showcase refresh is genuinely due.
- **Session hygiene:** end every work session by deleting `rtgi_window.txt`, `rtgi_config.txt`, `rtgi_shot_*`, `rtgi.log` from the **remote** game dir (keep only `rtgi_autoload.txt`), and never leave `reVC.exe` running there.
- **Memory:** keep `rtgi-progress.md` in the auto-memory dir current (backlog position, new traps/lessons, absolute dates) — it is the only state that survives a fresh session.

## Current state (2026-07-20)

Original backlog #1–#9 all shipped (composites, emissive night models, headlights, real hit albedo, 2nd bounce, SVGF variance guiding, sea reflections, photo mode, checkerboard GI, temporal AO, BLAS compaction, resize/device-lost recreate, rain giblend easing, firefly fix via young-history à-trous bootstrap + radiance clamp 6.0). Config hot-reload and reflection-pass filtering (temporal clamp + spatial, `reflfilter=`) landed. GitHub published with A/B showcase. Details: RTGI.md + git log.

## Backlog (highest value first; when exhausted, invent more and re-polish)

1. **Water z-order issues (USER-REPORTED, remaining piece).** Near-water texture pop FIXED (wavy/mask atomics now draw through the RTGI water shader) and caustic shimmer SHIPPED (watercaustics=, distance-faded). The reported z-index issues have not been reproduced yet - hunt them: docks/boats/jetties, underwater geometry through the surface, shore decals. Capture at piers with boats, and while airborne over water.
2. **Glass follow-ups.** Translucent world panes verified at Howlin' Petes biker emporium (5 marked meshes, Downtown -600,680). Still to check: mall interior (AREA_MALL=4 - interior floor coords uncatalogued; probe (420,1080) landed in unstreamed LOD land), downtown office glass, Biker bar interior (AREA_BIKER_BAR=11). Consider BLAS-side: reflection rays hitting glass of OTHER buildings still see stochastic 45% dither.
3. **Cutscene debug harness (USER SUGGESTION).** Add cutscene=NAME config key: load+play a mission cutscene via CCutsceneMgr after settle - fixed camera paths give far more consistent A/B captures than teleport stills. Wire into capture.ps1 once ported.
4. **Night polish after moon shadows.** Moon shadows SHIPPED (fixed south direction, sprite brightness ramp x0.30, moonshadows=). Re-judge night emissive balance with them on; consider whether streetlight pools need shadowing too.
5. **Transient combat lights.** Explosions/gunfire already `AddLight` LIGHT_POINT → likely already feed GI. Verify in combat at night (spawn a fight or scripted explosion via debug); if muzzle flashes/explosions don't visibly bounce, wire them into the GI light set with short decay.
6. **Interior light shafts.** Blocked on cataloguing interior floor coordinates per `area=` (hotel = save start, area 1; the malibu guess 489.6,-84.5 was wrong). Catalogue a few interiors first (debug-teleport + `area=N` sweep), then add volumetric shafts from windows/doors.
7. **GI probe fallback** for transparents/particles/water spray (they can't sample the screen-space GI buffers in their forward passes) — a small world-space probe grid updated from the RT results.
8. **Per-hour art-direction pass** (recurring): AO strength/radius vs hour, giblend vs timecycle at all hours, sun shadow softness vs art, water reflection floor (0.07) at dawn/dusk, wall wet sheen 0.25, road cap 0.75, paint base 0.35.
9. **Perf revisit** (recurring): rain reflection cost crept 0.83→1.23 ms; texture cache hit 975/1024 before the 2048 bump — consider LRU eviction; ped skinning cost; checkerboard quality at `checker=1`.
10. **Docs/architecture polish** (recurring): RTGI.md accuracy pass, `docs/comparisons` refresh, baseline refresh.
11. **Port `docs/comparisons/capture.ps1` to the test box.** It still launches the game locally (needs its restored-off-screen window trick for dense GIF capture). Move the run to 192.168.0.209 like `docs/remote_test.ps1` — the off-screen restore + hot-reload arming logic must run remotely (scp a helper ps1, run it via the scheduled task or a second schtasks entry).

Then go deeper — volumetric light shafts through rain, lightning-flash GI, dashboard/interior car lights at night, tunnel/underpass light adaptation, streetlight cone volumetrics in fog — and then start the revisit cycle again with fresh eyes. **Never stop.**

