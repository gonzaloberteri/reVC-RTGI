#pragma once

#ifdef RTGI

#include "vkcontext.h"

namespace rw { struct Geometry; }

namespace RayTracedGI {

// One BLAS per rw::Geometry, shared by every entity instancing that model.
// Geometry ranges are bucketed by material so hit shaders can resolve
// per-material data through geometryIndex.
struct BlasEntry
{
	VkAccelerationStructureKHR as;
	VkDeviceAddress asAddr;
	GpuBuffer asBuf;
	GpuBuffer vtxBuf;	// vec3 float positions, device address readable
	GpuBuffer idxBuf;	// uint32 indices, all material ranges back to back
	int32_t numRanges;	// == number of non-empty material buckets
	uint32_t firstRecord;	// index of this geometry's first GeomRecord
};

// per (BLAS, geometry range) record the shaders use to fetch triangle data
struct GeomRecord
{
	VkDeviceAddress vtxAddr;
	VkDeviceAddress idxAddr;	// start of this range's indices
};

// register the librw geometry-destructor plugin. Call once, before game
// assets load, so streamed-out geometry frees its BLAS automatically.
void BlasRegisterPlugin(void);

bool BlasInit(void);
void BlasShutdown(void);

// start a new frame: process deferred frees (safe: caller has fence-waited),
// reset the per-frame build budget
void BlasBeginFrame(void);

// look up the BLAS for a geometry; if none exists and the build budget
// allows, records a build into cmd and returns the (buildable) entry.
// Returns nil when over budget or the geometry is unsuitable.
BlasEntry *BlasGetOrBuild(rw::Geometry *geo, VkCommandBuffer cmd);

// upload the geometry record table if it changed; returns the SSBO
GpuBuffer *BlasRecordBuffer(void);

int32_t BlasCount(void);

}

#endif
