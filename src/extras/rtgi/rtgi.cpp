#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include "interop.h"

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "blas.h"
#include "tlas.h"
#include "passes.h"

#include "common.h"
#include "rtgi.h"
#include "skeleton.h"
#include "debugmenu.h"

namespace RayTracedGI {

void
RtgiLog(const char *fmt, ...)
{
	static FILE *logFile;
	if(logFile == nil)
		logFile = fopen("rtgi.log", "w");

	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	if(logFile){
		va_start(args, fmt);
		vfprintf(logFile, fmt, args);
		va_end(args);
		fflush(logFile);
	}
}

bool gbRayTracedGI = true;
int32 gnDebugView = DEBUGVIEW_RT_NORMALS;	// M2 default: show the RT scene

static bool initialised;
static uint32 frameCounter;
// a VK submit happened this frame and GL must signal it back, regardless of
// what the debug menu did to the toggles in between
static bool gFrameSubmitted;

// --- debug blit ---------------------------------------------------------------

static GLuint blitProgram;
static GLuint blitVAO;

static const char *blitVertSrc =
"#version 330\n"
"out vec2 v_uv;\n"
"void main() {\n"
"	vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
"	v_uv = p;\n"
"	// standard fullscreen triangle; DebugRender restricts it with the viewport\n"
"	gl_Position = vec4(p*2.0 - 1.0, 0.0, 1.0);\n"
"}\n";

static const char *blitFragSrc =
"#version 330\n"
"in vec2 v_uv;\n"
"out vec4 color;\n"
"uniform sampler2D tex;\n"
"void main() { color = vec4(texture(tex, v_uv).rgb, 1.0); }\n";

static GLuint
compileShader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nil);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if(!ok){
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), nil, log);
		RtgiLog("RTGI: shader compile error: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static bool
blitCreate(void)
{
	GLuint vs = compileShader(GL_VERTEX_SHADER, blitVertSrc);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, blitFragSrc);
	if(vs == 0 || fs == 0)
		return false;
	blitProgram = glCreateProgram();
	glAttachShader(blitProgram, vs);
	glAttachShader(blitProgram, fs);
	glLinkProgram(blitProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(blitProgram, GL_LINK_STATUS, &ok);
	if(!ok)
		return false;
	glGenVertexArrays(1, &blitVAO);
	return true;
}

static void
blitDestroy(void)
{
	if(blitProgram) glDeleteProgram(blitProgram);
	if(blitVAO) glDeleteVertexArrays(1, &blitVAO);
	blitProgram = 0;
	blitVAO = 0;
}

// --- lifecycle ----------------------------------------------------------------

void
Initialise(void)
{
	if(initialised)
		return;

	if(!InteropLoadGL())
		goto fail;
	if(!VkContextCreate(gInterop.glDeviceLUID))
		goto fail;
	if(!InteropCreate(RsGlobal.maximumWidth, RsGlobal.maximumHeight))
		goto fail;
	if(!blitCreate())
		goto fail;
	if(gVk.hasRayTracing){
		// before any world geometry loads, so streamed-out geometry
		// frees its BLAS automatically
		BlasRegisterPlugin();
		if(!BlasInit() || !TlasInit() || !PassesInit())
			goto fail;
	}

	initialised = true;
	RtgiLog("RTGI: initialised\n");
	return;

fail:
	RtgiLog("RTGI: initialisation failed, ray traced GI disabled\n");
	VkContextDestroy();
	gbRayTracedGI = false;
}

void
Shutdown(void)
{
	if(!initialised)
		return;
	if(gVk.hasRayTracing){
		PassesShutdown();
		TlasShutdown();
		BlasShutdown();
	}
	blitDestroy();
	InteropDestroy();
	VkContextDestroy();
	initialised = false;
}

// --- per frame ----------------------------------------------------------------

void
RenderFrame(void)
{
	if(!initialised || !gbRayTracedGI)
		return;

	// don't re-record while the previous frame's VK work is in flight
	vkWaitForFences(gVk.device, 1, &gVk.frameFence, VK_TRUE, UINT64_MAX);
	vkResetFences(gVk.device, 1, &gVk.frameFence);

	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(gVk.cmdBuf, &begin);

	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	bool wantTrace = gVk.hasRayTracing;
	bool traced = false;
	if(wantTrace){
		BlasBeginFrame();
		TlasCollect(gVk.cmdBuf);	// walks game world, queues BLAS builds
		traced = TlasBuild(gVk.cmdBuf);
	}

	if(traced){
		// image to GENERAL for compute writes
		VkImageMemoryBarrier toGeneral = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toGeneral.srcAccessMask = 0;
		toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toGeneral.image = gInterop.rtOutput.image;
		toGeneral.subresourceRange = range;
		vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &toGeneral);

		uint32_t mode = 1;
		if(gnDebugView == DEBUGVIEW_RT_DEPTH) mode = 2;
		else if(gnDebugView == DEBUGVIEW_RT_INSTANCES) mode = 3;
		PassesTracePrimary(gVk.cmdBuf, mode, frameCounter);

		VkImageMemoryBarrier afterTrace = toGeneral;
		afterTrace.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		afterTrace.dstAccessMask = 0;
		afterTrace.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		afterTrace.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, nullptr, 0, nullptr, 1, &afterTrace);
	}else{
		// fallback / M1 interop proof: animated clear
		VkImageMemoryBarrier toClear = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toClear.srcAccessMask = 0;
		toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toClear.image = gInterop.rtOutput.image;
		toClear.subresourceRange = range;
		vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &toClear);

		float t = frameCounter * 0.02f;
		VkClearColorValue color;
		color.float32[0] = 0.5f + 0.5f*sinf(t);
		color.float32[1] = 0.5f + 0.5f*sinf(t + 2.094f);
		color.float32[2] = 0.5f + 0.5f*sinf(t + 4.189f);
		color.float32[3] = 1.0f;
		vkCmdClearColorImage(gVk.cmdBuf, gInterop.rtOutput.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

		VkImageMemoryBarrier toGeneral = toClear;
		toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toGeneral.dstAccessMask = 0;
		toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0, 0, nullptr, 0, nullptr, 1, &toGeneral);
	}

	vkEndCommandBuffer(gVk.cmdBuf);

	if(traced && (frameCounter % 300) == 0)
		RtgiLog("RTGI: %u TLAS instances, %d BLASes\n", TlasInstanceCount(), BlasCount());

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &gVk.cmdBuf;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &gInterop.semRtDone;
	// wait until GL is done reading the previous frame's output
	if(!gInterop.firstFrame){
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &gInterop.semGlDone;
		submit.pWaitDstStageMask = &waitStage;
	}
	gInterop.firstFrame = false;

	vkQueueSubmit(gVk.queue, 1, &submit, gVk.frameFence);
	frameCounter++;
	gFrameSubmitted = true;

	// GL: block the pipe until VK results are ready (server-side)
	InteropWaitRtDone();
}

