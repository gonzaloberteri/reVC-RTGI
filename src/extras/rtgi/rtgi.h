#pragma once

#ifdef RTGI

// Ray traced global illumination via a Vulkan side-car next to the librw gl3
// backend. Results are shared with GL through GL_EXT_memory_object_win32.

namespace RayTracedGI {

enum eDebugView {
	DEBUGVIEW_OFF = 0,
	DEBUGVIEW_INTEROP,	// show the VK-written shared image (M1 interop proof)
	DEBUGVIEW_RT_NORMALS,	// primary rays: geometric normals
	DEBUGVIEW_RT_DEPTH,	// primary rays: depth
	DEBUGVIEW_RT_INSTANCES,	// primary rays: instance id colors
	DEBUGVIEW_AO,		// ray traced ambient occlusion
	DEBUGVIEW_GB_NORMAL,	// G-buffer world normals
	DEBUGVIEW_GB_DEPTH,	// G-buffer linear depth
	DEBUGVIEW_SUNVIS,	// ray traced sun visibility
	DEBUGVIEW_GI,		// accumulated diffuse GI radiance
	DEBUGVIEW_REFL,		// reflections
	DEBUGVIEW_VOL,		// volumetric light shafts
	DEBUGVIEW_MAX
};

// runtime master switch; forced false forever if init fails
extern bool gbRayTracedGI;
extern int32 gnDebugView;
// AO controls
extern bool gbAOEnable;
extern float gfAOStrength;	// 0 = off, 1 = full ambient modulation
extern float gfAORadius;
extern int32 gnAORays;
// RT sun shadows (vehicle blob-shadow replacement)
extern bool gbSunShadows;
extern bool gbMoonShadows;

// moon shadow support (rtgi.cpp): brightness 0..1 and to-moon direction
float MoonShadowStrength(void);
void MoonDirection(float dir[3]);
// diffuse GI
extern bool gbGIEnable;
extern float gfGIBlend;		// 0 = flat timecycle ambient, 1 = full GI
extern float gfGIExposure;
extern bool gbDenoise;
extern bool gbReflections;
extern bool gbReflFilter;	// temporal + spatial filter over the reflection buffer
extern bool gbGlassRefl;	// deterministic Fresnel mirror on vehicle glass
extern bool gbDumpTex;	// dev: log distinct world texture names
extern bool gbWaterCaustics;	// animated caustic shimmer on the water surface
extern float gfEmissiveBoost;	// night windows/neon radiance multiplier
extern bool gbGI2;		// second GI bounce (Russian roulette)
extern bool gbPhotoMode;	// progressive accumulation while the camera is still
extern bool gbCheckerGI;	// trace GI on alternating pixels (perf knob)

// true when RT shadows replace the vehicle blob shadows this frame
bool ReplacingVehicleShadows(void);

// call once the GL context exists (after RW init); safe to call when unsupported
void Initialise(void);
void Shutdown(void);

// per-frame: run the Vulkan work for this frame (no-op when disabled)
void RenderFrame(void);
// draw debug visualization overlay (called late in the frame, before 2D)
void DebugRender(void);

// hook up debug menu entries (called from DebugMenuPopulate)
void AddDebugMenuEntries(void);

}

#endif
