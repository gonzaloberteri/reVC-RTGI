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

}

#endif
