#pragma once

#ifdef RTGI

namespace rw { struct Atomic; struct Texture; namespace gl3 { struct InstanceDataHeader; } }

namespace RayTracedGI {

// GL-side pieces: the G-buffer prepass FBO/shader and the AO-aware world
// forward shader used by the custom world pipe.

bool GbufferInit(int width, int height);
void GbufferShutdown(void);

// render world normals + linear depth for the current camera into the shared
// G-buffer images. Call inside the RW frame (camera active), before the VK
// submit.
void GbufferRender(void);

// forward render callback for world atomics: samples the AO texture in the
// ambient term. Returns false when RTGI is off (caller runs its normal path).
bool WorldRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);

// per-frame telemetry counter (plain unsigned: this header is included
// before common.h's uint32 typedef exists)
unsigned int EnvGlassMeshCount(void);

// true when the procedural water look owns a surface with this texture —
// the neo gloss pipe must not add the baked water sparkle over it
bool SuppressOGWaterGloss(rw::Texture *tex);
// true while the shader-only water look is active (extends water draw
// distance so the seabed LOD never peeks past the water at the horizon)
bool UsingProceduralWater(void);

// same composite for vehicle atomics; RT reflections replace the matFX
// env-map pass on materials that carried one.
bool VehicleRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);

// ped composites (rigid and skinned); no reflections on skin/cloth.
bool PedRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);
bool PedSkinRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);

// sea reflections: wrap the forward water draws with the im3d override
// that mixes the RT reflection buffer over the vanilla water.
void WaterRenderBegin(void);
void WaterRenderEnd(void);
// near-camera wavy/mask water atomics bypass the im3d override; this draws
// them with the RTGI water shader instead. Returns false when RTGI is off
// (caller renders vanilla).
bool RenderWaterAtomic(rw::Atomic *atomic);
// true while GbufferRender re-renders the sea into the G-buffer
extern bool gbWaterGbufPass;

}

#endif
