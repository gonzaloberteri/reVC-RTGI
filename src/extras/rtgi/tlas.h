#pragma once

#ifdef RTGI

#include "vkcontext.h"

namespace RayTracedGI {

// TLAS instance visibility masks
enum {
	MASK_STATIC = 0x01,	// world, objects
	MASK_VEHICLES = 0x02,
	MASK_ALL = 0xFF,
};

bool TlasInit(void);
void TlasShutdown(void);

// walk CWorld sectors around the camera plus the big-building lists, emit
// one TLAS instance per rendered atomic (clumps become several instances,
// which makes vehicle doors/wheels correct for free). Wrong-area entities
// (interiors) are skipped. Must run on the game thread; also queues BLAS
// builds into cmd (budgeted).
void TlasCollect(VkCommandBuffer cmd);

// record the TLAS build; call after TlasCollect in the same command buffer
// (a barrier between BLAS builds and this is inserted here)
bool TlasBuild(VkCommandBuffer cmd);

VkAccelerationStructureKHR TlasHandle(void);
uint32_t TlasInstanceCount(void);

}

#endif
