#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>

#include "blas.h"

#include <glad/glad.h>

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

// --- compaction ---------------------------------------------------------------
// builds carry ALLOW_COMPACTION; the frame after a build we read the
// compacted size (fence-proven) and copy into a right-sized AS, retiring
// the fat original through a handle-level deferred list
static VkQueryPool gCompactQueryPool;
static std::vector<rw::Geometry*> gBuiltThisFrame;	// awaiting size query
static std::vector<rw::Geometry*> gPendingCompact;	// query in flight
struct RetiredAs { VkAccelerationStructureKHR as; GpuBuffer buf; int framesLeft; };
static std::vector<RetiredAs> gRetiredAs;
static uint64_t gCompactSavedBytes;

uint32_t
BlasCompactionSavedMB(void)
{
	return (uint32_t)(gCompactSavedBytes >> 20);
}

static GeomRecord gRecords[MAX_RECORDS];
static uint32_t gNumRecords;
static bool gRecordsDirty;
static GpuBuffer gRecordBuf;

static int gBuildsThisFrame;
// per-build scratch buffers; alive until the frame fence proves the builds
// finished, then freed in BlasBeginFrame
static std::vector<GpuBuffer> gFrameScratch;
static std::vector<GpuBuffer> gPrevFrameScratch;

// --- material albedo ----------------------------------------------------------

// mean texture color per rw::Texture, sampled once via GL readback; the GI
// bounce uses this as the hit surface's albedo approximation
static std::unordered_map<rw::Texture*, uint32_t> gTexMeanCache;

static uint32_t
textureMeanColor(rw::Texture *tex)
{
	if(tex == nil || tex->raster == nil)
		return 0xFFFFFFFFu;
	auto it = gTexMeanCache.find(tex);
	if(it != gTexMeanCache.end())
		return it->second;

	uint32_t mean = 0xFFFFFFFFu;
	rw::gl3::Gl3Raster *natras = PLUGINOFFSET(rw::gl3::Gl3Raster, tex->raster, rw::gl3::nativeRasterOffset);
	if(natras && natras->texid){
		GLint prevTex;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
		glBindTexture(GL_TEXTURE_2D, natras->texid);
		GLint w = 0, h = 0, level = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		// use a small mip if present to keep the readback tiny
		GLint levels = 0;
		while((w >> levels) > 16 && (h >> levels) > 16)
			levels++;
		GLint lw = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, levels, GL_TEXTURE_WIDTH, &lw);
		if(lw == 0)
			levels = 0;
		level = levels;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
		if(w > 0 && h > 0 && w <= 512 && h <= 512){
			uint8_t *pixels = (uint8_t*)malloc(w*h*4);
			if(pixels){
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
				if(glGetError() == GL_NO_ERROR){
					uint64_t r = 0, g = 0, b = 0;
					int n = w*h;
					for(int i = 0; i < n; i++){
						r += pixels[i*4+0];
						g += pixels[i*4+1];
						b += pixels[i*4+2];
					}
					mean = (uint32_t)(r/n) | ((uint32_t)(g/n) << 8) |
						((uint32_t)(b/n) << 16) | 0xFF000000u;
				}
				free(pixels);
			}
		}
		glBindTexture(GL_TEXTURE_2D, prevTex);
	}
	gTexMeanCache[tex] = mean;
	return mean;
}

static uint32_t
materialAlbedo(rw::Material *mat)
{
	if(mat == nil)
		return 0xFFFFFFFFu;
	uint32_t tm = textureMeanColor(mat->texture);
	uint32_t r = ((tm & 0xFF) * mat->color.red) / 255;
	uint32_t g = (((tm >> 8) & 0xFF) * mat->color.green) / 255;
	uint32_t b = (((tm >> 16) & 0xFF) * mat->color.blue) / 255;
	return r | (g << 8) | (b << 16) | 0xFF000000u;
}

// --- hit-point texture cache --------------------------------------------------
// small copies of game textures, GL-read once and uploaded into a VK sampled-
// image array; hit shaders sample these for real albedo instead of the mean

enum { TEXCACHE_MAXDIM = 128 };

