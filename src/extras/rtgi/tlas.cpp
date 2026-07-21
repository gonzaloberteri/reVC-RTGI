#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <unordered_set>

#include "tlas.h"
#include "blas.h"
#include "gbuffer.h"

#include "common.h"
#include <rwcore.h>
#include <rpworld.h>
#include "World.h"
#include "Entity.h"
#include "Game.h"
#include "Camera.h"
#include "ModelInfo.h"

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

// --- skinned peds -------------------------------------------------------------
// CPU-skinned into a per-slot buffer each frame, small BLAS rebuilt per frame.
enum {
	MAX_PED_SLOTS = 24,
	PED_MAX_VERTS = 4096,
	PED_MAX_TRIS = 8192,
	PED_RADIUS = 60,
};
struct PedSlot
{
	GpuBuffer vtxBuf;	// host-visible, object-space posed positions
	GpuBuffer idxBuf;
	GpuBuffer asBuf;
	GpuBuffer scratchBuf;
	VkAccelerationStructureKHR as;
	VkDeviceAddress asAddr;
	uint32_t record;	// persistent GeomRecord slot
	bool ready;
};
static PedSlot gPedSlots[MAX_PED_SLOTS];
static int gNumPedSlotsUsed;	// per frame

static bool
pedSlotInit(PedSlot *s)
{
	VkBufferUsageFlags inputUsage =
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if(!BufferCreate(&s->vtxBuf, PED_MAX_VERTS * 3*sizeof(float), inputUsage, true) ||
	   !BufferCreate(&s->idxBuf, PED_MAX_TRIS * 3*sizeof(uint32_t), inputUsage, true))
		return false;

	// worst-case sized BLAS + scratch, reused every frame
	VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	g.geometry.triangles.vertexStride = 3*sizeof(float);
	g.geometry.triangles.maxVertex = PED_MAX_VERTS - 1;
	g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;

	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build.geometryCount = 1;
	build.pGeometries = &g;
	uint32_t maxPrims = PED_MAX_TRIS;
	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	vkGetAccelerationStructureBuildSizesKHR(gVk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		&build, &maxPrims, &sizes);

	if(!BufferCreate(&s->asBuf, sizes.accelerationStructureSize,
	   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false) ||
	   !BufferCreate(&s->scratchBuf, sizes.buildScratchSize,
	   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false))
		return false;

	VkAccelerationStructureCreateInfoKHR asInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
	asInfo.buffer = s->asBuf.buf;
	asInfo.size = sizes.accelerationStructureSize;
	asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	if(vkCreateAccelerationStructureKHR(gVk.device, &asInfo, nullptr, &s->as) != VK_SUCCESS)
		return false;
	VkAccelerationStructureDeviceAddressInfoKHR addrInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
	addrInfo.accelerationStructure = s->as;
	s->asAddr = vkGetAccelerationStructureDeviceAddressKHR(gVk.device, &addrInfo);

	GeomRecord rec;
	rec.vtxAddr = s->vtxBuf.addr;
	rec.idxAddr = s->idxBuf.addr;
	rec.uvAddr = 0;
	rec.albedo = 0xFF707070u;	// generic clothing gray for GI bounces
	rec.emissive = 0;
	rec.texSlot = UINT32_MAX;
	// alpha byte matters: traversal commits non-opaque candidates by
	// matColor alpha — ped BLASes are opaque today, but a 0 here would
	// silently vanish peds from rays if that ever changes
	rec.matColor = 0xFFFFFFFFu;
	s->record = BlasAllocRecord(rec);
	if(s->record == UINT32_MAX)
		return false;

	s->ready = true;
	return true;
}

static void
pedSlotDestroy(PedSlot *s)
{
	if(s->as)
		vkDestroyAccelerationStructureKHR(gVk.device, s->as, nullptr);
	BufferDestroy(&s->vtxBuf);
	BufferDestroy(&s->idxBuf);
	BufferDestroy(&s->asBuf);
	BufferDestroy(&s->scratchBuf);
	memset(s, 0, sizeof(*s));
}

