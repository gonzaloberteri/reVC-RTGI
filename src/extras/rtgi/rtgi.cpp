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
#include "gbuffer.h"

#include "common.h"
#include "rtgi.h"
#include "skeleton.h"
#include "debugmenu.h"
#include "Timer.h"
#include "Timecycle.h"
#include "Frontend.h"
#include "Draw.h"
#include "World.h"
#include "PlayerPed.h"
#include "Game.h"
#include "Clock.h"
#include "Weather.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Explosion.h"

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
int32 gnDebugView = DEBUGVIEW_OFF;	// M3: AO shows in the scene itself
bool gbAOEnable = true;
float gfAOStrength = 0.85f;
float gfAORadius = 2.5f;
int32 gnAORays = 2;
bool gbSunShadows = true;
bool gbMoonShadows = true;
bool gbGIEnable = true;
float gfGIBlend = 0.6f;
float gfGIExposure = 1.0f;
bool gbDenoise = true;
bool gbReflections = true;
bool gbReflFilter = true;
bool gbGlassRefl = true;
bool gbDumpTex;		// dev: log distinct world texture names (dumptex=1)
bool gbWaterCaustics = true;
float gfEmissiveBoost = 1.0f;
bool gbGI2 = true;
bool gbPhotoMode;
bool gbCheckerGI;

static bool initialised;
static uint32 frameCounter;
static bool gResetGIHistory = true;

// dev/testing: config "tp=x,y,z" teleports the player once the world is
// streamed in, so automated runs can verify any location
static float gTeleport[4];	// x,y,z + optional heading in degrees
static bool gTeleportHasHeading;
static bool gWantTeleport;
static int32 gnForceArea = 0;	// config "area=N": interior for the teleport (eAreaName)
static int32 gnForceHour = -1;	// config "hour=N": pin the game clock
static int32 gnForceWeather = -1;	// config "weather=N"
// config "cutscene=NAME,x,y,z": play a mission cutscene's camera spline at
// the given offset — deterministic camera paths for A/B captures (the
// spline data is offset-relative, so any location works). "cutloop=1"
// restarts the spline when it finishes (repeating sweep for GIFs).
static char gCutsceneName[12];
static float gCutsceneOfs[3];
static bool gWantCutscene;
static bool gCutsceneStarted;
static bool gbCutsceneLoop;
// config "explode=x,y,z": detonate a grenade-type explosion there every
// ~5 s once settled — verifies transient combat lights feed the GI
static float gExplodePos[3];
static bool gWantExplosions;
// a VK submit happened this frame and GL must signal it back, regardless of
// what the debug menu did to the toggles in between
static bool gFrameSubmitted;

// GPU pass timing: timestamps written along the frame's command buffer,
// read back after the frame fence, averaged into the telemetry line
enum {
	TS_BEGIN, TS_TLAS, TS_AO, TS_GI, TS_DENOISE, TS_REFL, TS_REFLT,
	TS_COUNT
};
static VkQueryPool gTsPool;
static double gTsAccumMs[TS_COUNT];	// per-stage sums (deltas)
static uint32 gTsFrames;
static bool gTsWrittenLastFrame;
static float gTsPeriodMs;

