# Goal: continuously improve ray-traced lighting in reVC-RTGI

You are working unsupervised, indefinitely. Nobody will answer questions â€” decide and act. Improve the ray-traced lighting of this project forever: implement new lighting features, then revisit and raise the quality of existing ones, then repeat. **There is no "done".** When the backlog below is empty, invent the next one (there is always a surface that could be lit better) and keep going.

Prime directive: **the game must look and behave like original Vice City â€” only the lighting improves. With `enabled=0` (or a build without `--with-rtgi`) the game must stay pixel-identical vanilla.**

## Project map

Fork of re3 (`miami` = GTA Vice City decompile). The librw OpenGL 3.3 raster pipeline draws every frame; a Vulkan ray-query side-car (RTX 3090) computes AO, 2-bounce GI, sun shadows, emissive/point/spot lights, and reflections, composited via GL/VK interop. **Read `RTGI.md` first** â€” architecture, per-file map, every config key, tuned constants, open observations. `git log --oneline -30` shows the recent arc.

- Repo: `C:\Users\PC\Downloads\re3`, branch `rtgi`. Published at `github.com/gonzaloberteri/reVC-RTGI` (private; `origin`).
- RTGI code: `src/extras/rtgi/` + `src/extras/shaders/rtgi*`; all gated behind `#ifdef RTGI` + runtime toggles.
- `vendor/librw` is a **submodule** â†’ `github.com/gonzaloberteri/librw` branch `rtgi` (aap/librw + the `im3dOverrideShader` hook, archived at `docs/librw-rtgi.patch`). If you must change librw: commit inside the submodule, push its `rtgi` branch, then commit the gitlink bump in the main repo.
- Game dir (assets + save slot 1): `C:\Users\PC\Downloads\re3-game`. **Never touch the Steam VC install. Never commit game assets.**

## Build & deploy

```
cd C:\Users\PC\Downloads\re3          # MSBuild MUST run from repo root; a persisted cd breaks build\reVC.sln relative paths
./premake5.exe vs2019 --with-librw --with-rtgi     # only when premake5.lua or file lists change
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" build/reVC.sln -p:Configuration=Release "-p:Platform=win-amd64-librw_gl3_glfw-oal" -p:PlatformToolset=v143 -m -v:m -nologo
cp bin/win-amd64-librw_gl3_glfw-oal/Release/reVC.exe C:/Users/PC/Downloads/re3-game/
```

GL shaders: `cd src/extras/shaders && sh makeinc_glsl.sh <file>`. RT shaders: `cd src/extras/rtgi/shaders && GLSLANG="C:/VulkanSDK/1.4.350.0/Bin/glslangValidator.exe" sh make_spirv.sh`. Generated `.inc` are gitignored by pattern â€” `git add -f` them. Occasionally verify a vanilla premake (no `--with-rtgi`) still compiles.

## Autonomous in-game verification (after EVERY change â€” never commit unverified)

Harness files in the game dir (**always write them with absolute paths** â€” a stale config once made two "night" runs silently re-run noon):

1. `rtgi_autoload.txt` = `1` â†’ auto-loads save 1. Leave this file alone.
2. `rtgi_window.txt` = `1280 720` (or `1920 1080` for showcase, `2560 1440` for perf soak) â†’ windowed background run.
3. `rtgi_config.txt`, one `key=value` per line â€” full key list in RTGI.md. Teleports: `tp=x,y,z,heading` (deg, 0=N, CCW: 90=W, 270=E; snaps camera behind player). `weather=`: 0 sunny, 1 cloudy, 2 rainy, 3 foggy. **The config is hot-reloaded** (mtime polled every 30 frames): edit it mid-run to change toggles; `tp=` re-teleports on every reload; re-forcing the same weather is a no-op.
4. Launch minimized (`Start-Process -WindowStyle Minimized`), sleep ~90 s, kill `reVC`, read `rtgi.log`, convert `rtgi_shot_*.bmp` â†’ PNG, and **look at the images** (Read tool). Judge like a graphics programmer: silhouettes, light direction, noise, ghosting, brightness vs vanilla art direction. Judge lighting changes from **multiple camera angles** â€” a single angle once hid whole facades flooding the street pink.
5. Crash triage: early death â†’ `Get-WinEvent` Application log id 1000 for `reVC.exe`; `rtgi.log` shows how far init got.
6. Matrix (relevant subset per change): noon `tp=230,-1290,12 hour=12 weather=0`; night `hour=2`; rain `weather=2`; an `enabled=0` run to confirm vanilla untouched. Compare against `docs/baseline/*.png`; refresh baselines when a change intentionally improves the look.

### Harness traps (hard-won â€” respect these or lose hours)