static GpuImage gCacheImgs[TEXCACHE_MAX];
static uint32_t gNumCacheImgs;
static VkSampler gCacheSampler;
static std::unordered_map<rw::Texture*, uint32_t> gTexSlotCache;

uint32_t
TexCacheCount(void)
{
	return gNumCacheImgs;
}

VkImageView
TexCacheView(uint32_t slot)
{
	return slot < gNumCacheImgs ? gCacheImgs[slot].view : VK_NULL_HANDLE;
}

VkSampler
TexCacheSampler(void)
{
	if(gCacheSampler == VK_NULL_HANDLE){
		VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		si.magFilter = VK_FILTER_LINEAR;
		si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;	// VC textures tile
		si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		vkCreateSampler(gVk.device, &si, nullptr, &gCacheSampler);
	}
	return gCacheSampler;
}

// upload w*h RGBA8 pixels sitting in staging into a fresh cache image; the
// staging buffer joins the frame scratch list (freed once the fence proves
// the copy done). Returns the slot or UINT32_MAX.
static uint32_t
texCacheUpload(GpuBuffer staging, int w, int h, VkCommandBuffer cmd)
{
	GpuImage *img = &gCacheImgs[gNumCacheImgs];
	if(!ImageCreate(img, w, h, VK_FORMAT_R8G8B8A8_UNORM,
	   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)){
		BufferDestroy(&staging);
		return UINT32_MAX;
	}

	VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	bar.srcAccessMask = 0;
	bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.image = img->image;
	bar.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &bar);

	VkBufferImageCopy region = {};
	region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
	vkCmdCopyBufferToImage(cmd, staging.buf, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &bar);

	gFrameScratch.push_back(staging);
	return gNumCacheImgs++;
}

void
TexCacheEnsureDummy(VkCommandBuffer cmd)
{
	if(gNumCacheImgs > 0)
		return;
	GpuBuffer staging;
	if(!BufferCreate(&staging, 4*4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
		return;
	memset(staging.mapped, 0xFF, 4*4);	// 2x2 white
	texCacheUpload(staging, 2, 2, cmd);
}

static uint32_t
textureSlot(rw::Texture *tex, VkCommandBuffer cmd)
{
	if(tex == nil || tex->raster == nil)
		return UINT32_MAX;
	auto it = gTexSlotCache.find(tex);
	if(it != gTexSlotCache.end())
		return it->second;
	if(gNumCacheImgs >= TEXCACHE_MAX){
		static bool warned;
		if(!warned){
			RtgiLog("RTGI: texture cache full (%u)\n", gNumCacheImgs);
			warned = true;
		}
		return UINT32_MAX;
	}

	uint32_t slot = UINT32_MAX;
	rw::gl3::Gl3Raster *natras = PLUGINOFFSET(rw::gl3::Gl3Raster, tex->raster, rw::gl3::nativeRasterOffset);
	if(natras && natras->texid){
		GLint prevTex;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
		glBindTexture(GL_TEXTURE_2D, natras->texid);
		GLint w = 0, h = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		// pick a small mip if there is one
		int level = 0;
		while((w >> level) > TEXCACHE_MAXDIM && (h >> level) > TEXCACHE_MAXDIM)
			level++;
		GLint lw = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &lw);
		if(lw == 0)
			level = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &h);
		if(w > 0 && h > 0 && w <= 512 && h <= 512){
			GpuBuffer staging;
			if(BufferCreate(&staging, (VkDeviceSize)w*h*4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)){
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_UNSIGNED_BYTE, staging.mapped);
				if(glGetError() == GL_NO_ERROR)
					slot = texCacheUpload(staging, w, h, cmd);
				else
					BufferDestroy(&staging);
			}
		}
		glBindTexture(GL_TEXTURE_2D, prevTex);
	}
	gTexSlotCache[tex] = slot;
	return slot;
}