static void
timestampsReadPrevious(void)
{
	if(!gTsWrittenLastFrame)
		return;
	gTsWrittenLastFrame = false;
	uint64_t ts[TS_COUNT];
	if(vkGetQueryPoolResults(gVk.device, gTsPool, 0, TS_COUNT, sizeof(ts), ts,
	   sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
		return;
	for(int i = 1; i < TS_COUNT; i++)
		gTsAccumMs[i] += (double)(ts[i] - ts[i-1]) * gTsPeriodMs;
	gTsFrames++;
}

static bool
timestampsBegin(VkCommandBuffer cmd)
{
	if(gTsPool == VK_NULL_HANDLE){
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(gVk.physicalDevice, &props);
		if(props.limits.timestampComputeAndGraphics == VK_FALSE)
			return false;
		gTsPeriodMs = props.limits.timestampPeriod * 1e-6f;
		VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qi.queryCount = TS_COUNT;
		if(vkCreateQueryPool(gVk.device, &qi, nullptr, &gTsPool) != VK_SUCCESS)
			return false;
	}
	vkCmdResetQueryPool(cmd, gTsPool, 0, TS_COUNT);
	return true;
}

static void
timestamp(VkCommandBuffer cmd, int which)
{
	if(gTsPool)
		vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gTsPool, which);
}

// dev/testing: dump the backbuffer every N frames (0 = off; config
// "shotframes=N"). OS screen capture can't see the fullscreen GL frontbuffer,
// so automated runs read these instead.
static int32 gnShotFrames;

static void readConfigFile(void);

// dev-harness state changes (teleport, clock/weather pinning) tick on their
// own counter so vanilla (enabled=0) comparison runs land in the same scene
static void
devHarnessTick(void)
{
	static uint32 tick;
	tick++;

	// hot-reload: pick up rtgi_config.txt edits mid-run (mtime poll). Lets
	// automated captures arm shotframes only after the scene has settled —
	// dense glReadPixels dumps during load/teleport livelock the pipeline.
	if((tick % 30) == 0){
		static FILETIME appliedMtime;
		static bool haveMtime;
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if(GetFileAttributesExA("rtgi_config.txt", GetFileExInfoStandard, &fad)){
			if(!haveMtime){
				// boot-time apply already happened in Initialise
				appliedMtime = fad.ftLastWriteTime;
				haveMtime = true;
			}else if(CompareFileTime(&fad.ftLastWriteTime, &appliedMtime) != 0){
				appliedMtime = fad.ftLastWriteTime;
				readConfigFile();
				RtgiLog("RTGI: config file re-applied (hot-reload)\n");
			}
		}
	}

	if(gWantTeleport && tick >= 150){
		CPlayerPed *player = FindPlayerPed();
		if(player){
			// main map by default; "area=N" targets an interior instead
			CGame::currArea = gnForceArea;
			player->m_area = gnForceArea;
			player->Teleport(CVector(gTeleport[0], gTeleport[1], gTeleport[2]));
			if(gTeleportHasHeading){
				// heading in degrees, 0 = north (+y); snap the follow
				// camera behind so shots frame a known direction
				float a = DEGTORAD(gTeleport[3]);
				player->m_fRotationCur = a;
				player->m_fRotationDest = a;
				player->GetMatrix().SetRotateZOnly(a);
				TheCamera.SetCameraDirectlyBehindForFollowPed_CamOnAString();
			}
			RtgiLog("RTGI: teleported player to %.0f %.0f %.0f (area %d heading %.0f)\n",
				gTeleport[0], gTeleport[1], gTeleport[2], gnForceArea,
				gTeleportHasHeading ? gTeleport[3] : -1.0f);
		}
		gWantTeleport = false;
		gResetGIHistory = true;
	}
	if(gnForceHour >= 0 && tick >= 150 && CClock::GetHours() != gnForceHour){
		if(tick == 150)
			RtgiLog("RTGI: forcing clock to %d:00 (was %d:%02d)\n",
				gnForceHour, CClock::GetHours(), CClock::GetMinutes());
		CClock::GetHoursRef() = gnForceHour;
		CClock::GetMinutesRef() = 0;
	}
	static int32 appliedWeather = -1;
	if(gnForceWeather >= 0 && tick >= 160 && gnForceWeather != appliedWeather){
		CWeather::ForceWeatherNow((int16)gnForceWeather);
		RtgiLog("RTGI: forced weather %d\n", gnForceWeather);
		appliedWeather = gnForceWeather;
	}

	// deterministic capture cameras: play a cutscene spline at the
	// configured offset once the world has settled
	if(gWantCutscene && !gCutsceneStarted && tick >= 250){
		CCutsceneMgr::LoadCutsceneData(gCutsceneName);
		CCutsceneMgr::SetCutsceneOffset(CVector(gCutsceneOfs[0], gCutsceneOfs[1], gCutsceneOfs[2]));
		CCutsceneMgr::ms_cutsceneLoadStatus = 1;
		gCutsceneStarted = true;
		gResetGIHistory = true;
		RtgiLog("RTGI: cutscene %s started at %.0f %.0f %.0f (loop=%d)\n",
			gCutsceneName, gCutsceneOfs[0], gCutsceneOfs[1], gCutsceneOfs[2], gbCutsceneLoop);
	}
	if(gCutsceneStarted && gbCutsceneLoop && CCutsceneMgr::HasLoaded() &&
	   CCutsceneMgr::HasCutsceneFinished())
		CCutsceneMgr::SetupCutsceneToStart();

	// periodic scripted explosion for combat-light verification
	if(gWantExplosions && tick >= 300 && (tick % 150) == 0){
		CExplosion::AddExplosion(nil, nil, EXPLOSION_GRENADE,
			CVector(gExplodePos[0], gExplodePos[1], gExplodePos[2]), 0);
		RtgiLog("RTGI: dev explosion at %.0f %.0f %.0f\n",
			gExplodePos[0], gExplodePos[1], gExplodePos[2]);
	}
}

static void
screenshotDump(void)
{
	// own counter, not frameCounter: that one only advances when RT frames
	// are submitted, and vanilla (enabled=0) comparison runs must dump too
	static uint32 dumpCounter;
	dumpCounter++;
	if(gnShotFrames <= 0 || (dumpCounter % gnShotFrames) != 0)
		return;

	GLint vp[4];
	glGetIntegerv(GL_VIEWPORT, vp);
	int w = vp[2] & ~3, h = vp[3];	// row-align width to 4 for BMP
	uint8 *pixels = (uint8*)malloc(w*h*3);
	if(pixels == nil)
		return;
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(vp[0], vp[1], w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels);

	static int shotIndex;
	char name[64];
	sprintf(name, "rtgi_shot_%d.bmp", shotIndex % 4);
	shotIndex++;

	// minimal BMP24; GL's bottom-up rows match BMP's layout directly
	FILE *f = fopen(name, "wb");
	if(f){
		int imgSize = w*h*3;
		uint8 fileHdr[14] = { 'B','M', 0,0,0,0, 0,0, 0,0, 54,0,0,0 };
		uint32 fileSize = 54 + imgSize;
		memcpy(fileHdr+2, &fileSize, 4);
		uint8 infoHdr[40] = { 40,0,0,0 };
		memcpy(infoHdr+4, &w, 4);
		memcpy(infoHdr+8, &h, 4);
		infoHdr[12] = 1;	// planes
		infoHdr[14] = 24;	// bpp
		memcpy(infoHdr+20, &imgSize, 4);
		fwrite(fileHdr, 1, 14, f);
		fwrite(infoHdr, 1, 40, f);
		fwrite(pixels, 1, imgSize, f);
		fclose(f);
	}
	free(pixels);
}

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
"uniform int u_mode;\n"	// 0 = rgb, 1 = replicate red, 2 = normals 0.5+0.5, 3 = depth vis
"void main() {\n"
"	vec4 t = texture(tex, v_uv);\n"
"	vec3 c = t.rgb;\n"
"	if(u_mode == 1) c = vec3(t.r);\n"
"	else if(u_mode == 2) c = t.rgb*0.5 + 0.5;\n"
"	else if(u_mode == 3) c = vec3(exp(-t.r*0.01));\n"
"	else if(u_mode == 4) c = vec3(t.g);\n"
"	color = vec4(c, 1.0);\n"
"}\n";

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

// dev/testing: optional rtgi_config.txt next to the exe overrides the toggles
// (one "key=value" per line) so automated runs can A/B without the debug menu
static void
readConfigFile(void)
{
	FILE *f = fopen("rtgi_config.txt", "r");
	if(f == nil)
		return;
	char line[128];
	while(fgets(line, sizeof(line), f)){
		int ival; float fval;
		if(sscanf(line, "view=%d", &ival) == 1) gnDebugView = ival;
		else if(sscanf(line, "enabled=%d", &ival) == 1) gbRayTracedGI = ival != 0;
		else if(sscanf(line, "ao=%d", &ival) == 1) gbAOEnable = ival != 0;
		else if(sscanf(line, "aostrength=%f", &fval) == 1) gfAOStrength = fval;
		else if(sscanf(line, "aoradius=%f", &fval) == 1) gfAORadius = fval;
		else if(sscanf(line, "aorays=%d", &ival) == 1) gnAORays = ival;
		else if(sscanf(line, "sunshadows=%d", &ival) == 1) gbSunShadows = ival != 0;
		else if(sscanf(line, "moonshadows=%d", &ival) == 1) gbMoonShadows = ival != 0;
		else if(sscanf(line, "gi=%d", &ival) == 1) gbGIEnable = ival != 0;
		else if(sscanf(line, "giblend=%f", &fval) == 1) gfGIBlend = fval;
		else if(sscanf(line, "giexposure=%f", &fval) == 1) gfGIExposure = fval;
		else if(sscanf(line, "denoise=%d", &ival) == 1) gbDenoise = ival != 0;
		else if(sscanf(line, "reflections=%d", &ival) == 1) gbReflections = ival != 0;
		else if(sscanf(line, "reflfilter=%d", &ival) == 1) gbReflFilter = ival != 0;
		else if(sscanf(line, "glassrefl=%d", &ival) == 1) gbGlassRefl = ival != 0;
		else if(sscanf(line, "dumptex=%d", &ival) == 1) gbDumpTex = ival != 0;
		else if(sscanf(line, "watercaustics=%d", &ival) == 1) gbWaterCaustics = ival != 0;
		else if(sscanf(line, "emissive=%f", &fval) == 1) gfEmissiveBoost = fval;
		else if(sscanf(line, "gi2=%d", &ival) == 1) gbGI2 = ival != 0;
		else if(sscanf(line, "photo=%d", &ival) == 1) gbPhotoMode = ival != 0;
		else if(sscanf(line, "checker=%d", &ival) == 1) gbCheckerGI = ival != 0;
		else if(sscanf(line, "shotframes=%d", &ival) == 1) gnShotFrames = ival;
		else if(sscanf(line, "tp=%f,%f,%f,%f", &gTeleport[0], &gTeleport[1], &gTeleport[2], &gTeleport[3]) == 4){
			gWantTeleport = true;
			gTeleportHasHeading = true;
		}
		else if(sscanf(line, "tp=%f,%f,%f", &gTeleport[0], &gTeleport[1], &gTeleport[2]) == 3) gWantTeleport = true;
		else if(sscanf(line, "cutscene=%11[^,\n],%f,%f,%f", gCutsceneName,
		   &gCutsceneOfs[0], &gCutsceneOfs[1], &gCutsceneOfs[2]) == 4) gWantCutscene = true;
		else if(sscanf(line, "cutloop=%d", &ival) == 1) gbCutsceneLoop = ival != 0;
		else if(sscanf(line, "explode=%f,%f,%f", &gExplodePos[0], &gExplodePos[1], &gExplodePos[2]) == 3)
			gWantExplosions = true;
		else if(sscanf(line, "area=%d", &ival) == 1) gnForceArea = ival;
		else if(sscanf(line, "hour=%d", &ival) == 1) gnForceHour = ival;
		else if(sscanf(line, "weather=%d", &ival) == 1) gnForceWeather = ival;
	}
	fclose(f);
	RtgiLog("RTGI: config file applied (view=%d ao=%d strength=%.2f)\n",
		gnDebugView, gbAOEnable, gfAOStrength);
}

void
Initialise(void)
{
	if(initialised)
		return;

	readConfigFile();

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
		if(!GbufferInit(RsGlobal.maximumWidth, RsGlobal.maximumHeight))
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
		GbufferShutdown();
		PassesShutdown();
		TlasShutdown();
		BlasShutdown();
	}
	if(gTsPool){
		vkDestroyQueryPool(gVk.device, gTsPool, nullptr);
		gTsPool = VK_NULL_HANDLE;
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
	// video-mode change: every size-dependent resource (interop images,
	// G-buffer, pass images) is baked to the old resolution — rebuild the
	// whole RT stack through the proven startup/exit paths. Checked even
	// while the master toggle is off so re-enabling finds sane sizes.
	{
		static int32 lastW, lastH;
		if(lastW == 0){
			lastW = RsGlobal.maximumWidth;
			lastH = RsGlobal.maximumHeight;
		}
		if(initialised &&
		   (RsGlobal.maximumWidth != lastW || RsGlobal.maximumHeight != lastH)){
			RtgiLog("RTGI: resolution changed %dx%d -> %dx%d, recreating\n",
				lastW, lastH, RsGlobal.maximumWidth, RsGlobal.maximumHeight);
			bool wasEnabled = gbRayTracedGI;
			Shutdown();
			Initialise();
			gbRayTracedGI = wasEnabled && initialised;
			gResetGIHistory = true;
			lastW = RsGlobal.maximumWidth;
			lastH = RsGlobal.maximumHeight;
		}
	}

	if(!initialised || !gbRayTracedGI)
		return;

	// drop GI history on interior/level changes (bulk geometry swaps)
	{
		static int prevArea = -1;
		if(CGame::currArea != prevArea){
			prevArea = CGame::currArea;
			gResetGIHistory = true;
		}
	}

	// don't re-record while the previous frame's VK work is in flight.
	// A bounded wait keeps a wedged/lost device from hanging the game.
	VkResult waitRes = vkWaitForFences(gVk.device, 1, &gVk.frameFence, VK_TRUE, 2000000000ull);
	if(waitRes != VK_SUCCESS){
		RtgiLog("RTGI: frame fence wait failed (%d) — disabling ray tracing\n",
			(int)waitRes);
		gbRayTracedGI = false;
		return;
	}
	vkResetFences(gVk.device, 1, &gVk.frameFence);
	timestampsReadPrevious();

	// GL: G-buffer prepass for this camera, handed to VK via semaphore
	bool gbufDone = false;
	if(gVk.hasRayTracing && gbAOEnable){
		GbufferRender();
		InteropSignalGbufDone();
		gbufDone = true;
	}

	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(gVk.cmdBuf, &begin);

	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	bool wantTrace = gVk.hasRayTracing;
	bool traced = false;
	bool timing = false;
	if(wantTrace){
		BlasBeginFrame(gVk.cmdBuf);
		timing = timestampsBegin(gVk.cmdBuf);
		if(timing) timestamp(gVk.cmdBuf, TS_BEGIN);
		TexCacheEnsureDummy(gVk.cmdBuf);	// slot 0 backs unused array entries
		TlasCollect(gVk.cmdBuf);	// walks game world, queues BLAS builds
		BlasEndFrame(gVk.cmdBuf);	// compacted-size queries for new builds
		traced = TlasBuild(gVk.cmdBuf);
	}
	if(timing) timestamp(gVk.cmdBuf, TS_TLAS);

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

		// primary debug view only when someone is looking at it
		if(gnDebugView >= DEBUGVIEW_RT_NORMALS && gnDebugView <= DEBUGVIEW_RT_INSTANCES){
			uint32_t mode = 1;
			if(gnDebugView == DEBUGVIEW_RT_DEPTH) mode = 2;
			else if(gnDebugView == DEBUGVIEW_RT_INSTANCES) mode = 3;
			PassesTracePrimary(gVk.cmdBuf, mode, frameCounter);
		}

		// AO pass, consuming the G-buffer
		if(gbufDone){
			VkImageMemoryBarrier aoToGeneral = toGeneral;
			aoToGeneral.image = gInterop.aoOutput.image;
			vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &aoToGeneral);
			// gbuffer images were left in GENERAL by the semaphore import
			PassesTraceAO(gVk.cmdBuf, frameCounter, (uint32_t)gnAORays, gfAORadius, gbGIEnable);
			if(timing) timestamp(gVk.cmdBuf, TS_AO);

			// diffuse GI + temporal accumulation
			if(gbGIEnable){
				VkImageMemoryBarrier giToGeneral = toGeneral;
				giToGeneral.image = gInterop.giOutput.image;
				vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					0, 0, nullptr, 0, nullptr, 1, &giToGeneral);
				// gResetGIHistory is cleared after the reflection
				// filter below — it consumes the same flag
				PassesTraceGI(gVk.cmdBuf, frameCounter, gResetGIHistory);
				if(timing) timestamp(gVk.cmdBuf, TS_GI);
				// photo mode: the converged average must not be blurred
				if(gbDenoise && !gbPhotoMode)
					PassesDenoiseGI(gVk.cmdBuf);
				if(timing) timestamp(gVk.cmdBuf, TS_DENOISE);
			}else{
				if(timing) timestamp(gVk.cmdBuf, TS_GI);
				if(timing) timestamp(gVk.cmdBuf, TS_DENOISE);
			}

			// reflections
			if(gbReflections){
				VkImageMemoryBarrier reflToGeneral = toGeneral;
				reflToGeneral.image = gInterop.reflOutput.image;
				vkCmdPipelineBarrier(gVk.cmdBuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					0, 0, nullptr, 0, nullptr, 1, &reflToGeneral);
				PassesTraceReflections(gVk.cmdBuf, frameCounter, gbReflFilter);
				if(timing) timestamp(gVk.cmdBuf, TS_REFL);
				if(gbReflFilter)
					PassesFilterReflections(gVk.cmdBuf, frameCounter, gResetGIHistory);
			}else{
				if(timing) timestamp(gVk.cmdBuf, TS_REFL);
			}
			if(timing) timestamp(gVk.cmdBuf, TS_REFLT);
			gResetGIHistory = false;
		}

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

	// only frames that wrote every stamp are readable next frame
	gTsWrittenLastFrame = timing && traced && gbufDone;

	if(traced && (frameCounter % 300) == 0){
		RtgiLog("RTGI: %u TLAS instances, %d BLASes (%u MB compacted away), %u GI lights (%u headlights), %u cached textures, %u glass-marked world meshes (paused=%d menu=%d fade=%d)\n",
			TlasInstanceCount(), BlasCount(), BlasCompactionSavedMB(), GiLightCount(), GiHeadlightCount(), TexCacheCount(),
			EnvGlassMeshCount(),
			CTimer::GetIsPaused(), FrontEndMenuManager.m_bMenuActive, CDraw::FadeValue);
		if(gTsFrames > 0){
			RtgiLog("RTGI: GPU ms avg over %u frames: blas/tlas %.2f, ao %.2f, gi %.2f, denoise %.2f, refl %.2f, reflt %.2f\n",
				gTsFrames,
				gTsAccumMs[TS_TLAS]/gTsFrames, gTsAccumMs[TS_AO]/gTsFrames,
				gTsAccumMs[TS_GI]/gTsFrames, gTsAccumMs[TS_DENOISE]/gTsFrames,
				gTsAccumMs[TS_REFL]/gTsFrames, gTsAccumMs[TS_REFLT]/gTsFrames);
			memset(gTsAccumMs, 0, sizeof(gTsAccumMs));
			gTsFrames = 0;
		}
	}

	VkSemaphore waitSems[2];
	VkPipelineStageFlags waitStages[2];
	uint32_t numWaits = 0;
	// wait until GL is done reading the previous frame's output
	if(!gInterop.firstFrame){
		waitSems[numWaits] = gInterop.semGlDone;
		waitStages[numWaits] = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		numWaits++;
	}
	// wait until GL finished the G-buffer prepass
	if(gbufDone){
		waitSems[numWaits] = gInterop.semGbufDone;
		waitStages[numWaits] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		numWaits++;
	}

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &gVk.cmdBuf;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &gInterop.semRtDone;
	submit.waitSemaphoreCount = numWaits;
	submit.pWaitSemaphores = waitSems;
	submit.pWaitDstStageMask = waitStages;
	gInterop.firstFrame = false;

	VkResult submitRes = vkQueueSubmit(gVk.queue, 1, &submit, gVk.frameFence);
	if(submitRes != VK_SUCCESS){
		// device lost (driver reset, TDR): disable ray tracing for the
		// session instead of hanging the GL side on a semaphore that
		// will never signal. The game continues vanilla.
		RtgiLog("RTGI: vkQueueSubmit failed (%d) — device lost? Disabling ray tracing\n",
			(int)submitRes);
		gbRayTracedGI = false;
		gFrameSubmitted = false;
		return;
	}
	frameCounter++;
	gFrameSubmitted = true;

	// GL: block the pipe until VK results are ready (server-side)
	InteropWaitRtDone();
}

