#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include "blas.h"

#include "common.h"
#include <rwcore.h>
#include <rpworld.h>

namespace RayTracedGI {

// budgets — missing BLASes just skip the TLAS for a frame or two, which
// matches streaming pop-in, so a small budget avoids build hitches
enum {
	BUILDS_PER_FRAME = 8,
	MAX_RECORDS = 65536,
	DEFERRED_FRAMES = 3,	// frames to hold freed GPU objects (single in-flight + slack)
};

static std::unordered_map<rw::Geometry*, BlasEntry*> gBlasMap;

struct DeferredFree { BlasEntry *entry; int framesLeft; };
static std::vector<DeferredFree> gDeferredFrees;

static GeomRecord gRecords[MAX_RECORDS];
static uint32_t gNumRecords;
static bool gRecordsDirty;
static GpuBuffer gRecordBuf;

static int gBuildsThisFrame;
// per-build scratch buffers; alive until the frame fence proves the builds
// finished, then freed in BlasBeginFrame
static std::vector<GpuBuffer> gFrameScratch;
static std::vector<GpuBuffer> gPrevFrameScratch;

// --- geometry destructor plugin ----------------------------------------------

#define ID_RTGI 0x52544749	// 'RTGI'

static void*
rtgiGeometryDtor(void *object, int32, int32)
{
	rw::Geometry *geo = (rw::Geometry*)object;
	auto it = gBlasMap.find(geo);
	if(it != gBlasMap.end()){
		// the rw::Geometry memory dies now; GPU objects die after the
		// in-flight frames are provably done
		gDeferredFrees.push_back({ it->second, DEFERRED_FRAMES });
		gBlasMap.erase(it);
	}
	return object;
}

void
BlasRegisterPlugin(void)
{
	rw::Geometry::registerPlugin(0, ID_RTGI, nil, rtgiGeometryDtor, nil);
}

// --- lifecycle ----------------------------------------------------------------

bool
BlasInit(void)
{
	gNumRecords = 0;
	gRecordsDirty = false;
	return true;
}

static void
blasEntryDestroy(BlasEntry *e)
{
	if(e->as)
		vkDestroyAccelerationStructureKHR(gVk.device, e->as, nullptr);
	BufferDestroy(&e->asBuf);
	BufferDestroy(&e->vtxBuf);
	BufferDestroy(&e->idxBuf);
	delete e;
}

void
BlasShutdown(void)
{
	vkDeviceWaitIdle(gVk.device);
	for(auto &kv : gBlasMap)
		blasEntryDestroy(kv.second);
	gBlasMap.clear();
	for(auto &df : gDeferredFrees)
		blasEntryDestroy(df.entry);
	gDeferredFrees.clear();
	BufferDestroy(&gRecordBuf);
	for(auto &b : gFrameScratch) BufferDestroy(&b);
	for(auto &b : gPrevFrameScratch) BufferDestroy(&b);
	gFrameScratch.clear();
	gPrevFrameScratch.clear();
	gNumRecords = 0;
}

void
BlasBeginFrame(void)
{
	gBuildsThisFrame = 0;
	// scratch from two frames ago is fence-proven idle
	for(auto &b : gPrevFrameScratch)
		BufferDestroy(&b);
	gPrevFrameScratch.swap(gFrameScratch);
	gFrameScratch.clear();
	// caller has waited the frame fence, so anything queued long enough ago
	// is no longer referenced by the GPU
	for(size_t i = 0; i < gDeferredFrees.size(); ){
		if(--gDeferredFrees[i].framesLeft <= 0){
			blasEntryDestroy(gDeferredFrees[i].entry);
			gDeferredFrees[i] = gDeferredFrees.back();
			gDeferredFrees.pop_back();
		}else
			i++;
	}
}

// --- build --------------------------------------------------------------------

BlasEntry*
BlasGetOrBuild(rw::Geometry *geo, VkCommandBuffer cmd)
{
	auto it = gBlasMap.find(geo);
	if(it != gBlasMap.end())
		return it->second;

	if(gBuildsThisFrame >= BUILDS_PER_FRAME)
		return nil;
	if(geo->flags & rw::Geometry::NATIVE)
		return nil;
	if(geo->numTriangles == 0 || geo->numVertices == 0 ||
	   geo->triangles == nil || geo->morphTargets == nil ||
	   geo->morphTargets[0].vertices == nil)
		return nil;

	int32_t numMats = geo->matList.numMaterials;
	if(numMats <= 0)
		numMats = 1;

	// bucket triangles by material (geo->triangles carries matId; this also
	// sidesteps tristrip handling in the mesh header)
	std::vector<uint32_t> matTriCount(numMats, 0);
	for(int32_t i = 0; i < geo->numTriangles; i++){
		int32_t m = geo->triangles[i].matId;
		if(m < 0 || m >= numMats) m = 0;
		matTriCount[m]++;
	}

	int32_t numRanges = 0;
	for(int32_t m = 0; m < numMats; m++)
		if(matTriCount[m] > 0)
			numRanges++;
	if(numRanges == 0)
		return nil;
	if(gNumRecords + numRanges > MAX_RECORDS){
		static bool warned;
		if(!warned){
			RtgiLog("RTGI: geometry record table full (%u)\n", gNumRecords);
			warned = true;
		}
		return nil;
	}

	BlasEntry *e = new BlasEntry();
	memset(e, 0, sizeof(*e));

	// vertex buffer (shared by all ranges)
	VkBufferUsageFlags inputUsage =
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if(!BufferCreate(&e->vtxBuf, geo->numVertices * 3*sizeof(float), inputUsage, true) ||
	   !BufferCreate(&e->idxBuf, geo->numTriangles * 3*sizeof(uint32_t), inputUsage, true)){
		blasEntryDestroy(e);
		return nil;
	}

	float *vtx = (float*)e->vtxBuf.mapped;
	rw::V3d *src = geo->morphTargets[0].vertices;
	for(int32_t i = 0; i < geo->numVertices; i++){
		vtx[i*3+0] = src[i].x;
		vtx[i*3+1] = src[i].y;
		vtx[i*3+2] = src[i].z;
	}

	// indices: per-material ranges back to back
	uint32_t *idx = (uint32_t*)e->idxBuf.mapped;
	std::vector<uint32_t> matOffset(numMats, 0);	// in triangles
	{
		uint32_t off = 0;
		for(int32_t m = 0; m < numMats; m++){
			matOffset[m] = off;
			off += matTriCount[m];
		}
	}
	{
		std::vector<uint32_t> cursor = matOffset;
		for(int32_t i = 0; i < geo->numTriangles; i++){
			int32_t m = geo->triangles[i].matId;
			if(m < 0 || m >= numMats) m = 0;
			uint32_t t = cursor[m]++;
			idx[t*3+0] = geo->triangles[i].v[0];
			idx[t*3+1] = geo->triangles[i].v[1];
			idx[t*3+2] = geo->triangles[i].v[2];
		}
	}

	// geometry ranges
	std::vector<VkAccelerationStructureGeometryKHR> geoms;
	std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
	std::vector<uint32_t> primCounts;
	e->firstRecord = gNumRecords;
	for(int32_t m = 0; m < numMats; m++){
		if(matTriCount[m] == 0)
			continue;
		VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
		g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;	// alpha handled later via material records
		g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		g.geometry.triangles.vertexData.deviceAddress = e->vtxBuf.addr;
		g.geometry.triangles.vertexStride = 3*sizeof(float);
		g.geometry.triangles.maxVertex = geo->numVertices - 1;
		g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		g.geometry.triangles.indexData.deviceAddress = e->idxBuf.addr + matOffset[m]*3*sizeof(uint32_t);
		geoms.push_back(g);

		VkAccelerationStructureBuildRangeInfoKHR r = {};
		r.primitiveCount = matTriCount[m];
		ranges.push_back(r);
		primCounts.push_back(matTriCount[m]);

		GeomRecord &rec = gRecords[gNumRecords++];
		rec.vtxAddr = e->vtxBuf.addr;
		rec.idxAddr = e->idxBuf.addr + matOffset[m]*3*sizeof(uint32_t);
	}
	e->numRanges = numRanges;
	gRecordsDirty = true;

	// size query
	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build.geometryCount = (uint32_t)geoms.size();
	build.pGeometries = geoms.data();

	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(gVk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&build, primCounts.data(), &sizes);

	if(!BufferCreate(&e->asBuf, sizes.accelerationStructureSize,
	   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)){
		blasEntryDestroy(e);
		return nil;
	}
	GpuBuffer scratch;
	if(!BufferCreate(&scratch, sizes.buildScratchSize,
	   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)){
		blasEntryDestroy(e);
		return nil;
	}
	gFrameScratch.push_back(scratch);

	VkAccelerationStructureCreateInfoKHR asInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	asInfo.buffer = e->asBuf.buf;
	asInfo.size = sizes.accelerationStructureSize;
	asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if(vkCreateAccelerationStructureKHR(gVk.device, &asInfo, nullptr, &e->as) != VK_SUCCESS){
		blasEntryDestroy(e);
		return nil;
	}

	build.dstAccelerationStructure = e->as;
	build.scratchData.deviceAddress = scratch.addr;
	const VkAccelerationStructureBuildRangeInfoKHR *pRanges = ranges.data();
	vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &pRanges);

	VkAccelerationStructureDeviceAddressInfoKHR addrInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
	addrInfo.accelerationStructure = e->as;
	e->asAddr = vkGetAccelerationStructureDeviceAddressKHR(gVk.device, &addrInfo);

	gBuildsThisFrame++;
	gBlasMap[geo] = e;
	return e;
}

GpuBuffer*
BlasRecordBuffer(void)
{
	if(gRecordsDirty || gRecordBuf.buf == nil){
		VkDeviceSize size = gNumRecords ? gNumRecords * sizeof(GeomRecord) : sizeof(GeomRecord);
		BufferEnsure(&gRecordBuf, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		if(gRecordBuf.mapped && gNumRecords)
			memcpy(gRecordBuf.mapped, gRecords, gNumRecords * sizeof(GeomRecord));
		gRecordsDirty = false;
	}
	return &gRecordBuf;
}

int32_t
BlasCount(void)
{
	return (int32_t)gBlasMap.size();
}

}

#endif