- **Dense dumps at boot livelock the game.** `shotframes=2` from boot freezes the world at the teleport/weather tick (~160) while dumps keep rewriting one stale frame (timestamps advance, content frozen â€” deeply misleading). Stills: use `shotframes=250`. Dense capture: boot `shotframes=0`, arm via hot-reload after settle.
- **Iconified window + no dumps starves the VK fence** â†’ `frame fence wait failed â€” disabling ray tracing`. Sparse glReadPixels dumps are what keep minimized runs alive (each one drains GL). For dense capture the window must be **restored off-screen** â€” `docs/comparisons/capture.ps1` does this correctly (EnumWindows by PID + `IsWindowVisible` filter â€” GLFW owns hidden helper windows; `Process.MainWindowHandle` is flakily 0 â€” verify with `IsIconic`, retry).
- **Polling `rtgi_shot_*.bmp` with `Get-ChildItem` alone sees stale NTFS timestamps** (lazy dir-entry flush ~6 s). Dedup dump frames by content hash after opening each file.
- Shot slots rotate `rtgi_shot_0..3.bmp`; a killed run can leave one truncated (keep only max-size files).
- Tommy **cannot swim** â€” never teleport into water (hospital respawn ruins the run). Beach waterline â‰ˆ x=640â€“690.
- Minimized-run GPU timings are inflated (power state); compare like with like.
- PowerShell 5.1: nested arrays unroll through pipelines â€” use `[pscustomobject]`. ffmpeg lives under `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*`; `py` has Pillow.

## Working rules

- One improvement at a time: implement â†’ build â†’ verify in-game visually â†’ commit (imperative message: what + why) â†’ update `RTGI.md` (features, constants, observations) and this file's backlog.
- Every feature gets: a runtime toggle (debug menu in `rtgi.cpp AddDebugMenuEntries` + config key), a debug view if it produces a buffer, telemetry in `rtgi.log` if it has counts/timings.
- Never leave the tree broken. Failed approach â†’ revert, record under "attempted" in RTGI.md so it isn't retried blindly.
- Frame budget: if a feature costs >~2 ms at 1440p, add a quality knob with a sane default. Log per-pass timings.
- Every ~3 features: full regression sweep (matrix + `enabled=0` + vanilla-compile check).
- **Push `origin rtgi` after each verified, regression-green commit** (private repo â€” this is the backup). Never force-push, never change repo settings/visibility, never push assets.
- **Showcase upkeep:** after a visibly-improved feature, re-run `pwsh -File docs/comparisons/capture.ps1` (regenerates the README's raster PNGs + ray-traced GIFs deterministically; see `docs/comparisons/README.md`) and commit the refreshed set. The README table uses fixed-width HTML `<img width="435">` cells â€” keep that format.
- **Session hygiene:** end every work session by deleting `rtgi_window.txt`, `rtgi_config.txt`, `rtgi_shot_*`, `rtgi.log` from the game dir (keep only `rtgi_autoload.txt`).
- **Memory:** keep `rtgi-progress.md` in the auto-memory dir current (backlog position, new traps/lessons, absolute dates) â€” it is the only state that survives a fresh session.

## Current state (2026-07-20)

Original backlog #1â€“#9 all shipped (composites, emissive night models, headlights, real hit albedo, 2nd bounce, SVGF variance guiding, sea reflections, photo mode, checkerboard GI, temporal AO, BLAS compaction, resize/device-lost recreate, rain giblend easing, firefly fix via young-history Ã -trous bootstrap + radiance clamp 6.0). Config hot-reload and reflection-pass filtering (temporal clamp + spatial, `reflfilter=`) landed. GitHub published with A/B showcase. Details: RTGI.md + git log.

## Backlog (highest value first; when exhausted, invent more and re-polish)

1. **Photo-mode AO accumulation.** Photo mode true-averages GI but AO still shows grain in photo stills â€” accumulate AO the same way while the camera is still.
2. **Glass & window specular.** Vehicle windshields are non-opaque in the BLAS (dither in reflections) and building windows have no specular response. Give glass a proper reflective treatment (Fresnel-weighted RT reflection sample, deterministic â€” not stochastic â€” for smooth surfaces).
3. **Moon shadows + night polish.** Sun shadows exist; at night the moon casts nothing. Trace moon visibility at night hours, soft penumbra, subtle intensity. Re-judge night emissive balance after.
4. **Transient combat lights.** Explosions/gunfire already `AddLight` LIGHT_POINT â†’ likely already feed GI. Verify in combat at night (spawn a fight or scripted explosion via debug); if muzzle flashes/explosions don't visibly bounce, wire them into the GI light set with short decay.
5. **Interior light shafts.** Blocked on cataloguing interior floor coordinates per `area=` (hotel = save start, area 1; the malibu guess 489.6,-84.5 was wrong). Catalogue a few interiors first (debug-teleport + `area=N` sweep), then add volumetric shafts from windows/doors.
6. **GI probe fallback** for transparents/particles/water spray (they can't sample the screen-space GI buffers in their forward passes) â€” a small world-space probe grid updated from the RT results.
7. **Per-hour art-direction pass** (recurring): AO strength/radius vs hour, giblend vs timecycle at all hours, sun shadow softness vs art, water reflection floor (0.07) at dawn/dusk, wall wet sheen 0.25, road cap 0.75, paint base 0.35.
8. **Perf revisit** (recurring): rain reflection cost crept 0.83â†’1.23 ms; texture cache hit 975/1024 before the 2048 bump â€” consider LRU eviction; ped skinning cost; checkerboard quality at `checker=1`.
9. **Docs/architecture polish** (recurring): RTGI.md accuracy pass, `docs/comparisons` refresh, baseline refresh.

Then go deeper â€” volumetric light shafts through rain, lightning-flash GI, dashboard/interior car lights at night, tunnel/underpass light adaptation, streetlight cone volumetrics in fog â€” and then start the revisit cycle again with fresh eyes. **Never stop.**

