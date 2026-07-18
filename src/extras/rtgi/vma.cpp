#ifdef RTGI

// VulkanMemoryAllocator implementation TU. Function pointers come from volk.

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

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#pragma warning(push)
#pragma warning(disable: 4100 4127 4189 4324 4505)
#include "vk_mem_alloc.h"
#pragma warning(pop)

#endif
