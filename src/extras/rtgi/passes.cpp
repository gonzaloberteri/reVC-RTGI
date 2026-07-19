#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "passes.h"
#include "interop.h"
#include "tlas.h"
#include "blas.h"

#include "common.h"
#include <rwcore.h>
#include "main.h"
#include "Timecycle.h"
#include "PointLights.h"
#include "Weather.h"
#include "Pools.h"
#include "Vehicle.h"
#include "Camera.h"
#include "rtgi.h"

#include "shaders/obj/primary_comp.inc"
#include "shaders/obj/ao_comp.inc"
#include "shaders/obj/gi_comp.inc"
#include "shaders/obj/temporal_comp.inc"
#include "shaders/obj/atrous_comp.inc"
#include "shaders/obj/refl_comp.inc"

namespace RayTracedGI {

struct PushConstants
{
	float camPos[4];
	float camRight[4];	// w = viewWindow.x
	float camUp[4];		// w = viewWindow.y
	float camFwd[4];
	uint32_t size[2];
	uint32_t mode;
	uint32_t frame;
};

struct AoPushConstants
{
	float camPos[4];
	float camRight[4];
	float camUp[4];
	float camFwd[4];
	float sunDir[4];	// w > 0 = sun up
	uint32_t size[2];
	uint32_t frame;
	uint32_t numRays;
	float aoRadius;
	float pad[3];
};

static VkDescriptorSetLayout gSetLayout;
static VkPipelineLayout gPipeLayout;
static VkPipeline gPipeline;
static VkDescriptorPool gDescPool;
static VkDescriptorSet gDescSet;
static VkImageView gOutView;

static VkDescriptorSetLayout gAoSetLayout;
static VkPipelineLayout gAoPipeLayout;
static VkPipeline gAoPipeline;
static VkDescriptorSet gAoDescSet;

// GI + temporal accumulation
struct GiPushConstants
{
	float camPos[4];
	float camRight[4];
	float camUp[4];
	float camFwd[4];
	float sunDir[4];
	float sunColor[4];
	float skyTop[4];
	float skyBottom[4];
	uint32_t size[2];
	uint32_t frame;
	uint32_t pad0;
};

struct TemporalPushConstants
{
	float camPos[4];
	float camRight[4];
	float camUp[4];
	float camFwd[4];
	float prevCamPos[4];
	float prevCamRight[4];
	float prevCamUp[4];
	float prevCamFwd[4];
	uint32_t size[2];
	uint32_t frame;
	uint32_t reset;
};

static VkDescriptorSetLayout gGiSetLayout;
static VkPipelineLayout gGiPipeLayout;
static VkPipeline gGiPipeline;
static VkDescriptorSet gGiDescSet;

static VkDescriptorSetLayout gTemporalSetLayout;
static VkPipelineLayout gTemporalPipeLayout;
static VkPipeline gTemporalPipeline;
static VkDescriptorSet gTemporalDescSet;

struct AtrousPushConstants
{
	uint32_t size[2];
	int32_t step;
	float pad;
};

static VkDescriptorSetLayout gAtrousSetLayout;
static VkPipelineLayout gAtrousPipeLayout;
static VkPipeline gAtrousPipeline;
static VkDescriptorSet gAtrousDescSets[3];	// one per iteration

static GpuImage gGiRaw;
static GpuImage gGiAccum[2];
static GpuImage gDepthHist[2];
static GpuImage gAtrousScratch;

// game point lights snapshot for the GI pass
struct GpuPointLight
{
	float posRadius[4];	// xyz, w = radius
	float color[4];		// rgb, w = spot cos cutoff (0 = omni)
	float dir[4];		// xyz spot direction
};
// game CPointLights (32) + vehicle headlight cones
enum { MAX_GI_LIGHTS = 96 };
static GpuBuffer gLightBuf;
static uint32_t gLastNumLights;
static uint32_t gLastNumHeadlights;

uint32_t
GiLightCount(void)
{
	return gLastNumLights;
}

uint32_t
GiHeadlightCount(void)
{
	return gLastNumHeadlights;
}

// reflections
struct ReflPushConstants
{
	float camPos[4];
	float camRight[4];
	float camUp[4];
	float camFwd[4];
	float sunDir[4];
	float sunColor[4];
	float skyTop[4];
	float skyBottom[4];
	uint32_t size[2];
	uint32_t frame;
	float wetness;
};
static VkDescriptorSetLayout gReflSetLayout;
static VkPipelineLayout gReflPipeLayout;
static VkPipeline gReflPipeline;
static VkDescriptorSet gReflDescSet;
static int gAccumIndex;
static bool gGiImagesInitialised;	// UNDEFINED->GENERAL done
static float gPrevCam[16];		// pos/right/up/fwd with vw in w
static bool gHavePrevCam;

// image infos for the texture-cache array binding; slots beyond the cached
// count alias slot 0 (the white dummy) so every descriptor stays valid
static VkDescriptorImageInfo*
texCacheInfos(void)
{
	static VkDescriptorImageInfo infos[TEXCACHE_MAX];
	uint32_t n = TexCacheCount();
	VkSampler sampler = TexCacheSampler();
	for(uint32_t i = 0; i < TEXCACHE_MAX; i++){
		infos[i].sampler = sampler;
		infos[i].imageView = TexCacheView(i < n ? i : 0);
		infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
	return infos;
}

static VkPipeline
createComputePipeline(const uint32_t *code, size_t codeSize, VkPipelineLayout layout)
{
	VkShaderModuleCreateInfo smInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	smInfo.codeSize = codeSize;
	smInfo.pCode = code;
	VkShaderModule module;
	if(vkCreateShaderModule(gVk.device, &smInfo, nullptr, &module) != VK_SUCCESS)
		return VK_NULL_HANDLE;

	VkComputePipelineCreateInfo cpInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpInfo.stage.module = module;
	cpInfo.stage.pName = "main";
	cpInfo.layout = layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkResult res = vkCreateComputePipelines(gVk.device, VK_NULL_HANDLE, 1, &cpInfo, nullptr, &pipeline);
	vkDestroyShaderModule(gVk.device, module, nullptr);
	return res == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

bool
PassesInit(void)
{
	VkDescriptorSetLayoutBinding bindings[3] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 3;
	layoutInfo.pBindings = bindings;
	if(vkCreateDescriptorSetLayout(gVk.device, &layoutInfo, nullptr, &gSetLayout) != VK_SUCCESS)
		return false;

	VkPushConstantRange pcRange = {};
	pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcRange.size = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo plInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = &gSetLayout;
	plInfo.pushConstantRangeCount = 1;
	plInfo.pPushConstantRanges = &pcRange;
	if(vkCreatePipelineLayout(gVk.device, &plInfo, nullptr, &gPipeLayout) != VK_SUCCESS)
		return false;

	VkShaderModuleCreateInfo smInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	smInfo.codeSize = sizeof(primary_comp_spv);
	smInfo.pCode = primary_comp_spv;
	VkShaderModule module;
	if(vkCreateShaderModule(gVk.device, &smInfo, nullptr, &module) != VK_SUCCESS)
		return false;

	VkComputePipelineCreateInfo cpInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	cpInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpInfo.stage.module = module;
	cpInfo.stage.pName = "main";
	cpInfo.layout = gPipeLayout;
	VkResult res = vkCreateComputePipelines(gVk.device, VK_NULL_HANDLE, 1, &cpInfo, nullptr, &gPipeline);
	vkDestroyShaderModule(gVk.device, module, nullptr);
	if(res != VK_SUCCESS){
		RtgiLog("RTGI: compute pipeline creation failed\n");
		return false;
	}

	VkDescriptorPoolSize poolSizes[4] = {
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 8 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 24 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 },
		// + the two texture-cache arrays (GI + reflections)
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 + 2*TEXCACHE_MAX },
	};
	VkDescriptorPoolCreateInfo dpInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpInfo.maxSets = 12;
	dpInfo.poolSizeCount = 4;
	dpInfo.pPoolSizes = poolSizes;
	if(vkCreateDescriptorPool(gVk.device, &dpInfo, nullptr, &gDescPool) != VK_SUCCESS)
		return false;

	VkDescriptorSetAllocateInfo dsInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsInfo.descriptorPool = gDescPool;
	dsInfo.descriptorSetCount = 1;
	dsInfo.pSetLayouts = &gSetLayout;
	if(vkAllocateDescriptorSets(gVk.device, &dsInfo, &gDescSet) != VK_SUCCESS)
		return false;

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = gInterop.rtOutput.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if(vkCreateImageView(gVk.device, &viewInfo, nullptr, &gOutView) != VK_SUCCESS)
		return false;

	// --- AO pass ---------------------------------------------------------

	VkDescriptorSetLayoutBinding aoBindings[4] = {};
	aoBindings[0].binding = 0;
	aoBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	aoBindings[0].descriptorCount = 1;
	aoBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	aoBindings[1].binding = 1;
	aoBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	aoBindings[1].descriptorCount = 1;
	aoBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	aoBindings[2].binding = 2;
	aoBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	aoBindings[2].descriptorCount = 1;
	aoBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	aoBindings[3].binding = 3;
	aoBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	aoBindings[3].descriptorCount = 1;
	aoBindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo aoLayoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	aoLayoutInfo.bindingCount = 4;
	aoLayoutInfo.pBindings = aoBindings;
	if(vkCreateDescriptorSetLayout(gVk.device, &aoLayoutInfo, nullptr, &gAoSetLayout) != VK_SUCCESS)
		return false;

	VkPushConstantRange aoPcRange = {};
	aoPcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	aoPcRange.size = sizeof(AoPushConstants);
	VkPipelineLayoutCreateInfo aoPlInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	aoPlInfo.setLayoutCount = 1;
	aoPlInfo.pSetLayouts = &gAoSetLayout;
	aoPlInfo.pushConstantRangeCount = 1;
	aoPlInfo.pPushConstantRanges = &aoPcRange;
	if(vkCreatePipelineLayout(gVk.device, &aoPlInfo, nullptr, &gAoPipeLayout) != VK_SUCCESS)
		return false;

	gAoPipeline = createComputePipeline(ao_comp_spv, sizeof(ao_comp_spv), gAoPipeLayout);
	if(gAoPipeline == VK_NULL_HANDLE){
		RtgiLog("RTGI: AO pipeline creation failed\n");
		return false;
	}

	VkDescriptorSetAllocateInfo aoDsInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	aoDsInfo.descriptorPool = gDescPool;
	aoDsInfo.descriptorSetCount = 1;
	aoDsInfo.pSetLayouts = &gAoSetLayout;
	if(vkAllocateDescriptorSets(gVk.device, &aoDsInfo, &gAoDescSet) != VK_SUCCESS)
		return false;

	// --- GI pass ---------------------------------------------------------

	{
	VkDescriptorSetLayoutBinding b[7] = {};
	VkDescriptorType types[7] = {
		VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// texture cache array
	};
	for(int i = 0; i < 7; i++){
		b[i].binding = i;
		b[i].descriptorType = types[i];
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	b[6].descriptorCount = TEXCACHE_MAX;
	VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	li.bindingCount = 7;
	li.pBindings = b;
	if(vkCreateDescriptorSetLayout(gVk.device, &li, nullptr, &gGiSetLayout) != VK_SUCCESS)
		return false;

	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.size = sizeof(GiPushConstants);
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &gGiSetLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if(vkCreatePipelineLayout(gVk.device, &pli, nullptr, &gGiPipeLayout) != VK_SUCCESS)
		return false;
	gGiPipeline = createComputePipeline(gi_comp_spv, sizeof(gi_comp_spv), gGiPipeLayout);
	if(gGiPipeline == VK_NULL_HANDLE)
		return false;
	VkDescriptorSetAllocateInfo dsi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsi.descriptorPool = gDescPool;
	dsi.descriptorSetCount = 1;
	dsi.pSetLayouts = &gGiSetLayout;
	if(vkAllocateDescriptorSets(gVk.device, &dsi, &gGiDescSet) != VK_SUCCESS)
		return false;
	}

	// --- temporal pass ---------------------------------------------------

	{
	VkDescriptorSetLayoutBinding b[7] = {};
	VkDescriptorType types[7] = {
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		// giRaw
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// prevAccum
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// prevDepth
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// gbDepth
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		// outAccum
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		// outShared (GL)
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		// outPrevDepth
	};
	for(int i = 0; i < 7; i++){
		b[i].binding = i;
		b[i].descriptorType = types[i];
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	li.bindingCount = 7;
	li.pBindings = b;
	if(vkCreateDescriptorSetLayout(gVk.device, &li, nullptr, &gTemporalSetLayout) != VK_SUCCESS)
		return false;

	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.size = sizeof(TemporalPushConstants);
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &gTemporalSetLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if(vkCreatePipelineLayout(gVk.device, &pli, nullptr, &gTemporalPipeLayout) != VK_SUCCESS)
		return false;
	gTemporalPipeline = createComputePipeline(temporal_comp_spv, sizeof(temporal_comp_spv), gTemporalPipeLayout);
	if(gTemporalPipeline == VK_NULL_HANDLE)
		return false;
	VkDescriptorSetAllocateInfo dsi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsi.descriptorPool = gDescPool;
	dsi.descriptorSetCount = 1;
	dsi.pSetLayouts = &gTemporalSetLayout;
	if(vkAllocateDescriptorSets(gVk.device, &dsi, &gTemporalDescSet) != VK_SUCCESS)
		return false;
	}

	// --- reflections -----------------------------------------------------

	{
	VkDescriptorSetLayoutBinding b[6] = {};
	VkDescriptorType types[6] = {
		VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// texture cache array
	};
	for(int i = 0; i < 6; i++){
		b[i].binding = i;
		b[i].descriptorType = types[i];
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	b[5].descriptorCount = TEXCACHE_MAX;
	VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	li.bindingCount = 6;
	li.pBindings = b;
	if(vkCreateDescriptorSetLayout(gVk.device, &li, nullptr, &gReflSetLayout) != VK_SUCCESS)
		return false;
	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.size = sizeof(ReflPushConstants);
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &gReflSetLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if(vkCreatePipelineLayout(gVk.device, &pli, nullptr, &gReflPipeLayout) != VK_SUCCESS)
		return false;
	gReflPipeline = createComputePipeline(refl_comp_spv, sizeof(refl_comp_spv), gReflPipeLayout);
	if(gReflPipeline == VK_NULL_HANDLE)
		return false;
	VkDescriptorSetAllocateInfo dsi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsi.descriptorPool = gDescPool;
	dsi.descriptorSetCount = 1;
	dsi.pSetLayouts = &gReflSetLayout;
	if(vkAllocateDescriptorSets(gVk.device, &dsi, &gReflDescSet) != VK_SUCCESS)
		return false;
	}

	int w = gInterop.giOutput.width, h = gInterop.giOutput.height;
	VkImageUsageFlags giUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if(!ImageCreate(&gGiRaw, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, giUsage) ||
	   !ImageCreate(&gGiAccum[0], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, giUsage) ||
	   !ImageCreate(&gGiAccum[1], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, giUsage) ||
	   !ImageCreate(&gDepthHist[0], w, h, VK_FORMAT_R32_SFLOAT, giUsage) ||
	   !ImageCreate(&gDepthHist[1], w, h, VK_FORMAT_R32_SFLOAT, giUsage) ||
	   !ImageCreate(&gAtrousScratch, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, giUsage))
		return false;

	// --- a-trous denoiser ------------------------------------------------

	{
	VkDescriptorSetLayoutBinding b[4] = {};
	VkDescriptorType types[4] = {
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// in
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,		// out
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// normal
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,	// depth
	};
	for(int i = 0; i < 4; i++){
		b[i].binding = i;
		b[i].descriptorType = types[i];
		b[i].descriptorCount = 1;
		b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	li.bindingCount = 4;
	li.pBindings = b;
	if(vkCreateDescriptorSetLayout(gVk.device, &li, nullptr, &gAtrousSetLayout) != VK_SUCCESS)
		return false;
	VkPushConstantRange pcr = {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.size = sizeof(AtrousPushConstants);
	VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &gAtrousSetLayout;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if(vkCreatePipelineLayout(gVk.device, &pli, nullptr, &gAtrousPipeLayout) != VK_SUCCESS)
		return false;
	gAtrousPipeline = createComputePipeline(atrous_comp_spv, sizeof(atrous_comp_spv), gAtrousPipeLayout);
	if(gAtrousPipeline == VK_NULL_HANDLE)
		return false;
	VkDescriptorSetLayout layouts[3] = { gAtrousSetLayout, gAtrousSetLayout, gAtrousSetLayout };
	VkDescriptorSetAllocateInfo dsi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	dsi.descriptorPool = gDescPool;
	dsi.descriptorSetCount = 3;
	dsi.pSetLayouts = layouts;
	if(vkAllocateDescriptorSets(gVk.device, &dsi, gAtrousDescSets) != VK_SUCCESS)
		return false;
	}

	return true;
}

void
PassesShutdown(void)
{
	BufferDestroy(&gLightBuf);
	if(gReflPipeline) vkDestroyPipeline(gVk.device, gReflPipeline, nullptr);
	if(gReflPipeLayout) vkDestroyPipelineLayout(gVk.device, gReflPipeLayout, nullptr);
	if(gReflSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gReflSetLayout, nullptr);
	gReflPipeline = VK_NULL_HANDLE;
	gReflPipeLayout = VK_NULL_HANDLE;
	gReflSetLayout = VK_NULL_HANDLE;
	ImageDestroy(&gAtrousScratch);
	if(gAtrousPipeline) vkDestroyPipeline(gVk.device, gAtrousPipeline, nullptr);
	if(gAtrousPipeLayout) vkDestroyPipelineLayout(gVk.device, gAtrousPipeLayout, nullptr);
	if(gAtrousSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gAtrousSetLayout, nullptr);
	gAtrousPipeline = VK_NULL_HANDLE;
	gAtrousPipeLayout = VK_NULL_HANDLE;
	gAtrousSetLayout = VK_NULL_HANDLE;
	ImageDestroy(&gGiRaw);
	ImageDestroy(&gGiAccum[0]);
	ImageDestroy(&gGiAccum[1]);
	ImageDestroy(&gDepthHist[0]);
	ImageDestroy(&gDepthHist[1]);
	if(gGiPipeline) vkDestroyPipeline(gVk.device, gGiPipeline, nullptr);
	if(gGiPipeLayout) vkDestroyPipelineLayout(gVk.device, gGiPipeLayout, nullptr);
	if(gGiSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gGiSetLayout, nullptr);
	if(gTemporalPipeline) vkDestroyPipeline(gVk.device, gTemporalPipeline, nullptr);
	if(gTemporalPipeLayout) vkDestroyPipelineLayout(gVk.device, gTemporalPipeLayout, nullptr);
	if(gTemporalSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gTemporalSetLayout, nullptr);
	gGiPipeline = VK_NULL_HANDLE;
	gGiPipeLayout = VK_NULL_HANDLE;
	gGiSetLayout = VK_NULL_HANDLE;
	gTemporalPipeline = VK_NULL_HANDLE;
	gTemporalPipeLayout = VK_NULL_HANDLE;
	gTemporalSetLayout = VK_NULL_HANDLE;
	gGiImagesInitialised = false;
	gHavePrevCam = false;
	if(gAoPipeline) vkDestroyPipeline(gVk.device, gAoPipeline, nullptr);
	if(gAoPipeLayout) vkDestroyPipelineLayout(gVk.device, gAoPipeLayout, nullptr);
	if(gAoSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gAoSetLayout, nullptr);
	if(gOutView) vkDestroyImageView(gVk.device, gOutView, nullptr);
	if(gDescPool) vkDestroyDescriptorPool(gVk.device, gDescPool, nullptr);
	if(gPipeline) vkDestroyPipeline(gVk.device, gPipeline, nullptr);
	if(gPipeLayout) vkDestroyPipelineLayout(gVk.device, gPipeLayout, nullptr);
	if(gSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gSetLayout, nullptr);
	gAoPipeline = VK_NULL_HANDLE;
	gAoPipeLayout = VK_NULL_HANDLE;
	gAoSetLayout = VK_NULL_HANDLE;
	gOutView = VK_NULL_HANDLE;
	gDescPool = VK_NULL_HANDLE;
	gPipeline = VK_NULL_HANDLE;
	gPipeLayout = VK_NULL_HANDLE;
	gSetLayout = VK_NULL_HANDLE;
}

void
PassesTracePrimary(VkCommandBuffer cmd, uint32_t mode, uint32_t frame)
{
	// frame fence was waited, so the set is not in flight
	VkAccelerationStructureKHR tlas = TlasHandle();
	VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
	asWrite.accelerationStructureCount = 1;
	asWrite.pAccelerationStructures = &tlas;
	VkDescriptorImageInfo imgInfo = {};
	imgInfo.imageView = gOutView;
	imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	GpuBuffer *records = BlasRecordBuffer();
	VkDescriptorBufferInfo bufInfo = {};
	bufInfo.buffer = records->buf;
	bufInfo.range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet writes[3] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].pNext = &asWrite;
	writes[0].dstSet = gDescSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = gDescSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].pImageInfo = &imgInfo;
	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = gDescSet;
	writes[2].dstBinding = 2;
	writes[2].descriptorCount = 1;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].pBufferInfo = &bufInfo;
	vkUpdateDescriptorSets(gVk.device, 3, writes, 0, nullptr);

	// camera from the RW camera the game renders with
	rw::Camera *cam = (rw::Camera*)Scene.camera;
	rw::Matrix *ltm = ((rw::Frame*)cam->getFrame())->getLTM();

	PushConstants pc = {};
	pc.camPos[0] = ltm->pos.x; pc.camPos[1] = ltm->pos.y; pc.camPos[2] = ltm->pos.z;
	pc.camRight[0] = ltm->right.x; pc.camRight[1] = ltm->right.y; pc.camRight[2] = ltm->right.z;
	pc.camRight[3] = cam->viewWindow.x;
	pc.camUp[0] = ltm->up.x; pc.camUp[1] = ltm->up.y; pc.camUp[2] = ltm->up.z;
	pc.camUp[3] = cam->viewWindow.y;
	pc.camFwd[0] = ltm->at.x; pc.camFwd[1] = ltm->at.y; pc.camFwd[2] = ltm->at.z;
	pc.size[0] = gInterop.rtOutput.width;
	pc.size[1] = gInterop.rtOutput.height;
	pc.mode = mode;
	pc.frame = frame;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gPipeLayout, 0, 1, &gDescSet, 0, nullptr);
	vkCmdPushConstants(cmd, gPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (gInterop.rtOutput.width + 7)/8, (gInterop.rtOutput.height + 7)/8, 1);
}

void
PassesTraceAO(VkCommandBuffer cmd, uint32_t frame, uint32_t numRays, float radius)
{
	VkAccelerationStructureKHR tlas = TlasHandle();
	VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
	asWrite.accelerationStructureCount = 1;
	asWrite.pAccelerationStructures = &tlas;
	VkDescriptorImageInfo aoImgInfo = {};
	aoImgInfo.imageView = gInterop.aoOutput.view;
	aoImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	VkDescriptorImageInfo normalInfo = {};
	normalInfo.sampler = gInterop.sampler;
	normalInfo.imageView = gInterop.gbNormal.view;
	normalInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	VkDescriptorImageInfo depthInfo = normalInfo;
	depthInfo.imageView = gInterop.gbDepth.view;

	VkWriteDescriptorSet writes[4] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].pNext = &asWrite;
	writes[0].dstSet = gAoDescSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = gAoDescSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].pImageInfo = &aoImgInfo;
	writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[2].dstSet = gAoDescSet;
	writes[2].dstBinding = 2;
	writes[2].descriptorCount = 1;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[2].pImageInfo = &normalInfo;
	writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[3].dstSet = gAoDescSet;
	writes[3].dstBinding = 3;
	writes[3].descriptorCount = 1;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[3].pImageInfo = &depthInfo;
	vkUpdateDescriptorSets(gVk.device, 4, writes, 0, nullptr);

