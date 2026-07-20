# reVC-RTGI — Ray Traced Global Illumination for reVC

This fork adds real-time ray traced lighting to reVC while leaving the game
itself untouched. The existing librw OpenGL renderer still draws every frame;
a Vulkan ray tracing side-car (VK_KHR_ray_query) traces against the live game
world and feeds the results back through GL/VK interop
(`GL_EXT_memory_object_win32` / `GL_EXT_semaphore_win32`).

**Requirements:** Windows x64, an RTX-class NVIDIA GPU (developed on an
RTX 3090), Vulkan driver. Everything is compile-time gated behind `RTGI`
and runtime-gated behind toggles — without `--with-rtgi` the build is vanilla.

## What is ray traced

- **Ambient occlusion** — cosine-hemisphere rays modulate the world's ambient
  lighting term per pixel
- **Diffuse GI (2 bounces)** — sky light and sun bounce traced per pixel
  (second bounce via 50% Russian roulette), temporal accumulation with
  luminance-moment history, and a variance-guided edge-aware à-trous
  denoiser (SVGF-style); replaces part of the flat timecycle ambient
  (blend knob preserves art direction)
- **Sun/moon shadows** — vehicles and peds cast accurate ray traced shadows
  (replaces their blob shadows); world sun light stays baked, as shipped.
  At night the moon takes over as caster: fixed southern direction
  matching the moon sprite (offset 0,-100,15), strength follows the
  sprite's brightness ramp (full at 3AM, fades over ±3h, dimmed by
  cloud/fog) × 0.30 (`moonshadows=` toggle)
- **Point lights** — streetlights/headlights feed the GI bounce (NEE)
- **Emissive night models** — lit windows and neon (VC's timed night
  models) emit their material color into the GI bounce and into wet-road/
  paint reflections; luminance-gated (> 0.35) so only genuinely bright
  materials emit; `emissive=F` config / debug-menu boost knob
- **Headlight cones** — every nearby vehicle with lights on adds a warm
  ~45° spot light to the GI light set
- **Real hit albedo** — GI bounces and reflections sample small cached
  copies of the actual game textures (1024-slot bindless array, UVs
  interpolated at the hit) instead of per-material mean colors
- **Reflections** — wet roads mirror the actual scene when it rains, with a
  physically-shaped Fresnel curve (faint sheen straight down, mirror at
  grazing); per-surface reflectivity rides in G-buffer normal.w — vehicles
  encode base paint reflectivity (negated), roads use the game's own
  per-model wet-reflection flag, other surfaces get a light sheen. The
  buffer is filtered by a dedicated temporal pass (surface-motion
  reprojection with a 3x3 mean±sigma neighborhood clamp — reflections have
  no true motion vectors, the clamp rejects parallax ghosting) plus a
  depth/normal-weighted spatial mean that fades out as history converges;
  smooths the 45% stochastic foliage/glass dither and paint sparkle
- **Skinned peds** — CPU-posed every frame into per-ped BLASes so they occlude
  and cast like everything else; peds are also in the G-buffer (skinned
  vertex path) and composite AO/GI/sun shadows in their forward pass
- **Vehicles** — composite AO/GI and receive ray traced sun shadows; matFX
  env-map materials sample the RT reflection buffer instead of the env hack
- **Foliage** — alpha-tested materials (palms, shrubs) build as non-opaque
  BLAS ranges; every RT pass traverses them stochastically (45% coverage) so
  canopies cast soft partial shadows/AO instead of solid-quad blobs, and the
  G-buffer excludes them from the wet-sheen treatment
- **Vehicle glass** — translucent panes (windshields, windows; material
  alpha < 255, distinguishing them from alpha-cutout cargo/grilles) enter
  the G-buffer with a glass marker (normal.w = 3) and get a deterministic
  Fresnel mirror from the reflection pass (F0 0.04 → 0.95 at grazing);
  the composite tints the pane by the Fresnel weight while keeping the
  vanilla translucency (`glassrefl=` toggle)
- **World glass panes** — translucent world/object meshes (storefronts,
  the biker-bar/emporium fronts, breakable shop glass; material
  alpha < 255) get the same Fresnel glass marker as vehicle panes.
  Baked "window" facade art stays untouched — an earlier texture-name
  heuristic double-reflected over prebaked reflections and was reverted
  (VC world models also carry no matFX env maps; both signals dead ends,
  the material-translucency test is the real one). Glass-marked mesh
  count rides the telemetry line; `dumptex=1` logs texture names (dev)