void
DebugRender(void)
{
	devHarnessTick();

	// keyed off gFrameSubmitted, not the toggles: the debug menu can flip
	// them between RenderFrame and here, and the semaphore pair must stay
	// balanced or the next submit deadlocks
	if(!initialised || !gFrameSubmitted){
		screenshotDump();	// still dump: vanilla comparison runs need shots
		return;
	}

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
		GLuint tex = gInterop.rtOutput.glTexture;
		int mode = 0;
		switch(gnDebugView){
		case DEBUGVIEW_AO: tex = gInterop.aoOutput.glTexture; mode = 1; break;
		case DEBUGVIEW_GB_NORMAL: tex = gInterop.gbNormal.glTexture; mode = 2; break;
		case DEBUGVIEW_GB_DEPTH: tex = gInterop.gbDepth.glTexture; mode = 3; break;
		case DEBUGVIEW_SUNVIS: tex = gInterop.aoOutput.glTexture; mode = 4; break;
		case DEBUGVIEW_GI: tex = gInterop.giOutput.glTexture; mode = 0; break;
		case DEBUGVIEW_REFL: tex = gInterop.reflOutput.glTexture; mode = 0; break;
		}
		glUseProgram(blitProgram);
		glBindVertexArray(blitVAO);
		glBindTexture(GL_TEXTURE_2D, tex);
		glUniform1i(glGetUniformLocation(blitProgram, "tex"), 0);
		glUniform1i(glGetUniformLocation(blitProgram, "u_mode"), mode);
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

	screenshotDump();
}