void
DebugRender(void)
{
	// keyed off gFrameSubmitted, not the toggles: the debug menu can flip
	// them between RenderFrame and here, and the semaphore pair must stay
	// balanced or the next submit deadlocks
	if(!initialised || !gFrameSubmitted)
		return;

	if(gnDebugView != DEBUGVIEW_OFF){
		GLint prevProgram, prevVAO, prevActiveTex, prevTex0;
		GLint prevViewport[4];
		GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);
		GLboolean blendWasOn = glIsEnabled(GL_BLEND);
		glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
		glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTex);
		glGetIntegerv(GL_VIEWPORT, prevViewport);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);

		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);
		// bottom-right quarter of the screen
		glViewport(prevViewport[0] + prevViewport[2]/2, prevViewport[1], prevViewport[2]/2, prevViewport[3]/2);
		glUseProgram(blitProgram);
		glBindVertexArray(blitVAO);
		glBindTexture(GL_TEXTURE_2D, gInterop.rtOutput.glTexture);
		glUniform1i(glGetUniformLocation(blitProgram, "tex"), 0);
		glDrawArrays(GL_TRIANGLES, 0, 3);

		glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
		glBindTexture(GL_TEXTURE_2D, prevTex0);
		glActiveTexture(prevActiveTex);
		glBindVertexArray(prevVAO);
		glUseProgram(prevProgram);
		if(depthWasOn) glEnable(GL_DEPTH_TEST);
		if(blendWasOn) glEnable(GL_BLEND);
	}

	// GL has now consumed this frame's output; let VK reuse it
	InteropSignalGlDone();
	gFrameSubmitted = false;
}

void
AddDebugMenuEntries(void)
{
	static const char *debugViews[] = { "Off", "Interop", "RT Normals", "RT Depth", "RT Instances" };
	DebugMenuAddVarBool8("RTGI", "Ray traced GI", (int8_t*)&gbRayTracedGI, nil);
	DebugMenuAddVar("RTGI", "Debug view", &gnDebugView, nil, 1, 0, DEBUGVIEW_MAX-1, debugViews);
}

}

#endif