static void
texCacheShutdown(void)
{
	for(uint32_t i = 0; i < gNumCacheImgs; i++)
		ImageDestroy(&gCacheImgs[i]);
	gNumCacheImgs = 0;
	gTexSlotCache.clear();
	if(gCacheSampler){
		vkDestroySampler(gVk.device, gCacheSampler, nullptr);
		gCacheSampler = VK_NULL_HANDLE;
	}
}

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
	BufferDestroy(&e->uvBuf);
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
	for(auto &r : gRetiredAs){
		vkDestroyAccelerationStructureKHR(gVk.device, r.as, nullptr);
		BufferDestroy(&r.buf);
	}
	gRetiredAs.clear();
	gBuiltThisFrame.clear();
	gPendingCompact.clear();
	if(gCompactQueryPool){
		vkDestroyQueryPool(gVk.device, gCompactQueryPool, nullptr);
		gCompactQueryPool = VK_NULL_HANDLE;
	}
	gCompactSavedBytes = 0;
	BufferDestroy(&gRecordBuf);
	for(auto &b : gFrameScratch) BufferDestroy(&b);
	for(auto &b : gPrevFrameScratch) BufferDestroy(&b);
	gFrameScratch.clear();
	gPrevFrameScratch.clear();
	gNumRecords = 0;
	texCacheShutdown();
	gTexMeanCache.clear();
}

void
BlasBeginFrame(VkCommandBuffer cmd)
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
	// fat originals whose compact copies are provably done
	for(size_t i = 0; i < gRetiredAs.size(); ){
		if(--gRetiredAs[i].framesLeft <= 0){
			vkDestroyAccelerationStructureKHR(gVk.device, gRetiredAs[i].as, nullptr);
			BufferDestroy(&gRetiredAs[i].buf);
			gRetiredAs[i] = gRetiredAs.back();
			gRetiredAs.pop_back();
		}else
			i++;
	}

	// compact last frame's builds: their size queries are fence-proven
	if(!gPendingCompact.empty()){
		uint64_t sizes[BUILDS_PER_FRAME];
		if(gPendingCompact.size() <= BUILDS_PER_FRAME &&
		   vkGetQueryPoolResults(gVk.device, gCompactQueryPool, 0, (uint32_t)gPendingCompact.size(),
		   sizeof(sizes), sizes, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS){
			for(size_t i = 0; i < gPendingCompact.size(); i++){
				auto it = gBlasMap.find(gPendingCompact[i]);
				if(it == gBlasMap.end())
					continue;	// streamed out meanwhile
				BlasEntry *e = it->second;
				uint64_t compSize = sizes[i];
				if(compSize == 0 || compSize + 4096 >= e->asBuf.size)
					continue;	// not worth a copy
				GpuBuffer newBuf;
				if(!BufferCreate(&newBuf, compSize,
				   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false))
					continue;
				VkAccelerationStructureCreateInfoKHR asInfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
				asInfo.buffer = newBuf.buf;
				asInfo.size = compSize;
				asInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
				VkAccelerationStructureKHR newAs;
				if(vkCreateAccelerationStructureKHR(gVk.device, &asInfo, nullptr, &newAs) != VK_SUCCESS){
					BufferDestroy(&newBuf);
					continue;
				}
				VkCopyAccelerationStructureInfoKHR copy = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
				copy.src = e->as;
				copy.dst = newAs;
				copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
				vkCmdCopyAccelerationStructureKHR(cmd, &copy);

				gCompactSavedBytes += e->asBuf.size - compSize;
				RetiredAs retired = { e->as, e->asBuf, DEFERRED_FRAMES };
				gRetiredAs.push_back(retired);
				e->as = newAs;
				e->asBuf = newBuf;
				VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
				ai.accelerationStructure = newAs;
				e->asAddr = vkGetAccelerationStructureDeviceAddressKHR(gVk.device, &ai);
			}
		}
		gPendingCompact.clear();
	}
}

void
BlasEndFrame(VkCommandBuffer cmd)
{
	if(gBuiltThisFrame.empty())
		return;
	if(gCompactQueryPool == VK_NULL_HANDLE){
		VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
		qi.queryCount = BUILDS_PER_FRAME;
		if(vkCreateQueryPool(gVk.device, &qi, nullptr, &gCompactQueryPool) != VK_SUCCESS){
			gBuiltThisFrame.clear();
			return;
		}
	}

	// this frame's builds must be complete before their sizes are queried
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0, nullptr);

	vkCmdResetQueryPool(cmd, gCompactQueryPool, 0, BUILDS_PER_FRAME);
	// query index order must match gPendingCompact order exactly
	VkAccelerationStructureKHR handles[BUILDS_PER_FRAME];
	uint32_t n = 0;
	gPendingCompact.clear();
	for(rw::Geometry *geo : gBuiltThisFrame){
		auto it = gBlasMap.find(geo);
		if(it != gBlasMap.end() && n < BUILDS_PER_FRAME){
			handles[n++] = it->second->as;
			gPendingCompact.push_back(geo);
		}
	}
	if(n > 0)
		vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, n, handles,
			VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, gCompactQueryPool, 0);
	gBuiltThisFrame.clear();
}

