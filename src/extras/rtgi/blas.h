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
	GpuBuffer uvBuf;	// vec2 float texcoords (optional; 0 when absent)
	int32_t numRanges;	// == number of non-empty material buckets
	uint32_t firstRecord;	// index of this geometry's first GeomRecord
};

// per (BLAS, geometry range) record the shaders use to fetch triangle data.
// layout mirrored in gi.comp/refl.comp — keep in sync
struct GeomRecord
{
	VkDeviceAddress vtxAddr;
	VkDeviceAddress idxAddr;	// start of this range's indices
	VkDeviceAddress uvAddr;		// vec2 texcoords, 0 when the mesh has none
	uint32_t albedo;		// RGBA8: material color x mean texture color (fallback)
	uint32_t emissive;		// RGBA8 emitted color (night windows/neon); 0 = none
	uint32_t texSlot;		// texture cache slot for real albedo; ~0u = none
	uint32_t matColor;		// RGBA8 plain material color (multiplies the texture)
};

// --- hit-point texture cache: downsampled copies of game textures in a
// sampled-image array so hit shaders fetch real albedo -----------------------
// 975 slots were live after a 10-minute streaming soak at 1024; 2048 gives
// long sessions headroom (~64 MB worst case of small RGBA8 copies)
enum { TEXCACHE_MAX = 2048 };

// records the slot-0 dummy upload on first use; call once per frame before
// the trace passes are recorded
void TexCacheEnsureDummy(VkCommandBuffer cmd);
uint32_t TexCacheCount(void);
VkImageView TexCacheView(uint32_t slot);
VkSampler TexCacheSampler(void);

// register the librw geometry-destructor plugin. Call once, before game
// assets load, so streamed-out geometry frees its BLAS automatically.
void BlasRegisterPlugin(void);

bool BlasInit(void);
void BlasShutdown(void);

// start a new frame: process deferred frees and last frame's compaction
// queries (safe: caller has fence-waited), reset the per-frame build budget.
// Records compact-copies into cmd.
void BlasBeginFrame(VkCommandBuffer cmd);

// after all builds are recorded: queue compacted-size queries for this
// frame's builds (results consumed next BlasBeginFrame)
void BlasEndFrame(VkCommandBuffer cmd);

uint32_t BlasCompactionSavedMB(void);

// look up the BLAS for a geometry; if none exists and the build budget
// allows, records a build into cmd and returns the (buildable) entry.
// Returns nil when over budget or the geometry is unsuitable.
// emissiveScale > 0 marks the geometry's materials as light emitters in the
// GI bounce (night-model windows and neon), using their albedo as the color.
BlasEntry *BlasGetOrBuild(rw::Geometry *geo, VkCommandBuffer cmd, float emissiveScale = 0.0f);

// upload the geometry record table if it changed; returns the SSBO
GpuBuffer *BlasRecordBuffer(void);

// allocate a persistent record slot (for dynamic geometry like skinned peds);
// returns its index, or UINT32_MAX when full
uint32_t BlasAllocRecord(const GeomRecord &rec);

int32_t BlasCount(void);

}

#endif
