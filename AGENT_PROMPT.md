# Goal: continuously improve ray-traced lighting in the reVC RTGI fork

You are working unsupervised, indefinitely. Nobody will answer questions — decide and act. Your job is to improve the ray-traced lighting of this project forever: implement new lighting features, then revisit and raise the quality of existing ones, then repeat. There is no "done".

## Project

Fork of re3 (`miami` branch = GTA Vice City decompilation) with ray-traced global illumination added via a hybrid renderer: the original librw OpenGL 3.3 raster pipeline draws the frame; a Vulkan ray-tracing side-car (RTX 3090) computes AO, sun shadows, 1-bounce GI, point lights, and reflections, shared with GL via `GL_EXT_memory_object_win32` interop. Read `RTGI.md` in the repo root first — it documents the architecture, every file, and every config key. The prime directive of the whole project: **the game must look and behave like original Vice City, only the lighting improves; with RTGI off the game must be pixel-identical vanilla.**

- Repo: `C:\Users\PC\Downloads\re3` (branch `rtgi`, commit locally, do NOT push unless the user shows up and asks)
- RTGI code: `src/extras/rtgi/` (+`src/extras/shaders/rtgi*` for GL shaders); everything gated by `#ifdef RTGI` + runtime toggles
- Run dir with game assets + save in slot 1: `C:\Users\PC\Downloads\re3-game` (never touch the Steam VC install)

## Build

```
cd C:\Users\PC\Downloads\re3
./premake5.exe vs2019 --with-librw --with-rtgi     # only when premake5.lua or file lists change
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" build/reVC.sln -p:Configuration=Release "-p:Platform=win-amd64-librw_gl3_glfw-oal" -p:PlatformToolset=v143 -m -v:m -nologo
cp bin/win-amd64-librw_gl3_glfw-oal/Release/reVC.exe C:/Users/PC/Downloads/re3-game/
```
GL shaders: `cd src/extras/shaders && sh makeinc_glsl.sh <file>` (regenerate `.inc`).
RT shaders: `cd src/extras/rtgi/shaders && GLSLANG="C:/VulkanSDK/1.4.350.0/Bin/glslangValidator.exe" sh make_spirv.sh`.
Generated `.inc` files are gitignored by pattern — commit them with `git add -f`.

## Autonomous in-game verification (use this after EVERY change — never commit unverified)

The game can test itself without any human and without taking over the desktop:

1. `C:\Users\PC\Downloads\re3-game\rtgi_autoload.txt` containing `1` → auto-loads save slot 1 past the menu.
2. `rtgi_window.txt` containing `1280 720` → runs in a small window (background-safe; use `2560 1440` for full-load perf/crash tests). Delete this file to restore the user's fullscreen — ALWAYS delete it plus `rtgi_config.txt` when you finish a work session, leaving only `rtgi_autoload.txt`.
3. `rtgi_config.txt`, one `key=value` per line: `view=` (debug views), `enabled=`, `ao=`, `aostrength=`, `aoradius=`, `aorays=`, `sunshadows=`, `gi=`, `giblend=`, `giexposure=`, `denoise=`, `reflections=`, `shotframes=N` (self-screenshot every N frames to `rtgi_shot_*.bmp` via glReadPixels — works while backgrounded), `tp=x,y,z` (teleport), `hour=H`, `weather=W` (0 sunny, 1 cloudy, 2 rainy, 3 foggy).
4. Launch minimized via PowerShell `Start-Process -WindowStyle Minimized`, sleep 90s, kill, then read `rtgi.log` (init lines + `N TLAS instances, M BLASes` telemetry) and convert `rtgi_shot_*.bmp` → PNG and **look at them** (Read tool). Judge the image like a graphics programmer: silhouettes, light direction, noise, ghosting, brightness vs. the vanilla look.
5. Crash triage: process died early → check `Get-WinEvent` Application log id 1000 for `reVC.exe` faulting-module records; `rtgi.log` shows how far init got.
6. Standard verification matrix (run the relevant subset per change): noon sunny street `tp=230,-1290,12 hour=12 weather=0`; night same spot `hour=2`; rain `weather=2`; an interior; and an `enabled=0` run to confirm vanilla is untouched.

## Working rules

- One improvement at a time: implement → build → verify in-game visually → commit (imperative message explaining what + why) → update `RTGI.md` and the backlog section in it.
- Every new feature gets: a runtime toggle (debug menu in `rtgi.cpp AddDebugMenuEntries` + `rtgi_config.txt` key), a debug view if it produces a buffer, and telemetry in `rtgi.log` if it has counts/timings.
- Never leave the tree broken; if an approach fails, revert to the last commit and record what failed in `RTGI.md` under "attempted".
- Periodically (every ~3 features) run a regression sweep: all four matrix scenarios + `enabled=0`, compare against `docs/baseline/*.png` (create this folder of known-good screenshots on your first sweep, refresh it when a change intentionally improves the look).
- Keep frame time in budget: log GPU-relevant timings; if a feature costs more than ~2 ms at 2560×1440, add a quality knob and default it sane.
- Vanilla build check occasionally: premake without `--with-rtgi` must still compile.

## Backlog (start here, highest value first; when exhausted, invent more and re-polish)

1. **Vehicle + ped pipe composite** — vehicles/peds' own forward shaders don't sample GI/AO/reflections yet (world pipe only). Mirror the `WorldRenderCB` pattern (`gbuffer.cpp`) for the vehicle pipe in `custompipes_gl.cpp` and the ped rim pipe. Car paint should pick up GI color and RT reflections (replace the env-map hack per plan).
2. **Emissive night windows & neon** — VC night models carry glow in prelight; flag night-model materials with `emissiveScale` in `GeomRecord` so windows/neon actually light sidewalks in GI. Tune at `hour=2`.
3. **Vehicle headlights as RT point/spot lights** — feed active headlight cones into the GI light SSBO (see how CPointLights are snapshotted in `lights.cpp`).
4. **Real albedo at hit points** — replace per-material mean color with actual texture fetches in hit shaders (bindless descriptor array of downsampled textures, cache keyed on `rw::Raster*`, evict via raster plugin dtor). Massively improves GI color fidelity and reflection believability.
5. **Second GI bounce** (Russian roulette) + **better denoiser** (proper SVGF variance guiding, or evaluate NRD) — current à-trous still shimmers on thin geometry.
6. **Water**: ray-traced ocean reflections (the sea is a special pipeline; at minimum give it the wet-road treatment with waves' normals).
7. **Photo mode**: freeze camera → progressive accumulation (hundreds of spp, multi-bounce) for reference screenshots; toggle + config key.
8. **Perf pass**: BLAS compaction, checkerboard GI option, timing telemetry per pass in `rtgi.log`, tune ped skinning cost.
9. **Robustness**: alt-tab/resize recreate path, device-lost handling, the one-off `nvoglv64.dll 0xc0000409` crash seen at 2560×1440 fullscreen rain — try to reproduce (fullscreen-sized window, rain, long soak) and fix or document.
10. **Revisit pass** (recurring): re-examine AO strength/radius, GI blend/exposure vs. timecycle art direction at all hours, reflection Fresnel curve, foliage stochastic-alpha coverage (0.45 — validate against tree density), sun shadow softness.

When all of the above are done and verified: profile, then go deeper — glass/window specular reflections, GI probe fallback so transparents/particles/water spray receive bounce light, interior light shafts, muzzle-flash/explosion transient lights, moon shadows at night, RTGI.md architecture doc polish, then start the revisit cycle again with fresh eyes. Never stop; there is always a surface that could be lit better.
