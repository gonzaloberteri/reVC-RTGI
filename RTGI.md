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
- **Diffuse GI (1 bounce)** — sky light and sun bounce traced per pixel with
  temporal accumulation and an edge-aware à-trous denoiser; replaces part of
  the flat timecycle ambient (blend knob preserves art direction)
- **Sun/moon shadows** — vehicles and peds cast accurate ray traced shadows
  (replaces their blob shadows); world sun light stays baked, as shipped
- **Point lights** — streetlights/headlights feed the GI bounce (NEE)
- **Reflections** — wet roads mirror the actual scene when it rains; vehicles
  carry per-surface reflectivity in the G-buffer
- **Skinned peds** — CPU-posed every frame into per-ped BLASes so they occlude
  and cast like everything else

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
`enabled/ao/gi/denoise/reflections/sunshadows=0|1`, `aostrength/aoradius/
giblend/giexposure=F`, `aorays=N`, `shotframes=N` (periodic BMP dumps),
`tp=x,y,z` (teleport after load), `hour=N`, `weather=N`,
plus `rtgi_autoload.txt` containing a save slot number (1-8) to auto-load.
`rtgi.log` carries init diagnostics and TLAS/BLAS telemetry.