	rw::Camera *cam = (rw::Camera*)Scene.camera;
	rw::Matrix *ltm = cam->getFrame()->getLTM();

	AoPushConstants pc = {};
	pc.camPos[0] = ltm->pos.x; pc.camPos[1] = ltm->pos.y; pc.camPos[2] = ltm->pos.z;
	pc.camRight[0] = ltm->right.x; pc.camRight[1] = ltm->right.y; pc.camRight[2] = ltm->right.z;
	pc.camRight[3] = cam->viewWindow.x;
	pc.camUp[0] = ltm->up.x; pc.camUp[1] = ltm->up.y; pc.camUp[2] = ltm->up.z;
	pc.camUp[3] = cam->viewWindow.y;
	pc.camFwd[0] = ltm->at.x; pc.camFwd[1] = ltm->at.y; pc.camFwd[2] = ltm->at.z;
	CVector sunDir = CTimeCycle::GetSunDirection();
	pc.sunDir[0] = sunDir.x; pc.sunDir[1] = sunDir.y; pc.sunDir[2] = sunDir.z;
	pc.sunDir[3] = (sunDir.z > 0.0f && CTimeCycle::GetShadowStrength() > 0) ? 1.0f : 0.0f;
	pc.size[0] = gInterop.aoOutput.width;
	pc.size[1] = gInterop.aoOutput.height;
	pc.frame = frame;
	pc.numRays = numRays;
	pc.aoRadius = radius;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gAoPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gAoPipeLayout, 0, 1, &gAoDescSet, 0, nullptr);
	vkCmdPushConstants(cmd, gAoPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (gInterop.aoOutput.width + 7)/8, (gInterop.aoOutput.height + 7)/8, 1);
}

