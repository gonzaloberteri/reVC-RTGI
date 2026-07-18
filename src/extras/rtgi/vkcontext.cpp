#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "vkcontext.h"

namespace RayTracedGI {

VkContext gVk;

static const char *requiredDeviceExts[] = {
	VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
	VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
};

static const char *rayTracingExts[] = {
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
	VK_KHR_RAY_QUERY_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

static bool
hasExtension(VkExtensionProperties *exts, uint32_t count, const char *name)
{
	for(uint32_t i = 0; i < count; i++)
		if(strcmp(exts[i].extensionName, name) == 0)
			return true;
	return false;
}

bool
VkContextCreate(const uint8_t *glDeviceLUID)
{
	memset(&gVk, 0, sizeof(gVk));

	if(volkInitialize() != VK_SUCCESS){
		RtgiLog("RTGI: Vulkan loader not found\n");
		return false;
	}

	VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	appInfo.pApplicationName = "reVC-RTGI";
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo instInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	instInfo.pApplicationInfo = &appInfo;

	if(vkCreateInstance(&instInfo, nullptr, &gVk.instance) != VK_SUCCESS){
		RtgiLog("RTGI: vkCreateInstance failed\n");
		return false;
	}
	volkLoadInstance(gVk.instance);

	// pick the physical device whose LUID matches the GL context's device
	uint32_t devCount = 0;
	vkEnumeratePhysicalDevices(gVk.instance, &devCount, nullptr);
	if(devCount == 0){
		RtgiLog("RTGI: no Vulkan devices\n");
		return false;
	}
	VkPhysicalDevice devices[16];
	if(devCount > 16) devCount = 16;
	vkEnumeratePhysicalDevices(gVk.instance, &devCount, devices);

	for(uint32_t i = 0; i < devCount; i++){
		VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		props2.pNext = &idProps;
		vkGetPhysicalDeviceProperties2(devices[i], &props2);

		if(idProps.deviceLUIDValid &&
		   memcmp(idProps.deviceLUID, glDeviceLUID, VK_LUID_SIZE) == 0){
			gVk.physicalDevice = devices[i];
			memcpy(gVk.deviceLUID, idProps.deviceLUID, VK_LUID_SIZE);
			strncpy(gVk.deviceName, props2.properties.deviceName, sizeof(gVk.deviceName)-1);
			break;
		}
	}
	if(gVk.physicalDevice == VK_NULL_HANDLE){
		RtgiLog("RTGI: no Vulkan device matches the GL context's LUID\n");
		return false;
	}
	RtgiLog("RTGI: using %s\n", gVk.deviceName);

	// check extensions
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(gVk.physicalDevice, nullptr, &extCount, nullptr);
	VkExtensionProperties *exts = new VkExtensionProperties[extCount];
	vkEnumerateDeviceExtensionProperties(gVk.physicalDevice, nullptr, &extCount, exts);

	for(size_t i = 0; i < sizeof(requiredDeviceExts)/sizeof(requiredDeviceExts[0]); i++)
		if(!hasExtension(exts, extCount, requiredDeviceExts[i])){
			RtgiLog("RTGI: missing required extension %s\n", requiredDeviceExts[i]);
			delete[] exts;
			return false;
		}

	gVk.hasRayTracing = true;
	for(size_t i = 0; i < sizeof(rayTracingExts)/sizeof(rayTracingExts[0]); i++)
		if(!hasExtension(exts, extCount, rayTracingExts[i])){
			RtgiLog("RTGI: ray tracing extension %s not available\n", rayTracingExts[i]);
			gVk.hasRayTracing = false;
		}
	delete[] exts;

	// queue: one graphics+compute queue
	uint32_t qfCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gVk.physicalDevice, &qfCount, nullptr);
	VkQueueFamilyProperties qfs[32];
	if(qfCount > 32) qfCount = 32;
	vkGetPhysicalDeviceQueueFamilyProperties(gVk.physicalDevice, &qfCount, qfs);
	gVk.queueFamily = UINT32_MAX;
	for(uint32_t i = 0; i < qfCount; i++)
		if(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){
			gVk.queueFamily = i;
			break;
		}
	if(gVk.queueFamily == UINT32_MAX){
		RtgiLog("RTGI: no graphics queue\n");
		return false;
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queueInfo.queueFamilyIndex = gVk.queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &prio;

	// device extensions: interop always; RT set when available (features enabled in M2)
	const char *enabledExts[16];
	uint32_t enabledCount = 0;
	for(size_t i = 0; i < sizeof(requiredDeviceExts)/sizeof(requiredDeviceExts[0]); i++)
		enabledExts[enabledCount++] = requiredDeviceExts[i];
	if(gVk.hasRayTracing)
		for(size_t i = 0; i < sizeof(rayTracingExts)/sizeof(rayTracingExts[0]); i++)
			enabledExts[enabledCount++] = rayTracingExts[i];

	VkPhysicalDeviceVulkan12Features feat12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	feat12.bufferDeviceAddress = VK_TRUE;
	feat12.descriptorIndexing = VK_TRUE;
	feat12.runtimeDescriptorArray = VK_TRUE;
	feat12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR featAccel = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
	featAccel.accelerationStructure = VK_TRUE;
	VkPhysicalDeviceRayTracingPipelineFeaturesKHR featRtp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
	featRtp.rayTracingPipeline = VK_TRUE;
	VkPhysicalDeviceRayQueryFeaturesKHR featRq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
	featRq.rayQuery = VK_TRUE;

	VkDeviceCreateInfo devInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	devInfo.queueCreateInfoCount = 1;
	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.enabledExtensionCount = enabledCount;
	devInfo.ppEnabledExtensionNames = enabledExts;
	devInfo.pNext = &feat12;
	if(gVk.hasRayTracing){
		feat12.pNext = &featAccel;
		featAccel.pNext = &featRtp;
		featRtp.pNext = &featRq;
	}

	if(vkCreateDevice(gVk.physicalDevice, &devInfo, nullptr, &gVk.device) != VK_SUCCESS){
		RtgiLog("RTGI: vkCreateDevice failed\n");
		return false;
	}
	volkLoadDevice(gVk.device);
	vkGetDeviceQueue(gVk.device, gVk.queueFamily, 0, &gVk.queue);

	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = gVk.queueFamily;
	if(vkCreateCommandPool(gVk.device, &poolInfo, nullptr, &gVk.cmdPool) != VK_SUCCESS)
		return false;

	VkCommandBufferAllocateInfo cbInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	cbInfo.commandPool = gVk.cmdPool;
	cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbInfo.commandBufferCount = 1;
	if(vkAllocateCommandBuffers(gVk.device, &cbInfo, &gVk.cmdBuf) != VK_SUCCESS)
		return false;

	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if(vkCreateFence(gVk.device, &fenceInfo, nullptr, &gVk.frameFence) != VK_SUCCESS)
		return false;

	if(gVk.hasRayTracing){
		gVk.accelProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
		VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		props2.pNext = &gVk.accelProps;
		vkGetPhysicalDeviceProperties2(gVk.physicalDevice, &props2);
	}

	// VMA on top of volk's function pointers
	VmaVulkanFunctions vmaFuncs = {};
	vmaFuncs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	vmaFuncs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
	VmaAllocatorCreateInfo vmaInfo = {};
	vmaInfo.physicalDevice = gVk.physicalDevice;
	vmaInfo.device = gVk.device;
	vmaInfo.instance = gVk.instance;
	vmaInfo.vulkanApiVersion = VK_API_VERSION_1_2;
	vmaInfo.pVulkanFunctions = &vmaFuncs;
	vmaInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if(vmaCreateAllocator(&vmaInfo, &gVk.allocator) != VK_SUCCESS){
		RtgiLog("RTGI: vmaCreateAllocator failed\n");
		return false;
	}

	RtgiLog("RTGI: Vulkan device ready (ray tracing: %s)\n", gVk.hasRayTracing ? "yes" : "NO");
	gVk.valid = true;
	return true;
}

void
VkContextDestroy(void)
{
	if(gVk.device){
		vkDeviceWaitIdle(gVk.device);
		if(gVk.allocator) vmaDestroyAllocator(gVk.allocator);
		if(gVk.frameFence) vkDestroyFence(gVk.device, gVk.frameFence, nullptr);
		if(gVk.cmdPool) vkDestroyCommandPool(gVk.device, gVk.cmdPool, nullptr);
		vkDestroyDevice(gVk.device, nullptr);
	}
	if(gVk.instance)
		vkDestroyInstance(gVk.instance, nullptr);
	memset(&gVk, 0, sizeof(gVk));
}

// --- buffer helper ------------------------------------------------------------

bool
BufferCreate(GpuBuffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
	memset(b, 0, sizeof(*b));

	VkBufferCreateInfo bufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufInfo.size = size;
	bufInfo.usage = usage;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	if(hostVisible)
		allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
			VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo outInfo;
	if(vmaCreateBuffer(gVk.allocator, &bufInfo, &allocInfo, &b->buf, &b->alloc, &outInfo) != VK_SUCCESS){
		RtgiLog("RTGI: buffer allocation failed (%llu bytes)\n", (unsigned long long)size);
		return false;
	}
	b->size = size;
	b->mapped = outInfo.pMappedData;

	if(usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT){
		VkBufferDeviceAddressInfo addrInfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
		addrInfo.buffer = b->buf;
		b->addr = vkGetBufferDeviceAddress(gVk.device, &addrInfo);
	}
	return true;
}

void
BufferDestroy(GpuBuffer *b)
{
	if(b->buf)
		vmaDestroyBuffer(gVk.allocator, b->buf, b->alloc);
	memset(b, 0, sizeof(*b));
}

bool
BufferEnsure(GpuBuffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible)
{
	if(b->buf && b->size >= size)
		return true;
	BufferDestroy(b);
	return BufferCreate(b, size + size/2, usage, hostVisible);
}

bool
ImageCreate(GpuImage *img, int width, int height, VkFormat format, VkImageUsageFlags usage)
{
	memset(img, 0, sizeof(*img));
	img->format = format;
	img->width = width;
	img->height = height;

	VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.imageType = VK_IMAGE_TYPE_2D;
	imgInfo.format = format;
	imgInfo.extent = { (uint32_t)width, (uint32_t)height, 1 };
	imgInfo.mipLevels = 1;
	imgInfo.arrayLayers = 1;
	imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.usage = usage;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
	if(vmaCreateImage(gVk.allocator, &imgInfo, &allocInfo, &img->image, &img->alloc, nullptr) != VK_SUCCESS)
		return false;

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = img->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = format;
	viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	return vkCreateImageView(gVk.device, &viewInfo, nullptr, &img->view) == VK_SUCCESS;
}

void
ImageDestroy(GpuImage *img)
{
	if(img->view) vkDestroyImageView(gVk.device, img->view, nullptr);
	if(img->image) vmaDestroyImage(gVk.allocator, img->image, img->alloc);
	memset(img, 0, sizeof(*img));
}

uint32_t
VkFindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties memProps;
	vkGetPhysicalDeviceMemoryProperties(gVk.physicalDevice, &memProps);
	for(uint32_t i = 0; i < memProps.memoryTypeCount; i++)
		if((typeBits & (1u << i)) &&
		   (memProps.memoryTypes[i].propertyFlags & props) == props)
			return i;
	return UINT32_MAX;
}

}

#endif