- **Interior light shafts** — inside interiors (area != 0, sun up) a
  half-res pass marches the view ray (8 jittered steps) tracing sun
  visibility per step; interiors are sealed shells, so panes textured
  with the flat sky fill ("skyblue") are marked as SUN PORTALS in the
  BLAS records (texSlot bit 16 — slot consumers mask 0xFFFF) and count
  as reaching the sun. A 3x3 blur kills the jitter dither; the result
  composites additively before the HUD. Scatter = lit path length ×
  0.12/m × forward phase. `volumetrics=`/`volstrength=` (+`volalways=`
  dev key to run outdoors); debug view "Volumetrics"; ~1.8 ms at 1080p
  interior-only
- **Sea reflections** — the water surface is re-rendered into the G-buffer
  (via a librw im3d shader-override hook, patch in docs/librw-rtgi.patch)
  and the reflection pass mirrors the actual scene off it with a
  procedural swell ripple; the forward water pass mixes the result over
  the vanilla look by fresnel strength
- **Shader-only water** (user direction) — the vanilla water texture,
  including its baked reflective sparkle, is dropped entirely: base tint
  comes from the timecycle water color (×0.62 standing in for the removed
  texture's mean), the caustic shimmer rides on top (world-space tiled
  ~12.6 m, distance-faded 60→220 m), and the RT sea reflection mixes over
  it. Texture alpha is kept as the shore mask; far water goes opaque
  (150→400 m) so the seabed LOD cannot grid through it. The near-camera
  wavy/mask ATOMICS bypassed the im3d override (vanilla-texture pop up
  close) and now draw through the same shader (`RenderWaterAtomic`).
  OG-water textures (waterclear/lodwaterclear/seabed) are stripped from
  the world pipe + stock matFX pipe (hooked; LOD sea atomics live there)
  and the neo gloss sparkle pass skips them; water draw distance ×3 while
  the procedural look is active. The sea-reflection swell ripple flattens
  with distance (80→400 m) so grazing mirror rays stay coherent instead
  of dissolving the horizon into speckle. `watercaustics=0` restores the
  textured look; camera pos rides in `u_gbParams` (librw uniform registry
  is at its 40-slot cap — a NEW registerUniform silently returns -1!)

## Architecture

```
src/extras/rtgi/
  rtgi.cpp        orchestration, debug menu, dev config
  vkcontext.*     Vulkan device (volk + VMA), LUID-matched to the GL context
  interop.*       VK-allocated GL-imported shared images + semaphore pairs
  gbuffer.*       GL prepass (world normals + linear depth) over the visible set
  blas.*          one BLAS per rw::Geometry from librw's retained CPU triangles;
                  freed automatically via a geometry-destructor plugin
  tlas.*          per-frame TLAS from CWorld sectors + big buildings; interior
                  area masking; per-ped skinning slots
  passes.*        ray query compute passes: AO+shadow, GI, temporal, à-trous,
                  reflections + reflection temporal/spatial filter
  shaders/        GLSL -> SPIR-V (.spv.inc committed; SDK only needed to edit)
```

Frame flow: `Idle()` → G-buffer prepass (GL) → semaphore → VK: BLAS builds
(8/frame budget) → TLAS rebuild → AO/shadow → GI → temporal → denoise →
reflections → semaphore → forward pass samples the results in the world shader
(`src/extras/shaders/rtgiWorld.*`).

## Build

```
premake5 vs2019 --with-librw --with-rtgi
powershell -File docs/build_deploy.ps1
```

`build_deploy.ps1` wraps MSBuild and then deterministically deploys: it
pushes the fresh exe to the test box (192.168.0.209) and verifies the
remote MD5 matches the local build, so the box can never silently run a
stale binary. `-NoDeploy` builds only; `-SyncAssets` also pushes changed
game-dir assets (name+size diff, runtime artifacts excluded);
`-SkipBuild` deploys an existing <20-min-old exe.

librw must be present at `vendor/librw` (current aap/librw master; the fork
carries compatibility fixes for it).

## Runtime controls

Debug menu (Ctrl+M) → RTGI: master toggle, AO/GI/shadows/reflections toggles,
AO strength/radius/rays, GI blend/exposure, debug views (RT normals/depth/
instances, AO, G-buffer, sun visibility, GI, reflections).

