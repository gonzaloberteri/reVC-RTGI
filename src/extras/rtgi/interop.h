#pragma once

#ifdef RTGI

#include "vkcontext.h"

namespace RayTracedGI {

// One VK-allocated, GL-imported shared image plus the semaphore pair used to
// hand it back and forth each frame. All images the side-car produces use this
// mechanism; M1 has a single proof-of-concept image.
struct SharedImage
{
	// VK side
	VkImage image;
	VkDeviceMemory memory;
	VkDeviceSize size;
	// GL side
	uint32_t glMemoryObject;
	uint32_t glTexture;

	int width, height;
};

struct InteropState
{
	bool glExtensionsPresent;
	uint8_t glDeviceLUID[8];

	SharedImage rtOutput;	// M1: VK clears this, GL displays it

	// VK->GL: RT results ready; GL->VK: GL done reading previous frame
	VkSemaphore semRtDone;
	VkSemaphore semGlDone;
	uint32_t glSemRtDone;
	uint32_t glSemGlDone;

	bool firstFrame;
	bool valid;
};

extern InteropState gInterop;

// load GL entry points + query LUID; needs a current GL context. false if the
// required GL extensions are missing.
bool InteropLoadGL(void);
// create shared image + semaphores; needs a valid VkContext
bool InteropCreate(int width, int height);
void InteropDestroy(void);

bool SharedImageCreate(SharedImage *img, int width, int height);
void SharedImageDestroy(SharedImage *img);

// GL-side sync helpers
void InteropWaitRtDone(void);	// server-side wait until VK signalled results
void InteropSignalGlDone(void);	// tell VK that GL consumed them

}

#endif
