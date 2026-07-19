#pragma once

#ifdef RTGI

namespace rw { struct Atomic; namespace gl3 { struct InstanceDataHeader; } }

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

// same composite for vehicle atomics; RT reflections replace the matFX
// env-map pass on materials that carried one.
bool VehicleRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);

// ped composites (rigid and skinned); no reflections on skin/cloth.
bool PedRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);
bool PedSkinRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);

}

#endif
