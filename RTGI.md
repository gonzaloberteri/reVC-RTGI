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
  (replaces their blob shadows); world sun light stays baked, as shipped
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
  per-model wet-reflection flag, other surfaces get a light sheen
- **Skinned peds** — CPU-posed every frame into per-ped BLASes so they occlude
  and cast like everything else; peds are also in the G-buffer (skinned
  vertex path) and composite AO/GI/sun shadows in their forward pass
- **Vehicles** — composite AO/GI and receive ray traced sun shadows; matFX
  env-map materials sample the RT reflection buffer instead of the env hack
- **Foliage** — alpha-tested materials (palms, shrubs) build as non-opaque
  BLAS ranges; every RT pass traverses them stochastically (45% coverage) so
  canopies cast soft partial shadows/AO instead of solid-quad blobs, and the
  G-buffer excludes them from the wet-sheen treatment
- **Sea reflections** — the water surface is re-rendered into the G-buffer
  (via a librw im3d shader-override hook, patch in docs/librw-rtgi.patch)
  and the reflection pass mirrors the actual scene off it with a
  procedural swell ripple; the forward water pass mixes the result over
  the vanilla look by fresnel strength

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
                  reflections
  shaders/        GLSL -> SPIR-V (.spv.inc committed; SDK only needed to edit)
```

Frame flow: `Idle()` → G-buffer prepass (GL) → semaphore → VK: BLAS builds
(8/frame budget) → TLAS rebuild → AO/shadow → GI → temporal → denoise →
reflections → semaphore → forward pass samples the results in the world shader
(`src/extras/shaders/rtgiWorld.*`).

## Build

```
premake5 vs2019 --with-librw --with-rtgi
msbuild build/reVC.sln -p:Configuration=Release -p:Platform=win-amd64-librw_gl3_glfw-oal -p:PlatformToolset=v143
```

librw must be present at `vendor/librw` (current aap/librw master; the fork
carries compatibility fixes for it).

## Runtime controls

Debug menu (Ctrl+M) → RTGI: master toggle, AO/GI/shadows/reflections toggles,
AO strength/radius/rays, GI blend/exposure, debug views (RT normals/depth/
instances, AO, G-buffer, sun visibility, GI, reflections).

`rtgi_config.txt` next to the exe (for automated testing): `view=N`,
`enabled/ao/gi/gi2/denoise/reflections/sunshadows=0|1`, `aostrength/
aoradius/giblend/giexposure/emissive=F`, `aorays=N`, `shotframes=N`
(periodic BMP dumps), `tp=x,y,z` (teleport after load), `area=N`
(interior for the teleport, eAreaName in Game.h), `hour=N`, `weather=N`,
plus `rtgi_autoload.txt` containing a save slot number (1-8) to auto-load
and `rtgi_window.txt` ("W H") forcing a small window for background runs.
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
- Rain feels darker than vanilla art direction (GI pulls ambient toward the
  dark storm sky); consider scaling giblend down in rain, or brightening the
  sky term under overcast weathers.
- `area=` config key exists but interior floor coordinates per area still
  need cataloguing (hotel = save start; malibu attempt at 489.6,-84.5 was
  not the club floor).
- Vehicle windshields are non-opaque in the BLAS, so reflections that hit
  glass dither at 45% coverage.
- Denoiser is now variance-guided; if thin-geometry shimmer persists in
  motion, next steps are variance spatial filtering and a history clamp.
- GPU timings (native 1440p, 3090): blas/tlas 0.65, ao 1.9, gi 2.6,
  denoise 2.2, refl 0.2 ms. Background-window runs report inflated
  numbers (GPU power state) — compare like with like.