`rtgi_config.txt` next to the exe (for automated testing): `view=N`,
`enabled/ao/gi/gi2/photo/denoise/reflections/reflfilter/glassrefl/sunshadows/moonshadows/watercaustics=0|1`,
`aostrength/aoradius/giblend/giexposure/emissive=F`, `aorays=N`,
`shotframes=N` (periodic BMP dumps), `dumptex=1` (log distinct world
texture names once each), `tp=x,y,z[,heading]` (teleport
after load; heading in degrees, 0 = north, CCW, snaps the camera
behind), `area=N` (interior for the teleport, eAreaName in Game.h),
`hour=N`, `weather=N`, `explode=x,y,z` (detonate a grenade-type explosion
there every ~2.5 s after settle — combat-light verification),
`cutscene=NAME,x,y,z` (play mission cutscene NAME's
camera spline at that offset — deterministic sweeps for A/B captures;
`cutloop=1` restarts it forever; names e.g. INT_A, LAW_1A, CUB_1 from
ANIM\CUTS.IMG), plus `rtgi_autoload.txt` containing a save slot
number (1-8) to auto-load and `rtgi_window.txt` ("W H") forcing a small
window for background runs.

The config file is **hot-reloaded**: edits are picked up mid-run (mtime
polled every 30 frames). Re-applying is idempotent for unchanged values;
a `tp=` line re-teleports on every reload. Capture scripts use this to
arm `shotframes` only after the scene has settled — dense `glReadPixels`
dumps during the load/teleport phase can livelock the frame loop, so
boot with `shotframes=0` and switch it on once loaded (see
`docs/comparisons/capture.ps1`).

Photo mode (`photo=1` / debug menu): while the camera holds still the GI
accumulates a true average (up to 4096 spp, denoiser bypassed, 90%
second-bounce probability) for clean reference shots; any camera motion
restarts the average.
The harness (teleport/clock/weather/screenshots) also works with
`enabled=0`, so vanilla comparison runs land in the identical scene.
`rtgi.log` carries init diagnostics and TLAS/BLAS telemetry.

## Backlog / notes

See `AGENT_PROMPT.md` for the living backlog. Tuned constants worth
revisiting: foliage stochastic coverage 0.45 (`TRAVERSE` macros + primary),
wall wet sheen 0.25, road wet reflectivity cap 0.75, vehicle paint base
0.35 with clearcoat Fresnel mix(0.08, 2.0, pow5).

`docs/baseline/` holds known-good sweep screenshots (noon/night/rain at
tp=230,-1290,12, hotel exterior, vanilla). Refresh when a change
intentionally improves the look.

Observations to revisit:
- Photo-mode AO accumulation works (verified 2026-07-20; the old "grain in
  photo stills" note predated temporal AO). Remaining photo-mode artifact:
  the player's idle sway stays inside the 5% depth-reprojection tolerance,
  so the true average smears a speckled fringe along his silhouette.
  Possible fix: tighten depth tolerance (or add a normal-difference test)
  when photo mode is active.
- Rain feels darker than vanilla art direction (GI pulls ambient toward the
  dark storm sky); consider scaling giblend down in rain, or brightening the
  sky term under overcast weathers.
- `area=` config key exists but interior floor coordinates per area still
  need cataloguing (hotel = save start; malibu attempt at 489.6,-84.5 was
  not the club floor).
- Vehicle windshields are non-opaque in the BLAS, so reflections that hit
  glass dither at 45% coverage (the reflection filter smooths the residue,
  but glass deserves a deterministic Fresnel treatment — see backlog).
- Denoiser is now variance-guided; if thin-geometry shimmer persists in
  motion, next steps are variance spatial filtering and a history clamp.
- GPU timings (restored-window 1440p, test-box 3090, 2026-07-20 with
  glass + reflection filter + procedural water): blas/tlas 0.68, ao 2.4,
  gi 3.05, denoise 2.5, refl 0.27, reflt 0.11 ≈ 9.0 ms total. Earlier
  dev-PC reference: 7.5 ms (ao 1.9, gi 2.6, denoise 2.2, refl 0.2); rain
  ~10 ms. MINIMIZED runs report inflated numbers (GPU power state) —
  compare like with like; the capture agent's gif mode restores the
  window off-screen for honest measurements.
- 0xc0000409 fullscreen-rain crash: did NOT reproduce in a 9-minute
  2560x1440 windowed rain soak (stable timings throughout). Suspect
  exclusive-fullscreen swapchain interaction; needs a real fullscreen
  session to chase further.
- Texture cache reached 975/1024 after ~10 min of streaming; long play
  sessions will hit the cap and fall back to mean colors — consider 2048
  or LRU eviction via a raster-destructor hook.
