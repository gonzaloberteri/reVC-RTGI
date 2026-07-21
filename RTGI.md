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
  (blend knob preserves art direction). The temporal pass reprojects with
  camera matrices only (no object motion vectors), so history under a
  MOVING occluder passes the depth test while still holding its shading —
  both the GI and the accumulated AO clamp their history to the current
  frame's 3x3 raw mean ± sigma (GI 2σ, AO 1.5σ; skipped in photo mode and
  on untraced checkerboard tiles) so fast cars cannot smear their
  shadow/occlusion into ghost trails
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
  vertex path) and composite AO/GI in their forward pass. Peds do NOT
  receive the RT sun-shadow term on their own bodies: their BLAS sits in
  the shadow-ray mask, so body pixels self-hit and a hard 1-ray shadow
  painted uncanny bands on the clothes — vanilla peds are flat
  directionally lit, and the composite matches that (they still cast onto
  the ground)
- **Vehicles** — composite AO/GI and receive ray traced sun shadows; matFX
  env-map materials sample the RT reflection buffer instead of the env hack.
  The _vlo/_lo LOD shell atomics are skipped in the G-buffer and TLAS walks
  (their RENDER flag stays set forever — the forward pass distance-gates
  them inside their render callbacks, and drawing them anyway wrapped every
  car in a low-poly reflective box; AtomicIsVehicleLod matches the five
  far-only LOD callbacks by renderCB pointer)
- **Foliage** — alpha-tested materials (palms, shrubs) build as non-opaque
  BLAS ranges. The color passes (reflections, GI) commit candidates with
  probability = the real texture alpha at the hit UV × material alpha
  (material alpha rides in GeomRecord matColor bits 24-31): leaf cutouts
  pass through instead of mirroring as black quads, solid leaf texels
  always block, translucent glass keeps a coverage dither the filters
  smooth. AO/shadow/volumetric rays keep the cheap flat 45% coin flip
  (soft canopy shade, no color fetched). The G-buffer excludes foliage
  from the wet-sheen treatment
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
- **Breakable glass panes** — two families, per the game's own IDE flags:
  *Artist glass* (flag 0x400 — the mall shop windows: ml_gapwindows,
  ml_recordwin, ml_jewelwin, ml_coffwin, the bikeshop fronts; these are
  the panes a player actually shatters) stays a visible atomic through the
  normal world pipe, but its translucency lives in the TEXTURE alpha with
  material alpha 255, so the meshIsGlass material test skipped it — the
  G-buffer walk now forces the glass marker for meshes of IsGlass-flagged
  models (`forceGlass` in gbufDrawAtomic), and the world composite mirrors
  them like any pane. *Code glass* (flag 0x200 — rare: police-station and
  downtown panes, mostly debug/mission placements, not normally
  player-reachable) is an invisible entity whose visual is a sliding
  fake-reflection quad CGlass draws from the collision model; the G-buffer
  prepass emits the same collision quads with the glass marker
  (`CGlass::RenderForRTGIGbuffer`, position-only im3d — face normals come
  from the G-buffer frag's derivatives) and CGlass's reflection-quad draw
  swaps the fake texture for the RT mirror (`GlassMirrorBegin/End` im3d
  override, `rtgiGlassMirror.frag`: reflection buffer by fragcoord,
  Fresnel from refl.a, vanilla 30-40 m fade via vertex alpha). Cracked
  panes keep the crack overlay; broken panes and falling shards stay
  vanilla. Pane counts ride the glass-mesh telemetry; `glassrefl=` gates
  everything
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
  is at its 40-slot cap — a NEW registerUniform silently returns -1!).
  The near paths (im3d quads + wavy/mask atomics) engage whenever ANY
  RTGI water feature is on — they must NOT require the reflection toggle
  (that split the sea: procedural far, vanilla textured near, with the
  camera-following mask atomic keeping the OG env-mapped look glued to
  the player). `u_gbParams.w` also carries a +2 flag when the reflection
  pass is off so the stale reflection buffer never mixes in. During the
  G-buffer prepass the wavy atomic draws with the CALLER's G-buffer
  shader/marker (it used to scribble forward water colors into the
  normal attachment, so near water lost its sea marker), and the forward
  atomic draw pins the translucent blend state explicitly (the caller
  sets none — inherited state was a source of z/blend artifacts)

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
- 0xc0000409 crash: reproduced ONCE (windowed 1080p) right after
  teleporting inside the mall's solid roof mass — "frame fence wait
  failed (2)" (VK_TIMEOUT, i.e. a GPU hang/TDR) followed by the crash;
  the same teleport then survived a retry, so the repro is
  non-deterministic. All traversal loops now carry a 512-candidate
  iteration guard (rayQueryTerminateEXT bail-out) so a pathological
  candidate stream cannot stall the GPU — defensive hardening, not a
  proven fix. The older fullscreen-rain sighting may be the same class.
  Did NOT reproduce in a 9-minute 2560x1440 windowed rain soak.
- Texture cache reached 975/1024 after ~10 min of streaming; long play
  sessions will hit the cap and fall back to mean colors — consider 2048
  or LRU eviction via a raster-destructor hook.
