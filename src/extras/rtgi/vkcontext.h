#pragma once

#ifdef RTGI

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include "volk.h"

namespace RayTracedGI {

// printf + flushed rtgi.log (the game is a windowed app; stdout is unreliable)
void RtgiLog(const char *fmt, ...);

struct VkContext
{
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkQueue queue;
	uint32_t queueFamily;
	VkCommandPool cmdPool;
	VkCommandBuffer cmdBuf;
	VkFence frameFence;

	// capabilities discovered at init
	bool hasRayTracing;	// accel struct + RT pipeline + ray query
	uint8_t deviceLUID[8];	// VK_LUID_SIZE
	char deviceName[256];

	bool valid;
};

extern VkContext gVk;

// glDeviceLUID: the 8-byte LUID of the GL context's device (from
// GL_DEVICE_LUID_EXT), used to pick the matching VkPhysicalDevice.
bool VkContextCreate(const uint8_t *glDeviceLUID);
void VkContextDestroy(void);

// find a device memory type index; returns UINT32_MAX if none
uint32_t VkFindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);

}

#endif
