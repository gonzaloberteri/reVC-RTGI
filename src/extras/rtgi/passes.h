#pragma once

#ifdef RTGI

#include "vkcontext.h"

namespace RayTracedGI {

bool PassesInit(void);
void PassesShutdown(void);

// primary-ray debug trace into the shared output image.
// mode: 1 = normals, 2 = depth, 3 = instances (matches primary.comp)
void PassesTracePrimary(VkCommandBuffer cmd, uint32_t mode, uint32_t frame);

// ray traced AO from the G-buffer into the shared AO image
void PassesTraceAO(VkCommandBuffer cmd, uint32_t frame, uint32_t numRays, float radius);

// one-bounce diffuse GI + temporal accumulation into the shared GI image
bool PassesTraceGI(VkCommandBuffer cmd, uint32_t frame, bool resetHistory);

// a-trous spatial filter over the accumulated GI, output into the shared image
void PassesDenoiseGI(VkCommandBuffer cmd);

// mirror reflections into the shared reflection image
void PassesTraceReflections(VkCommandBuffer cmd, uint32_t frame);

// lights fed to the last GI pass (game point lights + headlight cones)
uint32_t GiLightCount(void);
uint32_t GiHeadlightCount(void);

}

#endif
