#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "interop.h"

// GL core entry points come from librw's glad; keep GLFW from dragging in GL/gl.h
#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// --- GL_EXT_memory_object / GL_EXT_semaphore (win32) --------------------------
// librw's glad was not generated with these, so we carry the constants and load
// the entry points ourselves via glfwGetProcAddress.

#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT   0x9587
#define GL_DEDICATED_MEMORY_OBJECT_EXT    0x9581
#define GL_DEVICE_LUID_EXT                0x9599
#define GL_LAYOUT_GENERAL_EXT             0x958D

typedef void (APIENTRY *PFNGLGETUNSIGNEDBYTEVEXT)(GLenum pname, GLubyte *data);
typedef void (APIENTRY *PFNGLCREATEMEMORYOBJECTSEXT)(GLsizei n, GLuint *memoryObjects);
typedef void (APIENTRY *PFNGLDELETEMEMORYOBJECTSEXT)(GLsizei n, const GLuint *memoryObjects);
typedef void (APIENTRY *PFNGLMEMORYOBJECTPARAMETERIVEXT)(GLuint memoryObject, GLenum pname, const GLint *params);
typedef void (APIENTRY *PFNGLIMPORTMEMORYWIN32HANDLEEXT)(GLuint memory, GLuint64 size, GLenum handleType, void *handle);
typedef void (APIENTRY *PFNGLTEXSTORAGEMEM2DEXT)(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
typedef void (APIENTRY *PFNGLGENSEMAPHORESEXT)(GLsizei n, GLuint *semaphores);
typedef void (APIENTRY *PFNGLDELETESEMAPHORESEXT)(GLsizei n, const GLuint *semaphores);
typedef void (APIENTRY *PFNGLIMPORTSEMAPHOREWIN32HANDLEEXT)(GLuint semaphore, GLenum handleType, void *handle);
typedef void (APIENTRY *PFNGLWAITSEMAPHOREEXT)(GLuint semaphore, GLuint numBufferBarriers, const GLuint *buffers, GLuint numTextureBarriers, const GLuint *textures, const GLenum *srcLayouts);
typedef void (APIENTRY *PFNGLSIGNALSEMAPHOREEXT)(GLuint semaphore, GLuint numBufferBarriers, const GLuint *buffers, GLuint numTextureBarriers, const GLuint *textures, const GLenum *dstLayouts);

static PFNGLGETUNSIGNEDBYTEVEXT           glGetUnsignedBytevEXT_;
static PFNGLCREATEMEMORYOBJECTSEXT        glCreateMemoryObjectsEXT_;
static PFNGLDELETEMEMORYOBJECTSEXT        glDeleteMemoryObjectsEXT_;
static PFNGLMEMORYOBJECTPARAMETERIVEXT    glMemoryObjectParameterivEXT_;
static PFNGLIMPORTMEMORYWIN32HANDLEEXT    glImportMemoryWin32HandleEXT_;
static PFNGLTEXSTORAGEMEM2DEXT            glTexStorageMem2DEXT_;
static PFNGLGENSEMAPHORESEXT              glGenSemaphoresEXT_;
static PFNGLDELETESEMAPHORESEXT           glDeleteSemaphoresEXT_;
static PFNGLIMPORTSEMAPHOREWIN32HANDLEEXT glImportSemaphoreWin32HandleEXT_;
static PFNGLWAITSEMAPHOREEXT              glWaitSemaphoreEXT_;
static PFNGLSIGNALSEMAPHOREEXT            glSignalSemaphoreEXT_;

namespace RayTracedGI {

InteropState gInterop;

static bool
glHasExtension(const char *name)
{
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for(GLint i = 0; i < n; i++){
		const char *ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
		if(ext && strcmp(ext, name) == 0)
			return true;
	}
	return false;
}

bool
InteropLoadGL(void)
{
	memset(&gInterop, 0, sizeof(gInterop));

	if(!glHasExtension("GL_EXT_memory_object") ||
	   !glHasExtension("GL_EXT_memory_object_win32") ||
	   !glHasExtension("GL_EXT_semaphore") ||
	   !glHasExtension("GL_EXT_semaphore_win32")){
		RtgiLog("RTGI: GL interop extensions not available\n");
		return false;
	}

#define LOAD(name) name##_ = (decltype(name##_))glfwGetProcAddress(#name); if(name##_ == nullptr) return false
	LOAD(glGetUnsignedBytevEXT);
	LOAD(glCreateMemoryObjectsEXT);
	LOAD(glDeleteMemoryObjectsEXT);
	LOAD(glMemoryObjectParameterivEXT);
	LOAD(glImportMemoryWin32HandleEXT);
	LOAD(glTexStorageMem2DEXT);
	LOAD(glGenSemaphoresEXT);
	LOAD(glDeleteSemaphoresEXT);
	LOAD(glImportSemaphoreWin32HandleEXT);
	LOAD(glWaitSemaphoreEXT);
	LOAD(glSignalSemaphoreEXT);
#undef LOAD

	glGetUnsignedBytevEXT_(GL_DEVICE_LUID_EXT, gInterop.glDeviceLUID);
	gInterop.glExtensionsPresent = true;
	return true;
}

bool
SharedImageCreate(SharedImage *img, int width, int height)
{
	memset(img, 0, sizeof(*img));
	img->width = width;
	img->height = height;

	VkExternalMemoryImageCreateInfo extImg = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
	extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	VkImageCreateInfo imgInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imgInfo.pNext = &extImg;
	imgInfo.imageType = VK_IMAGE_TYPE_2D;
	imgInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	imgInfo.extent = { (uint32_t)width, (uint32_t)height, 1 };
	imgInfo.mipLevels = 1;
	imgInfo.arrayLayers = 1;
	imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if(vkCreateImage(gVk.device, &imgInfo, nullptr, &img->image) != VK_SUCCESS)
		return false;

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements(gVk.device, img->image, &memReq);
	img->size = memReq.size;

	VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
	dedicated.image = img->image;
	VkExportMemoryAllocateInfo exportAlloc = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
	exportAlloc.pNext = &dedicated;
	exportAlloc.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

	VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.pNext = &exportAlloc;
	alloc.allocationSize = memReq.size;
	alloc.memoryTypeIndex = VkFindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if(alloc.memoryTypeIndex == UINT32_MAX)
		return false;
	if(vkAllocateMemory(gVk.device, &alloc, nullptr, &img->memory) != VK_SUCCESS)
		return false;
	if(vkBindImageMemory(gVk.device, img->image, img->memory, 0) != VK_SUCCESS)
		return false;

	VkMemoryGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
	handleInfo.memory = img->memory;
	handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	HANDLE handle = nullptr;
	if(vkGetMemoryWin32HandleKHR(gVk.device, &handleInfo, &handle) != VK_SUCCESS)
		return false;

	// GL import; the handle's ownership moves to GL
	glCreateMemoryObjectsEXT_(1, &img->glMemoryObject);
	GLint dedicatedParam = GL_TRUE;
	glMemoryObjectParameterivEXT_(img->glMemoryObject, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicatedParam);
	glImportMemoryWin32HandleEXT_(img->glMemoryObject, img->size, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);

	glGenTextures(1, &img->glTexture);
	glBindTexture(GL_TEXTURE_2D, img->glTexture);
	glTexStorageMem2DEXT_(GL_TEXTURE_2D, 1, GL_RGBA16F, width, height, img->glMemoryObject, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	if(glGetError() != GL_NO_ERROR){
		RtgiLog("RTGI: GL error importing shared image\n");
		return false;
	}
	return true;
}

void
SharedImageDestroy(SharedImage *img)
{
	if(img->glTexture) glDeleteTextures(1, &img->glTexture);
	if(img->glMemoryObject) glDeleteMemoryObjectsEXT_(1, &img->glMemoryObject);
	if(img->image) vkDestroyImage(gVk.device, img->image, nullptr);
	if(img->memory) vkFreeMemory(gVk.device, img->memory, nullptr);
	memset(img, 0, sizeof(*img));
}

static bool
sharedSemaphoreCreate(VkSemaphore *vkSem, uint32_t *glSem)
{
	VkExportSemaphoreCreateInfo exportSem = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
	exportSem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	semInfo.pNext = &exportSem;
	if(vkCreateSemaphore(gVk.device, &semInfo, nullptr, vkSem) != VK_SUCCESS)
		return false;

	VkSemaphoreGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
	handleInfo.semaphore = *vkSem;
	handleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	HANDLE handle = nullptr;
	if(vkGetSemaphoreWin32HandleKHR(gVk.device, &handleInfo, &handle) != VK_SUCCESS)
		return false;

	glGenSemaphoresEXT_(1, glSem);
	glImportSemaphoreWin32HandleEXT_(*glSem, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
	return glGetError() == GL_NO_ERROR;
}

bool
InteropCreate(int width, int height)
{
	if(!gInterop.glExtensionsPresent || !gVk.valid)
		return false;

	if(!SharedImageCreate(&gInterop.rtOutput, width, height))
		return false;
	if(!sharedSemaphoreCreate(&gInterop.semRtDone, &gInterop.glSemRtDone))
		return false;
	if(!sharedSemaphoreCreate(&gInterop.semGlDone, &gInterop.glSemGlDone))
		return false;

	gInterop.firstFrame = true;
	gInterop.valid = true;
	RtgiLog("RTGI: interop resources created (%dx%d)\n", width, height);
	return true;
}

void
InteropDestroy(void)
{
	if(gVk.device)
		vkDeviceWaitIdle(gVk.device);
	if(gInterop.glSemRtDone) glDeleteSemaphoresEXT_(1, &gInterop.glSemRtDone);
	if(gInterop.glSemGlDone) glDeleteSemaphoresEXT_(1, &gInterop.glSemGlDone);
	if(gInterop.semRtDone) vkDestroySemaphore(gVk.device, gInterop.semRtDone, nullptr);
	if(gInterop.semGlDone) vkDestroySemaphore(gVk.device, gInterop.semGlDone, nullptr);
	SharedImageDestroy(&gInterop.rtOutput);
	memset(&gInterop, 0, sizeof(gInterop));
}

void
InteropWaitRtDone(void)
{
	GLenum layout = GL_LAYOUT_GENERAL_EXT;
	glWaitSemaphoreEXT_(gInterop.glSemRtDone, 0, nullptr, 1, &gInterop.rtOutput.glTexture, &layout);
}

void
InteropSignalGlDone(void)
{
	GLenum layout = GL_LAYOUT_GENERAL_EXT;
	glSignalSemaphoreEXT_(gInterop.glSemGlDone, 0, nullptr, 1, &gInterop.rtOutput.glTexture, &layout);
	// make sure the signal reaches the driver promptly
	glFlush();
}

}

#endif
