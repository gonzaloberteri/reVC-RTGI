#pragma once

#ifdef RTGI

#include "vkcontext.h"

namespace RayTracedGI {

bool PassesInit(void);
void PassesShutdown(void);

// primary-ray debug trace into the shared output image.
// mode: 1 = normals, 2 = depth, 3 = instances (matches primary.comp)
void PassesTracePrimary(VkCommandBuffer cmd, uint32_t mode, uint32_t frame);

}

#endif