// --- build --------------------------------------------------------------------

BlasEntry*
BlasGetOrBuild(rw::Geometry *geo, VkCommandBuffer cmd, float emissiveScale)
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

	// texcoords for real-albedo fetches at hit points (optional)
	if(geo->numTexCoordSets > 0 && geo->texCoords[0] &&
	   BufferCreate(&e->uvBuf, geo->numVertices * 2*sizeof(float), inputUsage, true)){
		float *uv = (float*)e->uvBuf.mapped;
		rw::TexCoords *uvsrc = geo->texCoords[0];
		for(int32_t i = 0; i < geo->numVertices; i++){
			uv[i*2+0] = uvsrc[i].u;
			uv[i*2+1] = uvsrc[i].v;
		}
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

	// alpha-tested materials (foliage etc.) build as non-opaque so rays can
	// stochastically pass through them instead of hitting solid quads
	auto materialAlphaTested = [](rw::Material *m) -> bool {
		if(m == nil)
			return false;
		if(m->color.alpha != 255)
			return true;
		if(m->texture && m->texture->raster)
			return PLUGINOFFSET(rw::gl3::Gl3Raster, m->texture->raster, rw::gl3::nativeRasterOffset)->hasAlpha;
		return false;
	};

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
		rw::Material *rangeMat = m < geo->matList.numMaterials ? geo->matList.materials[m] : nil;
		g.flags = materialAlphaTested(rangeMat) ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
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
		rec.uvAddr = e->uvBuf.buf ? e->uvBuf.addr : 0;
		rec.albedo = materialAlbedo(m < geo->matList.numMaterials ? geo->matList.materials[m] : nil);
		rw::Material *recMat = m < geo->matList.numMaterials ? geo->matList.materials[m] : nil;
		rec.texSlot = (rec.uvAddr && recMat) ? textureSlot(recMat->texture, cmd) : UINT32_MAX;
		rec.matColor = recMat ?
			((uint32_t)recMat->color.red | ((uint32_t)recMat->color.green << 8) |
			 ((uint32_t)recMat->color.blue << 16)) : 0xFFFFFFu;
		// night-model materials emit their own (mean) color, but only the
		// bright ones (neon tubes, lit windows) — large dim facade surfaces
		// of night meshes must not become area lights, and emission scales
		// with luminance so the brightest signs dominate
		rec.emissive = 0;
		if(emissiveScale > 0.0f){
			uint32_t a = rec.albedo;
			float lr = (a & 0xFF)/255.0f, lg = ((a >> 8) & 0xFF)/255.0f, lb = ((a >> 16) & 0xFF)/255.0f;
			float lum = 0.299f*lr + 0.587f*lg + 0.114f*lb;
			if(lum > 0.35f){
				float s = emissiveScale * lum;
				uint32_t r = (uint32_t)((a & 0xFF) * s); if(r > 255) r = 255;
				uint32_t g = (uint32_t)(((a >> 8) & 0xFF) * s); if(g > 255) g = 255;
				uint32_t b = (uint32_t)(((a >> 16) & 0xFF) * s); if(b > 255) b = 255;
				rec.emissive = r | (g << 8) | (b << 16);
			}
		}
	}
	e->numRanges = numRanges;
	gRecordsDirty = true;

	// size query
	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
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
	gBuiltThisFrame.push_back(geo);	// compact next frame
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

uint32_t
BlasAllocRecord(const GeomRecord &rec)
{
	if(gNumRecords >= MAX_RECORDS)
		return UINT32_MAX;
	gRecords[gNumRecords] = rec;
	gRecordsDirty = true;
	return gNumRecords++;
}

}

#endif
