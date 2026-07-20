#pragma once

#ifdef RTGI

#include "vkcontext.h"

namespace RayTracedGI {

// One VK-allocated, GL-imported shared image. GL writes the G-buffer ones as
// FBO attachments; VK writes the output ones from compute; both sides use
// GENERAL layout throughout to keep cross-API layout state trivial.
struct SharedImage
{
	// VK side
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
	VkDeviceSize size;
	VkFormat format;
	// GL side
	uint32_t glMemoryObject;
	uint32_t glTexture;
	uint32_t glInternalFormat;

	int width, height;
};

struct InteropState
{
	bool glExtensionsPresent;
	uint8_t glDeviceLUID[8];

	// GL writes, VK reads
	SharedImage gbNormal;	// RGBA16F world normal
	SharedImage gbDepth;	// R32F linear view depth
	// VK writes, GL reads
	SharedImage rtOutput;	// RGBA16F debug/trace output
	SharedImage aoOutput;	// RG8: r = AO visibility, g = sun visibility (M4)
	SharedImage giOutput;	// RGBA16F accumulated diffuse GI radiance
	SharedImage reflOutput;	// RGBA16F reflections, a = reflectivity
	SharedImage volOutput;	// RGBA16F half-res volumetric light shafts

	VkSampler sampler;	// nearest, for sampling the G-buffer in compute

	// GL gbuffer done -> VK may trace; VK->GL: results ready;
	// GL->VK: GL done reading previous frame
	VkSemaphore semGbufDone;
	VkSemaphore semRtDone;
	VkSemaphore semGlDone;
	uint32_t glSemGbufDone;
	uint32_t glSemRtDone;
	uint32_t glSemGlDone;

	bool firstFrame;
	bool valid;
};

extern InteropState gInterop;

// load GL entry points + query LUID; needs a current GL context. false if the
// required GL extensions are missing.
bool InteropLoadGL(void);
// create shared images + semaphores; needs a valid VkContext
bool InteropCreate(int width, int height);
void InteropDestroy(void);

bool SharedImageCreate(SharedImage *img, int width, int height,
	VkFormat vkFormat, uint32_t glInternalFormat, bool vkStorage);
void SharedImageDestroy(SharedImage *img);

// GL-side sync helpers
void InteropSignalGbufDone(void);	// after the G-buffer prepass
void InteropWaitRtDone(void);	// server-side wait until VK signalled results
void InteropSignalGlDone(void);	// tell VK that GL consumed them

}

#endif