// pose the ped's skinned atomic into the slot's vertex buffer (object space,
// same math as librw's uploadSkinMatrices) and rebuild its BLAS
static bool
pedSkinIntoSlot(rw::Atomic *atomic, PedSlot *s, VkCommandBuffer cmd, uint32_t *numTrisOut)
{
	using namespace rw;

	Geometry *geo = atomic->geometry;
	Skin *skin = Skin::get(geo);
	if(skin == nil || geo->numVertices > PED_MAX_VERTS || geo->numTriangles > PED_MAX_TRIS ||
	   geo->triangles == nil || geo->morphTargets == nil)
		return false;
	HAnimHierarchy *hier = Skin::getHierarchy(atomic);
	if(hier == nil || hier->matrices == nil || skin->numBones != hier->numNodes)
		return false;

	// compose object-space skinning matrices
	static Matrix boneMats[128];
	if(skin->numBones > 128)
		return false;
	Matrix *invMats = (Matrix*)skin->inverseMatrices;
	if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
		for(int32 i = 0; i < hier->numNodes; i++){
			Matrix inv = invMats[i];
			inv.flags = 0;
			Matrix::mult(&boneMats[i], &inv, &hier->matrices[i]);
		}
	}else{
		Matrix invAtmMat, tmp;
		Matrix::invert(&invAtmMat, atomic->getFrame()->getLTM());
		for(int32 i = 0; i < hier->numNodes; i++){
			Matrix inv = invMats[i];
			inv.flags = 0;
			Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
			Matrix::mult(&boneMats[i], &inv, &tmp);
		}
	}

	// skin positions
	float *dst = (float*)s->vtxBuf.mapped;
	V3d *src = geo->morphTargets[0].vertices;
	for(int32 i = 0; i < geo->numVertices; i++){
		V3d p = { 0.0f, 0.0f, 0.0f };
		V3d v = src[i];
		for(int32 w = 0; w < 4; w++){
			float weight = skin->weights[i*4 + w];
			if(weight == 0.0f)
				continue;
			Matrix *m = &boneMats[skin->indices[i*4 + w]];
			p.x += weight * (m->right.x*v.x + m->up.x*v.y + m->at.x*v.z + m->pos.x);
			p.y += weight * (m->right.y*v.x + m->up.y*v.y + m->at.y*v.z + m->pos.y);
			p.z += weight * (m->right.z*v.x + m->up.z*v.y + m->at.z*v.z + m->pos.z);
		}
		dst[i*3+0] = p.x; dst[i*3+1] = p.y; dst[i*3+2] = p.z;
	}

	uint32_t *idx = (uint32_t*)s->idxBuf.mapped;
	for(int32 i = 0; i < geo->numTriangles; i++){
		idx[i*3+0] = geo->triangles[i].v[0];
		idx[i*3+1] = geo->triangles[i].v[1];
		idx[i*3+2] = geo->triangles[i].v[2];
	}
	*numTrisOut = geo->numTriangles;

	// rebuild the BLAS in place
	VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	g.geometry.triangles.vertexData.deviceAddress = s->vtxBuf.addr;
	g.geometry.triangles.vertexStride = 3*sizeof(float);
	g.geometry.triangles.maxVertex = geo->numVertices - 1;
	g.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	g.geometry.triangles.indexData.deviceAddress = s->idxBuf.addr;

	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build.geometryCount = 1;
	build.pGeometries = &g;
	build.dstAccelerationStructure = s->as;
	build.scratchData.deviceAddress = s->scratchBuf.addr;

	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = *numTrisOut;
	const VkAccelerationStructureBuildRangeInfoKHR *pRange = &range;
	vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &pRange);
	return true;
}

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
	for(int i = 0; i < MAX_PED_SLOTS; i++)
		pedSlotDestroy(&gPedSlots[i]);
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
emitAtomic(rw::Atomic *atomic, VkCommandBuffer cmd, uint8_t mask, float emissiveScale = 0.0f)
{
	if(gNumInstances >= MAX_INSTANCES)
		return;
	if((atomic->object.object.flags & rw::Atomic::RENDER) == 0)
		return;
	// LOD shells would put a phantom low-poly box around every vehicle
	// (their distance gate lives in the render callback, not the flag)
	if(AtomicIsVehicleLod(atomic))
		return;
	rw::Geometry *geo = atomic->geometry;
	if(geo == nil)
		return;

	BlasEntry *blas = BlasGetOrBuild(geo, cmd, emissiveScale);
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

// skinned peds: CPU-pose into a slot, rebuild its BLAS, emit an instance
static void
emitPed(CEntity *e, VkCommandBuffer cmd)
{
	if(gNumPedSlotsUsed >= MAX_PED_SLOTS || gNumInstances >= MAX_INSTANCES)
		return;
	CVector d = e->GetPosition() - TheCamera.GetPosition();
	if(d.MagnitudeSqr() > (float)(PED_RADIUS*PED_RADIUS))
		return;
	if(RwObjectGetType(e->m_rwObject) != rpCLUMP)
		return;

	// first skinned atomic of the clump
	rw::Clump *clump = (rw::Clump*)e->m_rwObject;
	rw::Atomic *atomic = nil;
	FORLIST(lnk, clump->atomics){
		rw::Atomic *a = rw::Atomic::fromClump(lnk);
		if((a->object.object.flags & rw::Atomic::RENDER) && a->geometry &&
		   rw::Skin::get(a->geometry)){
			atomic = a;
			break;
		}
	}
	if(atomic == nil)
		return;

	PedSlot *s = &gPedSlots[gNumPedSlotsUsed];
	if(!s->ready && !pedSlotInit(s))
		return;

	uint32_t numTris = 0;
	if(!pedSkinIntoSlot(atomic, s, cmd, &numTris))
		return;
	gNumPedSlotsUsed++;

	VkAccelerationStructureInstanceKHR *inst =
		(VkAccelerationStructureInstanceKHR*)gInstanceBuf.mapped + gNumInstances;
	memset(inst, 0, sizeof(*inst));
	matrixToVk(&inst->transform, atomic->getFrame()->getLTM());
	inst->instanceCustomIndex = s->record;
	inst->mask = MASK_VEHICLES;	// dynamic casters: RT shadows, GI, reflections
	inst->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	inst->accelerationStructureReference = s->asAddr;
	gNumInstances++;
}

static void
emitEntity(CEntity *e, VkCommandBuffer cmd)
{
	if(e->m_rwObject == nil || !e->bIsVisible)
		return;
	if(!IsAreaVisible(e->m_area))
		return;	// interiors: only the current area traces
	if(HideMirrorWorld(e))
		return;	// fake floor-reflection copies must not pollute rays
	if(e->IsPed()){
		emitPed(e, cmd);
		return;
	}

	// vehicles get their own visibility mask so shadow rays can hit only
	// them (RT replacement for the blob shadows, without double-shadowing
	// the world's baked lighting)
	uint8_t mask = e->IsVehicle() ? MASK_VEHICLES : MASK_STATIC;

	// night-timed models (lit windows, neon) emit their material color into
	// the GI bounce; they only enter the TLAS while the game has them
	// visible, so no hour check is needed here
	float emissiveScale = 0.0f;
	CBaseModelInfo *mi = CModelInfo::GetModelInfo(e->GetModelIndex());
	if(mi && mi->GetModelType() == MITYPE_TIME){
		CTimeModelInfo *tmi = (CTimeModelInfo*)mi;
		if(tmi->GetTimeOn() > tmi->GetTimeOff())	// on-window spans midnight
			emissiveScale = 1.0f;
	}

	if(RwObjectGetType(e->m_rwObject) == rpATOMIC)
		emitAtomic((rw::Atomic*)e->m_rwObject, cmd, mask, emissiveScale);
	else{
		rw::Clump *clump = (rw::Clump*)e->m_rwObject;
		FORLIST(lnk, clump->atomics)
			emitAtomic(rw::Atomic::fromClump(lnk), cmd, mask, emissiveScale);
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
	gNumPedSlotsUsed = 0;

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
		ENTITYLIST_PEDS, ENTITYLIST_PEDS_OVERLAP,
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
