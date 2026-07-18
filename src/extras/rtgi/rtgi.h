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
