#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <unordered_set>

#include "tlas.h"
#include "blas.h"

#include "common.h"
#include <rwcore.h>
#include <rpworld.h>
#include "World.h"
#include "Entity.h"
#include "Game.h"
#include "Camera.h"

namespace RayTracedGI {

enum {
	MAX_INSTANCES = 24576,
	COLLECT_RADIUS = 300,	// units around the camera; big buildings are global
};

static GpuBuffer gInstanceBuf;	// host-visible VkAccelerationStructureInstanceKHR[]
static GpuBuffer gTlasBuf;
static GpuBuffer gTlasScratch;
static VkAccelerationStructureKHR gTlas;
static uint32_t gNumInstances;

bool
TlasInit(void)
{
	return BufferCreate(&gInstanceBuf, MAX_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
}

void
TlasShutdown(void)
{
	if(gTlas)
		vkDestroyAccelerationStructureKHR(gVk.device, gTlas, nullptr);
	gTlas = VK_NULL_HANDLE;
	BufferDestroy(&gInstanceBuf);
	BufferDestroy(&gTlasBuf);
	BufferDestroy(&gTlasScratch);
}

// rw::Matrix (column vectors right/up/at/pos) -> row-major 3x4
static void
matrixToVk(VkTransformMatrixKHR *dst, rw::Matrix *m)
{
	dst->matrix[0][0] = m->right.x; dst->matrix[0][1] = m->up.x; dst->matrix[0][2] = m->at.x; dst->matrix[0][3] = m->pos.x;
	dst->matrix[1][0] = m->right.y; dst->matrix[1][1] = m->up.y; dst->matrix[1][2] = m->at.y; dst->matrix[1][3] = m->pos.y;
	dst->matrix[2][0] = m->right.z; dst->matrix[2][1] = m->up.z; dst->matrix[2][2] = m->at.z; dst->matrix[2][3] = m->pos.z;
}

static void
emitAtomic(rw::Atomic *atomic, VkCommandBuffer cmd, uint8_t mask)
{
	if(gNumInstances >= MAX_INSTANCES)
		return;
	if((atomic->object.object.flags & rw::Atomic::RENDER) == 0)
		return;
	rw::Geometry *geo = atomic->geometry;
	if(geo == nil)
		return;

	BlasEntry *blas = BlasGetOrBuild(geo, cmd);
	if(blas == nil)
		return;	// over build budget this frame; entity pops in later

	VkAccelerationStructureInstanceKHR *inst =
		(VkAccelerationStructureInstanceKHR*)gInstanceBuf.mapped + gNumInstances;
	memset(inst, 0, sizeof(*inst));
	matrixToVk(&inst->transform, atomic->getFrame()->getLTM());
	inst->instanceCustomIndex = blas->firstRecord;
	inst->mask = mask;
	inst->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	inst->accelerationStructureReference = blas->asAddr;
	gNumInstances++;
}

static void
emitEntity(CEntity *e, VkCommandBuffer cmd)
{
	if(e->m_rwObject == nil || !e->bIsVisible)
		return;
	if(!IsAreaVisible(e->m_area))
		return;	// interiors: only the current area traces
	if(e->IsPed())
		return;	// GPU-skinned; excluded until M9

	// vehicles get their own visibility mask so shadow rays can hit only
	// them (RT replacement for the blob shadows, without double-shadowing
	// the world's baked lighting)
	uint8_t mask = e->IsVehicle() ? MASK_VEHICLES : MASK_STATIC;

	if(RwObjectGetType(e->m_rwObject) == rpATOMIC)
		emitAtomic((rw::Atomic*)e->m_rwObject, cmd, mask);
	else{
		rw::Clump *clump = (rw::Clump*)e->m_rwObject;
		FORLIST(lnk, clump->atomics)
			emitAtomic(rw::Atomic::fromClump(lnk), cmd, mask);
	}
}

static void
collectList(CPtrList &list, std::unordered_set<CEntity*> &seen, VkCommandBuffer cmd)
{
	for(CPtrNode *node = list.first; node; node = node->next){
		CEntity *e = (CEntity*)node->item;
		if(seen.insert(e).second)
			emitEntity(e, cmd);
	}
}

void
TlasCollect(VkCommandBuffer cmd)
{
	gNumInstances = 0;

	static std::unordered_set<CEntity*> seen;
	seen.clear();

	CVector camPos = TheCamera.GetPosition();
	int x0 = CWorld::GetSectorIndexX(camPos.x - COLLECT_RADIUS);
	int x1 = CWorld::GetSectorIndexX(camPos.x + COLLECT_RADIUS);
	int y0 = CWorld::GetSectorIndexY(camPos.y - COLLECT_RADIUS);
	int y1 = CWorld::GetSectorIndexY(camPos.y + COLLECT_RADIUS);
	if(x0 < 0) x0 = 0;
	if(y0 < 0) y0 = 0;
	if(x1 >= NUMSECTORS_X) x1 = NUMSECTORS_X-1;
	if(y1 >= NUMSECTORS_Y) y1 = NUMSECTORS_Y-1;

	static const int lists[] = {
		ENTITYLIST_BUILDINGS, ENTITYLIST_BUILDINGS_OVERLAP,
		ENTITYLIST_OBJECTS, ENTITYLIST_OBJECTS_OVERLAP,
		ENTITYLIST_VEHICLES, ENTITYLIST_VEHICLES_OVERLAP,
		ENTITYLIST_DUMMIES, ENTITYLIST_DUMMIES_OVERLAP,
	};

	for(int y = y0; y <= y1; y++)
		for(int x = x0; x <= x1; x++){
			CSector *sector = CWorld::GetSector(x, y);
			for(size_t i = 0; i < sizeof(lists)/sizeof(lists[0]); i++)
				collectList(sector->m_lists[lists[i]], seen, cmd);
		}

	// island LODs / big buildings: global, they are the far scene
	for(int level = 0; level < NUM_LEVELS; level++)
		collectList(CWorld::GetBigBuildingList((eLevelName)level), seen, cmd);
}

bool
TlasBuild(VkCommandBuffer cmd)
{
	if(gNumInstances == 0)
		return false;

	// BLAS builds must finish before the TLAS consumes them
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);

	VkAccelerationStructureGeometryKHR geom = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.data.deviceAddress = gInstanceBuf.addr;

	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build.geometryCount = 1;
	build.pGeometries = &geom;

	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(gVk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&build, &gNumInstances, &sizes);

	// TLAS result/scratch grow rarely; the fence at frame start means the
	// old ones are idle when BufferEnsure swaps them
	if(gTlasBuf.size < sizes.accelerationStructureSize && gTlas){
		vkDestroyAccelerationStructureKHR(gVk.device, gTlas, nullptr);
		gTlas = VK_NULL_HANDLE;
	}
	if(!BufferEnsure(&gTlasBuf, sizes.accelerationStructureSize,
	   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false))
		return false;
	if(!BufferEnsure(&gTlasScratch, sizes.buildScratchSize,
	   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false))
		return false;

	if(gTlas == VK_NULL_HANDLE){
		VkAccelerationStructureCreateInfoKHR asInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
		asInfo.buffer = gTlasBuf.buf;
		asInfo.size = sizes.accelerationStructureSize;
		asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		if(vkCreateAccelerationStructureKHR(gVk.device, &asInfo, nullptr, &gTlas) != VK_SUCCESS)
			return false;
	}

	build.dstAccelerationStructure = gTlas;
	build.scratchData.deviceAddress = gTlasScratch.addr;

	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = gNumInstances;
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &pRange);

	// TLAS ready before the ray pass reads it
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	return true;
}

VkAccelerationStructureKHR
TlasHandle(void)
{
	return gTlas;
}

uint32_t
TlasInstanceCount(void)
{
	return gNumInstances;
}

}

#endif