static void
fillCamera(float pos[4], float right[4], float up[4], float fwd[4])
{
	rw::Camera *cam = (rw::Camera*)Scene.camera;
	rw::Matrix *ltm = cam->getFrame()->getLTM();
	pos[0] = ltm->pos.x; pos[1] = ltm->pos.y; pos[2] = ltm->pos.z; pos[3] = 0.0f;
	right[0] = ltm->right.x; right[1] = ltm->right.y; right[2] = ltm->right.z;
	right[3] = cam->viewWindow.x;
	up[0] = ltm->up.x; up[1] = ltm->up.y; up[2] = ltm->up.z;
	up[3] = cam->viewWindow.y;
	fwd[0] = ltm->at.x; fwd[1] = ltm->at.y; fwd[2] = ltm->at.z; fwd[3] = 0.0f;
}

bool
PassesTraceGI(VkCommandBuffer cmd, uint32_t frame, bool resetHistory)
{
	int w = gInterop.giOutput.width, h = gInterop.giOutput.height;
	int cur = gAccumIndex, prev = 1 - gAccumIndex;

	// first use: everything to GENERAL once
	if(!gGiImagesInitialised){
		VkImageMemoryBarrier barriers[6] = {};
		VkImage images[6] = { gGiRaw.image, gGiAccum[0].image, gGiAccum[1].image,
			gDepthHist[0].image, gDepthHist[1].image, gAtrousScratch.image };
		for(int i = 0; i < 6; i++){
			barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
			barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[i].image = images[i];
			barriers[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		}
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 6, barriers);
		gGiImagesInitialised = true;
		resetHistory = true;
	}

	// point light snapshot for this frame
	uint32_t numLights = 0;
	{
		if(gLightBuf.buf == nil)
			BufferCreate(&gLightBuf, MAX_GI_LIGHTS * sizeof(GpuPointLight),
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		GpuPointLight *dst = (GpuPointLight*)gLightBuf.mapped;
		if(dst){
			// game point lights; skip LIGHT_DIRECTIONAL (the player's
			// headlight) — proper cones are added for all vehicles below
			for(int i = 0; i < CPointLights::NumLights && numLights < MAX_GI_LIGHTS; i++){
				CRegisteredPointLight *l = &CPointLights::aLights[i];
				if(l->type != CPointLights::LIGHT_POINT)
					continue;
				GpuPointLight *g = &dst[numLights++];
				g->posRadius[0] = l->coors.x;
				g->posRadius[1] = l->coors.y;
				g->posRadius[2] = l->coors.z;
				g->posRadius[3] = l->radius;
				g->color[0] = l->red;
				g->color[1] = l->green;
				g->color[2] = l->blue;
				g->color[3] = 0.0f;
				g->dir[0] = g->dir[1] = g->dir[2] = g->dir[3] = 0.0f;
			}

			// headlight cones for every nearby vehicle with lights on
			gLastNumHeadlights = 0;
			CVector camPos = TheCamera.GetPosition();
			CVehiclePool *pool = CPools::GetVehiclePool();
			for(int i = 0; pool && i < pool->GetSize() && numLights < MAX_GI_LIGHTS; i++){
				CVehicle *veh = pool->GetSlot(i);
				if(veh == nil || !veh->bLightsOn || veh->m_rwObject == nil)
					continue;
				CVector d = veh->GetPosition() - camPos;
				if(d.MagnitudeSqr() > sq(90.0f))
					continue;
				gLastNumHeadlights++;
				CVector fwd = veh->GetForward();
				CVector pos = veh->GetPosition() + fwd*2.2f;
				// aim slightly down so the beam pools on the road ahead
				CVector dir = fwd - veh->GetUp()*0.25f;
				dir.Normalise();
				GpuPointLight *g = &dst[numLights++];
				g->posRadius[0] = pos.x;
				g->posRadius[1] = pos.y;
				g->posRadius[2] = pos.z;
				g->posRadius[3] = 22.0f;
				g->color[0] = 1.0f;
				g->color[1] = 0.92f;
				g->color[2] = 0.72f;
				g->color[3] = 0.70f;	// spot cos cutoff (~45 deg cone)
				g->dir[0] = dir.x;
				g->dir[1] = dir.y;
				g->dir[2] = dir.z;
				g->dir[3] = 0.0f;
			}
		}
	}

	// GI descriptors
	{
	VkAccelerationStructureKHR tlas = TlasHandle();
	VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
	asWrite.accelerationStructureCount = 1;
	asWrite.pAccelerationStructures = &tlas;
	VkDescriptorImageInfo rawInfo = { VK_NULL_HANDLE, gGiRaw.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo normalInfo = { gInterop.sampler, gInterop.gbNormal.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo depthInfo = { gInterop.sampler, gInterop.gbDepth.view, VK_IMAGE_LAYOUT_GENERAL };
	GpuBuffer *records = BlasRecordBuffer();
	VkDescriptorBufferInfo bufInfo = { records->buf, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo lightInfo = { gLightBuf.buf, 0, VK_WHOLE_SIZE };

	VkWriteDescriptorSet writes[7] = {};
	for(int i = 0; i < 7; i++){
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = gGiDescSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[0].pNext = &asWrite;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].pImageInfo = &rawInfo;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[2].pImageInfo = &normalInfo;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[3].pImageInfo = &depthInfo;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[4].pBufferInfo = &bufInfo;
	writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[5].pBufferInfo = &lightInfo;
	writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[6].descriptorCount = TEXCACHE_MAX;
	writes[6].pImageInfo = texCacheInfos();
	vkUpdateDescriptorSets(gVk.device, 7, writes, 0, nullptr);
	}

	GiPushConstants gpc = {};
	fillCamera(gpc.camPos, gpc.camRight, gpc.camUp, gpc.camFwd);
	CVector sunDir = CTimeCycle::GetSunDirection();
	gpc.sunDir[0] = sunDir.x; gpc.sunDir[1] = sunDir.y; gpc.sunDir[2] = sunDir.z;
	gpc.sunDir[3] = sunDir.z > 0.0f ? 1.0f : 0.0f;
	gpc.sunColor[0] = CTimeCycle::GetDirectionalRed();
	gpc.sunColor[1] = CTimeCycle::GetDirectionalGreen();
	gpc.sunColor[2] = CTimeCycle::GetDirectionalBlue();
	gpc.sunColor[3] = gfEmissiveBoost;	// night windows/neon
	gpc.skyTop[0] = CTimeCycle::GetSkyTopRed()/255.0f;
	gpc.skyTop[1] = CTimeCycle::GetSkyTopGreen()/255.0f;
	gpc.skyTop[2] = CTimeCycle::GetSkyTopBlue()/255.0f;
	gpc.skyBottom[0] = CTimeCycle::GetSkyBottomRed()/255.0f;
	gpc.skyBottom[1] = CTimeCycle::GetSkyBottomGreen()/255.0f;
	gpc.skyBottom[2] = CTimeCycle::GetSkyBottomBlue()/255.0f;
	gpc.size[0] = w; gpc.size[1] = h;
	gpc.frame = frame;
	gpc.pad0 = numLights;
	gLastNumLights = numLights;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gGiPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gGiPipeLayout, 0, 1, &gGiDescSet, 0, nullptr);
	vkCmdPushConstants(cmd, gGiPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gpc), &gpc);
	vkCmdDispatch(cmd, (w + 7)/8, (h + 7)/8, 1);

	// giRaw written -> temporal reads it
	VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &memBarrier, 0, nullptr, 0, nullptr);

	// temporal descriptors
	{
	VkDescriptorImageInfo rawInfo = { VK_NULL_HANDLE, gGiRaw.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo prevAccumInfo = { gInterop.sampler, gGiAccum[prev].view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo prevDepthInfo = { gInterop.sampler, gDepthHist[prev].view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo depthInfo = { gInterop.sampler, gInterop.gbDepth.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo outAccumInfo = { VK_NULL_HANDLE, gGiAccum[cur].view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo outSharedInfo = { VK_NULL_HANDLE, gInterop.giOutput.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo outPrevDepthInfo = { VK_NULL_HANDLE, gDepthHist[cur].view, VK_IMAGE_LAYOUT_GENERAL };

	VkWriteDescriptorSet writes[7] = {};
	const VkDescriptorImageInfo *infos[7] = { &rawInfo, &prevAccumInfo, &prevDepthInfo,
		&depthInfo, &outAccumInfo, &outSharedInfo, &outPrevDepthInfo };
	VkDescriptorType types[7] = {
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	};
	for(int i = 0; i < 7; i++){
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = gTemporalDescSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = types[i];
		writes[i].pImageInfo = infos[i];
	}
	vkUpdateDescriptorSets(gVk.device, 7, writes, 0, nullptr);
	}

	TemporalPushConstants tpc = {};
	fillCamera(tpc.camPos, tpc.camRight, tpc.camUp, tpc.camFwd);
	if(gHavePrevCam)
		memcpy(tpc.prevCamPos, gPrevCam, sizeof(gPrevCam));
	else
		resetHistory = true;
	tpc.size[0] = w; tpc.size[1] = h;
	tpc.frame = frame;
	tpc.reset = resetHistory ? 1 : 0;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gTemporalPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gTemporalPipeLayout, 0, 1, &gTemporalDescSet, 0, nullptr);
	vkCmdPushConstants(cmd, gTemporalPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(tpc), &tpc);
	vkCmdDispatch(cmd, (w + 7)/8, (h + 7)/8, 1);

	// remember this frame's camera for reprojection
	memcpy(gPrevCam, tpc.camPos, sizeof(gPrevCam));
	gHavePrevCam = true;
	gAccumIndex = prev;
	return true;
}

void
PassesTraceReflections(VkCommandBuffer cmd, uint32_t frame)
{
	int w = gInterop.reflOutput.width, h = gInterop.reflOutput.height;

	VkAccelerationStructureKHR tlas = TlasHandle();
	VkWriteDescriptorSetAccelerationStructureKHR asWrite = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
	asWrite.accelerationStructureCount = 1;
	asWrite.pAccelerationStructures = &tlas;
	VkDescriptorImageInfo outInfo = { VK_NULL_HANDLE, gInterop.reflOutput.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo normalInfo = { gInterop.sampler, gInterop.gbNormal.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo depthInfo = { gInterop.sampler, gInterop.gbDepth.view, VK_IMAGE_LAYOUT_GENERAL };
	GpuBuffer *records = BlasRecordBuffer();
	VkDescriptorBufferInfo bufInfo = { records->buf, 0, VK_WHOLE_SIZE };

	VkWriteDescriptorSet writes[6] = {};
	for(int i = 0; i < 6; i++){
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = gReflDescSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[0].pNext = &asWrite;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	writes[1].pImageInfo = &outInfo;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[2].pImageInfo = &normalInfo;
	writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[3].pImageInfo = &depthInfo;
	writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[4].pBufferInfo = &bufInfo;
	writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[5].descriptorCount = TEXCACHE_MAX;
	writes[5].pImageInfo = texCacheInfos();
	vkUpdateDescriptorSets(gVk.device, 6, writes, 0, nullptr);

	ReflPushConstants pc = {};
	fillCamera(pc.camPos, pc.camRight, pc.camUp, pc.camFwd);
	CVector sunDir = CTimeCycle::GetSunDirection();
	pc.sunDir[0] = sunDir.x; pc.sunDir[1] = sunDir.y; pc.sunDir[2] = sunDir.z;
	pc.sunDir[3] = sunDir.z > 0.0f ? 1.0f : 0.0f;
	pc.sunColor[0] = CTimeCycle::GetDirectionalRed();
	pc.sunColor[1] = CTimeCycle::GetDirectionalGreen();
	pc.sunColor[2] = CTimeCycle::GetDirectionalBlue();
	pc.sunColor[3] = gfEmissiveBoost;	// night windows/neon
	pc.skyTop[0] = CTimeCycle::GetSkyTopRed()/255.0f;
	pc.skyTop[1] = CTimeCycle::GetSkyTopGreen()/255.0f;
	pc.skyTop[2] = CTimeCycle::GetSkyTopBlue()/255.0f;
	pc.skyBottom[0] = CTimeCycle::GetSkyBottomRed()/255.0f;
	pc.skyBottom[1] = CTimeCycle::GetSkyBottomGreen()/255.0f;
	pc.skyBottom[2] = CTimeCycle::GetSkyBottomBlue()/255.0f;
	pc.size[0] = w; pc.size[1] = h;
	pc.frame = frame;
	pc.wetness = CWeather::WetRoads;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gReflPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gReflPipeLayout, 0, 1, &gReflDescSet, 0, nullptr);
	vkCmdPushConstants(cmd, gReflPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (w + 7)/8, (h + 7)/8, 1);
}

void
PassesDenoiseGI(VkCommandBuffer cmd)
{
	int w = gInterop.giOutput.width, h = gInterop.giOutput.height;
	// note: gAccumIndex was flipped after the temporal pass; the image the
	// temporal pass just wrote is the new "prev"
	GpuImage *accum = &gGiAccum[1 - gAccumIndex];

	// temporal write -> denoiser read
	VkMemoryBarrier memBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	// iteration i/o: accum -> scratch -> giRaw -> shared giOutput
	VkImageView srcs[3] = { accum->view, gAtrousScratch.view, gGiRaw.view };
	VkImageView dsts[3] = { gAtrousScratch.view, gGiRaw.view, gInterop.giOutput.view };
	int steps[3] = { 1, 2, 4 };

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gAtrousPipeline);
	for(int i = 0; i < 3; i++){
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &memBarrier, 0, nullptr, 0, nullptr);

		VkDescriptorImageInfo inInfo = { gInterop.sampler, srcs[i], VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo outInfo = { VK_NULL_HANDLE, dsts[i], VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo normalInfo = { gInterop.sampler, gInterop.gbNormal.view, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo depthInfo = { gInterop.sampler, gInterop.gbDepth.view, VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet writes[4] = {};
		const VkDescriptorImageInfo *infos[4] = { &inInfo, &outInfo, &normalInfo, &depthInfo };
		VkDescriptorType types[4] = {
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		};
		for(int j = 0; j < 4; j++){
			writes[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[j].dstSet = gAtrousDescSets[i];
			writes[j].dstBinding = j;
			writes[j].descriptorCount = 1;
			writes[j].descriptorType = types[j];
			writes[j].pImageInfo = infos[j];
		}
		vkUpdateDescriptorSets(gVk.device, 4, writes, 0, nullptr);

		AtrousPushConstants pc = {};
		pc.size[0] = w; pc.size[1] = h;
		pc.step = steps[i];
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, gAtrousPipeLayout, 0, 1, &gAtrousDescSets[i], 0, nullptr);
		vkCmdPushConstants(cmd, gAtrousPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cmd, (w + 7)/8, (h + 7)/8, 1);
	}
}

}

#endif