bool
ReplacingVehicleShadows(void)
{
	return initialised && gbRayTracedGI && gbAOEnable && gbSunShadows;
}

// moon brightness 0..1: VC draws the moon as a camera-relative sprite due
// south (offset 0,-100,15), fully lit at 3AM, fading over +/-3h, dimmed
// by cloud/fog coverage (mirrors CClouds::Render). Moon shadows trace the
// same fixed direction with this as their strength.
float
MoonShadowStrength(void)
{
	if(!gbMoonShadows || CTimeCycle::GetSunDirection().z > 0.0f)
		return 0.0f;
	float minute = CClock::GetHours()*60 + CClock::GetMinutes() + CClock::GetSeconds()/60.0f;
	float fade = Abs(minute - 180.0f);
	if(fade >= 180.0f)
		return 0.0f;
	float coverage = Max(CWeather::Foggyness, CWeather::CloudCoverage);
	return (1.0f - coverage) * (1.0f - fade/180.0f);
}

// to-moon unit vector matching the sprite placement
void
MoonDirection(float dir[3])
{
	dir[0] = 0.0f;
	dir[1] = -0.98894f;	// normalize(0, -100, 15)
	dir[2] = 0.14834f;
}

void
AddDebugMenuEntries(void)
{
	static const char *debugViews[] = { "Off", "Interop", "RT Normals", "RT Depth", "RT Instances", "AO", "GB Normal", "GB Depth", "Sun Vis", "GI", "Reflections" };
	DebugMenuAddVarBool8("RTGI", "Ray traced GI", (int8_t*)&gbRayTracedGI, nil);
	DebugMenuAddVar("RTGI", "Debug view", &gnDebugView, nil, 1, 0, DEBUGVIEW_MAX-1, debugViews);
	DebugMenuAddVarBool8("RTGI", "RT ambient occlusion", (int8_t*)&gbAOEnable, nil);
	DebugMenuAddVarBool8("RTGI", "RT sun shadows", (int8_t*)&gbSunShadows, nil);
	DebugMenuAddVarBool8("RTGI", "RT moon shadows", (int8_t*)&gbMoonShadows, nil);
	DebugMenuAddVarBool8("RTGI", "Diffuse GI", (int8_t*)&gbGIEnable, nil);
	DebugMenuAddVar("RTGI", "GI blend", &gfGIBlend, nil, 0.05f, 0.0f, 1.0f);
	DebugMenuAddVar("RTGI", "GI exposure", &gfGIExposure, nil, 0.1f, 0.1f, 5.0f);
	DebugMenuAddVarBool8("RTGI", "GI denoise", (int8_t*)&gbDenoise, nil);
	DebugMenuAddVarBool8("RTGI", "RT reflections", (int8_t*)&gbReflections, nil);
	DebugMenuAddVarBool8("RTGI", "Reflection filter", (int8_t*)&gbReflFilter, nil);
	DebugMenuAddVarBool8("RTGI", "Glass reflections", (int8_t*)&gbGlassRefl, nil);
	DebugMenuAddVarBool8("RTGI", "Water caustics", (int8_t*)&gbWaterCaustics, nil);
	DebugMenuAddVar("RTGI", "Emissive boost", &gfEmissiveBoost, nil, 0.25f, 0.0f, 8.0f);
	DebugMenuAddVarBool8("RTGI", "GI second bounce", (int8_t*)&gbGI2, nil);
	DebugMenuAddVarBool8("RTGI", "Photo mode (accumulate)", (int8_t*)&gbPhotoMode, nil);
	DebugMenuAddVarBool8("RTGI", "GI checkerboard (perf)", (int8_t*)&gbCheckerGI, nil);
	DebugMenuAddVar("RTGI", "AO strength", &gfAOStrength, nil, 0.05f, 0.0f, 1.0f);
	DebugMenuAddVar("RTGI", "AO radius", &gfAORadius, nil, 0.5f, 0.5f, 10.0f);
	DebugMenuAddVar("RTGI", "AO rays", &gnAORays, nil, 1, 1, 8, nil);
}

}

#endif
