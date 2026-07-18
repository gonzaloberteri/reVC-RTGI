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

#include "shaders/obj/primary_comp.inc"

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

static VkDescriptorSetLayout gSetLayout;
static VkPipelineLayout gPipeLayout;
static VkPipeline gPipeline;
static VkDescriptorPool gDescPool;
static VkDescriptorSet gDescSet;
static VkImageView gOutView;

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

	VkDescriptorPoolSize poolSizes[3] = {
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
	};
	VkDescriptorPoolCreateInfo dpInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	dpInfo.maxSets = 1;
	dpInfo.poolSizeCount = 3;
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

	return true;
}

void
PassesShutdown(void)
{
	if(gOutView) vkDestroyImageView(gVk.device, gOutView, nullptr);
	if(gDescPool) vkDestroyDescriptorPool(gVk.device, gDescPool, nullptr);
	if(gPipeline) vkDestroyPipeline(gVk.device, gPipeline, nullptr);
	if(gPipeLayout) vkDestroyPipelineLayout(gVk.device, gPipeLayout, nullptr);
	if(gSetLayout) vkDestroyDescriptorSetLayout(gVk.device, gSetLayout, nullptr);
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

}

#endif
