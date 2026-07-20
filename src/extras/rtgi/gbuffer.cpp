#ifdef RTGI

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "interop.h"
#include "gbuffer.h"

#include <glad/glad.h>

#include "common.h"
#include <rwcore.h>
#include <rpworld.h>
#include <rpmatfx.h>
#include <rpskin.h>
#include "rtgi.h"
#include "Renderer.h"
#include "Entity.h"
#include "World.h"
#include "Timecycle.h"
#include "ModelInfo.h"
#include "WaterLevel.h"
#include "Weather.h"
#include "Timer.h"
#include "Camera.h"

namespace RayTracedGI {

static GLuint gFbo;
static GLuint gDepthRbo;
static rw::gl3::Shader *gGbufShader;
static rw::gl3::Shader *gGbufSkinShader;
static rw::gl3::Shader *gWorldShader;
static rw::gl3::Shader *gVehicleShader;
static rw::gl3::Shader *gSkinShader;
static rw::gl3::Shader *gWaterShader;
static int gWidth, gHeight;

// true while the sea is being re-rendered into the G-buffer; WaterLevel
// skips its texture-anim advance so the real pass keeps vanilla speed
bool gbWaterGbufPass;

static int32 u_aoTex;
static int32 u_rtgiParams;
static int32 u_giTex;
static int32 u_rtgiGIParams;
static int32 u_gbParams;
static int32 u_reflTex;
static int32 u_rtgiReflParams;
static int32 u_rtgiVehParams;
static int32 u_rtgiWaterCam;

#define U(i) (rw::gl3::currentShader->uniformLocations[i])

// stock-matFX pipeline interception (defined below)
static void matfxRenderCBHook(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header);
static void (*gOrigMatfxCB)(rw::Atomic*, rw::gl3::InstanceDataHeader*);

// dev aid (dumptex=1): log each distinct world-mesh texture name once, to
// mine the vocabulary for name-based material heuristics (VC world models
// carry no matFX env maps — texture names are the only glass signal)
static unsigned int gEnvGlassMeshes;

unsigned int EnvGlassMeshCount(void) { return gEnvGlassMeshes; }

static void
dumpTexName(rw::gl3::InstanceData *inst)
{
	enum { MAX_NAMES = 512 };
	static char seen[MAX_NAMES][32];
	static int numSeen;
	rw::Material *m = inst->material;
	if(m->texture == nil || numSeen >= MAX_NAMES)
		return;
	const char *name = m->texture->name;
	for(int i = 0; i < numSeen; i++)
		if(strncmp(seen[i], name, 32) == 0)
			return;
	strncpy(seen[numSeen++], name, 32);
	RtgiLog("RTGI: tex %s\n", name);
}

bool
GbufferInit(int width, int height)
{
	using namespace rw::gl3;

	gWidth = width;
	gHeight = height;

	u_aoTex = registerUniform("u_aoTex");
	u_rtgiParams = registerUniform("u_rtgiParams");
	u_giTex = registerUniform("u_giTex");
	u_rtgiGIParams = registerUniform("u_rtgiGIParams");
	u_gbParams = registerUniform("u_gbParams");
	u_reflTex = registerUniform("u_reflTex");
	u_rtgiReflParams = registerUniform("u_rtgiReflParams");
	u_rtgiVehParams = registerUniform("u_rtgiVehParams");
	u_rtgiWaterCam = registerUniform("u_rtgiWaterCam");

	{
#include "shaders/obj/rtgiGbuf_vert.inc"
#include "shaders/obj/rtgiGbuf_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiGbuf_vert_src, nil };
	const char *fs[] = { shaderDecl, rtgiGbuf_frag_src, nil };
	gGbufShader = Shader::create(vs, fs);
	if(gGbufShader == nil)
		return false;
	}
	{
#include "shaders/obj/rtgiGbufSkin_vert.inc"
#include "shaders/obj/rtgiGbuf_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiGbufSkin_vert_src, nil };
	const char *fs[] = { shaderDecl, rtgiGbuf_frag_src, nil };
	gGbufSkinShader = Shader::create(vs, fs);
	if(gGbufSkinShader == nil)
		return false;
	}
	{
#include "shaders/obj/rtgiWorld_vert.inc"
#include "shaders/obj/rtgiWorld_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiWorld_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, rtgiWorld_frag_src, nil };
	gWorldShader = Shader::create(vs, fs);
	if(gWorldShader == nil)
		return false;
	}
	{
#include "shaders/obj/rtgiWorld_vert.inc"
#include "shaders/obj/rtgiVehicle_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiWorld_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, rtgiVehicle_frag_src, nil };
	gVehicleShader = Shader::create(vs, fs);
	if(gVehicleShader == nil)
		return false;
	}
	{
#include "shaders/obj/rtgiSkin_vert.inc"
#include "shaders/obj/rtgiVehicle_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiSkin_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, rtgiVehicle_frag_src, nil };
	gSkinShader = Shader::create(vs, fs);
	if(gSkinShader == nil)
		return false;
	}
	{
#include "shaders/obj/rtgiWater_vert.inc"
#include "shaders/obj/rtgiWater_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiWater_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, rtgiWater_frag_src, nil };
	gWaterShader = Shader::create(vs, fs);
	if(gWaterShader == nil)
		return false;
	}

	glGenRenderbuffers(1, &gDepthRbo);
	glBindRenderbuffer(GL_RENDERBUFFER, gDepthRbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &gFbo);
	glBindFramebuffer(GL_FRAMEBUFFER, gFbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gInterop.gbNormal.glTexture, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gInterop.gbDepth.glTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gDepthRbo);
	GLenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
	glDrawBuffers(2, bufs);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if(status != GL_FRAMEBUFFER_COMPLETE){
		RtgiLog("RTGI: G-buffer FBO incomplete (0x%x)\n", status);
		return false;
	}

	// intercept the stock matFX pipeline (LOD sea atomics — see hook)
	{
		rw::gl3::ObjPipeline *mp = (rw::gl3::ObjPipeline*)rw::matFXGlobals.pipelines[rw::platform];
		if(mp && mp->renderCB != matfxRenderCBHook){
			gOrigMatfxCB = mp->renderCB;
			mp->renderCB = matfxRenderCBHook;
		}
	}
	return true;
}

void
GbufferShutdown(void)
{
	{
		rw::gl3::ObjPipeline *mp = (rw::gl3::ObjPipeline*)rw::matFXGlobals.pipelines[rw::platform];
		if(mp && mp->renderCB == matfxRenderCBHook)
			mp->renderCB = gOrigMatfxCB;
		gOrigMatfxCB = nil;
	}
	if(gGbufShader){ gGbufShader->destroy(); gGbufShader = nil; }
	if(gGbufSkinShader){ gGbufSkinShader->destroy(); gGbufSkinShader = nil; }
	if(gWorldShader){ gWorldShader->destroy(); gWorldShader = nil; }
	if(gVehicleShader){ gVehicleShader->destroy(); gVehicleShader = nil; }
	if(gSkinShader){ gSkinShader->destroy(); gSkinShader = nil; }
	if(gWaterShader){ gWaterShader->destroy(); gWaterShader = nil; }
	if(gFbo){ glDeleteFramebuffers(1, &gFbo); gFbo = 0; }
	if(gDepthRbo){ glDeleteRenderbuffers(1, &gDepthRbo); gDepthRbo = 0; }
}

static bool
meshHasAlpha(rw::gl3::InstanceData *inst)
{
	rw::Material *m = inst->material;
	if(inst->vertexAlpha || m->color.alpha != 255)
		return true;
	return m->texture && m->texture->raster &&
	   PLUGINOFFSET(rw::gl3::Gl3Raster, m->texture->raster, rw::gl3::nativeRasterOffset)->hasAlpha;
}

// glass panes are TRANSLUCENT (material color alpha < 255); alpha-cutout
// meshes (cargo junk, grilles, decals) are opaque materials with an alpha
// texture and must not become mirrors
static bool
meshIsGlass(rw::gl3::InstanceData *inst)
{
	rw::uint8 a = inst->material->color.alpha;
	return a != 255 && a != 0;
}


static void
gbufDrawAtomic(rw::Atomic *atomic, float reflW, float glassReflW, bool envAsGlass)
{
	using namespace rw::gl3;

	if((atomic->object.object.flags & rw::Atomic::RENDER) == 0)
		return;
	rw::Geometry *geo = atomic->geometry;
	if(geo == nil || geo->flags & rw::Geometry::NATIVE)
		return;

	atomic->getPipeline()->instance(atomic);
	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(header == nil || header->platform != rw::PLATFORM_GL3)
		return;

	// skinned atomics (peds) go through the bone-matrix vertex path
	if(rw::Skin::get(geo)){
		gGbufSkinShader->use();
		uploadSkinMatrices(atomic);
	}else
		gGbufShader->use();

	float refl[4] = { reflW, 0.0f, 0.0f, 0.0f };
	glUniform4fv(U(u_gbParams), 1, refl);
	float boundReflW = reflW;

	setWorldMatrix(atomic->getFrame()->getLTM());
	setupVertexInput(header);

	InstanceData *inst = header->inst;
	for(rw::uint32 i = 0; i < header->numMeshes; i++, inst++){
		// opaque meshes as-is; alpha meshes are normally excluded
		// (transparency can't occlude reliably, and alpha-textured
		// foliage must not become a wet-reflective surface) — except
		// vehicle glass, which enters with the glass marker so the
		// reflection pass gives panes a deterministic Fresnel mirror
		float want = reflW;
		if(meshHasAlpha(inst)){
			// translucent meshes are REAL glass panes (vehicle
			// windows, mall storefronts, bar fronts, breakable
			// shop glass) — baked facade "window" textures are
			// opaque and stay untouched
			if(glassReflW <= 0.0f || !meshIsGlass(inst))
				continue;
			want = glassReflW;
			gEnvGlassMeshes++;
		}else if(envAsGlass && gbDumpTex)
			dumpTexName(inst);
		if(want != boundReflW){
			float p[4] = { want, 0.0f, 0.0f, 0.0f };
			glUniform4fv(U(u_gbParams), 1, p);
			boundReflW = want;
		}
		drawInst(header, inst);
	}
	teardownVertexInput(header);
}

void
GbufferRender(void)
{
	using namespace rw::gl3;

	gEnvGlassMeshes = 0;

	GLint prevFbo, prevViewport[4];
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
	glGetIntegerv(GL_VIEWPORT, prevViewport);

	glBindFramebuffer(GL_FRAMEBUFFER, gFbo);
	glViewport(0, 0, gWidth, gHeight);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0);
	glDepthMask(GL_TRUE);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);

	// world + vehicles + peds from the renderer's visible set
	for(int32 i = 0; i < CRenderer::GetNoOfVisibleEntities(); i++){
		CEntity *e = CRenderer::GetVisibleEntity(i);
		if(e->m_rwObject == nil)
			continue;

		// G-buffer normal.w: < 0 = vehicle base reflectivity (negated),
		// 0..1.5 = wet-weather reflectivity multiplier, 2 = sea,
		// 3 = glass. Roads use the game's own per-model wet-reflection
		// flag; other surfaces get a light sheen; peds never reflect.
		float reflW, glassReflW = 0.0f;
		bool envAsGlass = false;
		if(e->IsVehicle()){
			reflW = -0.35f;
			if(gbGlassRefl && gbReflections)
				glassReflW = 3.0f;
		}else if(e->IsPed())
			reflW = 0.0f;
		else{
			if(e->IsBuilding() &&
			   ((CSimpleModelInfo*)CModelInfo::GetModelInfo(e->GetModelIndex()))->m_wetRoadReflection)
				reflW = 1.0f;
			else
				reflW = 0.25f;
			// translucent world meshes (storefronts, mall glass)
			// are real panes and mirror like vehicle glass — but a
			// distance-fading entity drops its material alpha and
			// must not flash into a mirror mid-fade
			if(gbGlassRefl && gbReflections && !e->bDistanceFade)
				glassReflW = 3.0f;
			envAsGlass = true;
		}

		if(RwObjectGetType(e->m_rwObject) == rpATOMIC)
			gbufDrawAtomic((rw::Atomic*)e->m_rwObject, reflW, glassReflW, envAsGlass);
		else{
			rw::Clump *clump = (rw::Clump*)e->m_rwObject;
			FORLIST(lnk, clump->atomics)
				gbufDrawAtomic(rw::Atomic::fromClump(lnk), reflW, glassReflW, envAsGlass);
		}
	}

	// sea surface: re-render the water through the im3d override so the
	// reflection pass can trace from it (marker reflW = 2). RenderWater
	// covers only sectors beyond 500m; the near (wavy) water lives in
	// RenderTransparentWater. Layer 1 skips the near-camera matFX mask
	// atomic, which would bypass the override and scribble into the
	// attachments; vertex alpha is forced off so nothing blends normals.
	{
		gGbufShader->use();
		float waterParams[4] = { 2.0f, 0.0f, 0.0f, 0.0f };
		glUniform4fv(U(u_gbParams), 1, waterParams);
		int32 prevLayers = CWaterLevel::m_nRenderWaterLayers;
		CWaterLevel::m_nRenderWaterLayers = 1;
		gbWaterGbufPass = true;
		im3dOverrideShader = gGbufShader;
		rw::SetRenderState(rw::VERTEXALPHA, FALSE);
		CWaterLevel::RenderWater();
		CWaterLevel::RenderTransparentWater();
		im3dOverrideShader = nil;
		gbWaterGbufPass = false;
		CWaterLevel::m_nRenderWaterLayers = prevLayers;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
	glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

// bind the RT result textures + shared composite uniforms for whichever
// composite shader is current
static void
uploadCompositeUniforms(void)
{
	using namespace rw::gl3;

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, gInterop.aoOutput.glTexture);
	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, gInterop.giOutput.glTexture);
	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, gInterop.reflOutput.glTexture);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(U(u_aoTex), 3);
	glUniform1i(U(u_giTex), 4);
	glUniform1i(U(u_reflTex), 5);
	float reflParams[4] = { gbReflections ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	glUniform4fv(U(u_rtgiReflParams), 1, reflParams);
	// sun by day; at night the moon casts subtle shadows in the same slot
	float shadowStrength;
	if(CTimeCycle::GetSunDirection().z > 0.0f)
		shadowStrength = gbSunShadows ? (CTimeCycle::GetShadowStrength()/255.0f)*0.55f : 0.0f;
	else
		shadowStrength = MoonShadowStrength()*0.30f;
	float params[4] = { gfAOStrength, 1.0f/gWidth, 1.0f/gHeight, shadowStrength };
	glUniform4fv(U(u_rtgiParams), 1, params);
	// rain: the GI tracks the dark storm sky and reads gloomier than the
	// vanilla art direction, so ease back toward the flat timecycle
	// ambient as the rain comes down
	float giBlend = gbGIEnable ? gfGIBlend * (1.0f - 0.4f*CWeather::Rain) : 0.0f;
	float giParams[4] = { giBlend, gfGIExposure, 0.0f, 0.0f };
	glUniform4fv(U(u_rtgiGIParams), 1, giParams);
}

// the far LOD sea around the islands is WORLD geometry carrying the vanilla
// water texture (baked reflective sparkle) — with the procedural water look
// that texture must go everywhere, or a speckled band rings the horizon
static bool
texIsOGWater(rw::Texture *tex)
{
	if(tex == nil)
		return false;
	char low[33];
	int i;
	for(i = 0; i < 32 && tex->name[i]; i++)
		low[i] = (char)tolower(tex->name[i]);
	low[i] = '\0';
	// NOT the seabed: sandy floor through the shallows is vanilla art (its
	// stripped prelight is a dark checker), and the far-horizon seabed
	// grid is hidden by the opaque-far water ramp anyway
	return strstr(low, "waterclear") != nil;
}

// LOD sea atomics carry matFX and render through librw's STOCK matFX
// pipeline (attachPipe in RwHelper overrides the world-pipe attachment),
// so they dodge WorldRenderCB and keep painting the OG water texture at
// the horizon. Hook the pipeline: atomics carrying the OG water texture
// composite through WorldRenderCB (which strips that texture); everything
// else falls through to the stock callback.
static void
matfxRenderCBHook(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw::gl3;

	if(gbRayTracedGI && gbAOEnable && gbWaterCaustics){
		bool hasOGWater = false;
		InstanceData *inst = header->inst;
		for(rw::int32 i = 0; i < header->numMeshes; i++)
			if(texIsOGWater(inst[i].material->texture)){
				hasOGWater = true;
				break;
			}
		if(hasOGWater){
			static bool logged;
			if(!logged){
				RtgiLog("RTGI: matFX OG-water atomic intercepted\n");
				logged = true;
			}
			if(WorldRenderCB(atomic, header))
				return;
		}
	}
	if(gOrigMatfxCB)
		gOrigMatfxCB(atomic, header);
}

// true while the procedural (shader-only) water look is active
bool
UsingProceduralWater(void)
{
	return gbRayTracedGI && gbAOEnable && gbReflections && gbWaterCaustics;
}

// the neo gloss pipe adds the baked water sparkle additively on top of
// whatever the base pass drew — suppress it wherever the procedural
// water look owns the surface
bool
SuppressOGWaterGloss(rw::Texture *tex)
{
	return UsingProceduralWater() && texIsOGWater(tex);
}

bool
WorldRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	if(!gbRayTracedGI || !gbAOEnable || gWorldShader == nil)
		return false;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);
	setupVertexInput(header);

	gWorldShader->use();
	uploadCompositeUniforms();

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		Material *m = inst->material;

		RGBA color = { 255, 255, 255, m->color.alpha };
		setMaterial(color, m->surfaceProps);
		if(gbWaterCaustics && texIsOGWater(m->texture)){
			static int logged;
			if(logged < 6){
				RtgiLog("RTGI: world pipe stripped OG water tex %s\n", m->texture->name);
				logged++;
			}
			setTexture(0, nil);	// prelight-only flat sea
		}else
			setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
	return true;
}

bool
VehicleRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	if(!gbRayTracedGI || !gbAOEnable || gVehicleShader == nil)
		return false;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);
	setupVertexInput(header);

	gVehicleShader->use();
	uploadCompositeUniforms();

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		Material *m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);
		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		// materials that carried a matFX env map (paint, chrome) get the
		// ray traced reflection instead; everything else stays diffuse.
		// glass meshes (the ones the G-buffer marked with the glass
		// reflW) composite the deterministic Fresnel mirror instead
		float envScale = 0.0f;
		MatFX *matfx = MatFX::get(m);
		if(matfx && matfx->type == MatFX::ENVMAP &&
		   matfx->fx[0].env.tex && matfx->fx[0].env.coefficient > 0.0f)
			envScale = 1.0f;
		float glass = (gbGlassRefl && meshIsGlass(inst)) ? 1.0f : 0.0f;
		float vehParams[4] = { envScale, glass, 0.0f, 0.0f };
		glUniform4fv(U(u_rtgiVehParams), 1, vehParams);

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
	return true;
}

// shared mesh loop for the ped composites (no reflections on skin/cloth)
static void
pedDrawMeshes(rw::uint32 flags, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	float vehParams[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	glUniform4fv(U(u_rtgiVehParams), 1, vehParams);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		Material *m = inst->material;

		setMaterial(flags, m->color, m->surfaceProps);
		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
}

bool
PedRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	if(!gbRayTracedGI || !gbAOEnable || gVehicleShader == nil)
		return false;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);
	setupVertexInput(header);

	gVehicleShader->use();
	uploadCompositeUniforms();

	pedDrawMeshes(atomic->geometry->flags, header);
	return true;
}

// wrap the forward water draws: the im3d override samples the RT
// reflection buffer + animates a caustic shimmer on top of the vanilla
// water look
static void
waterUploadUniforms(bool atomicPath)
{
	using namespace rw::gl3;

	glActiveTexture(GL_TEXTURE5);
	glBindTexture(GL_TEXTURE_2D, gInterop.reflOutput.glTexture);
	glActiveTexture(GL_TEXTURE0);
	glUniform1i(U(u_reflTex), 5);
	// wrap time to keep float precision in the caustic sin/cos towers
	float t = (float)(CTimer::GetTimeInMilliseconds() % 3600000u) * 0.001f;
	float params[4] = { t, 1.0f/gWidth, 1.0f/gHeight, gbWaterCaustics ? 1.0f : 0.0f };
	glUniform4fv(U(u_rtgiParams), 1, params);
	// camera rides in u_gbParams (w = 1 on the atomic path: wavy/mask
	// prelight is static daylight and must be replaced by the timecycle
	// water color); the librw uniform registry (40 slots) is full, so the
	// water pass reuses slots other passes own
	CVector camPos = TheCamera.GetPosition();
	float cam[4] = { camPos.x, camPos.y, camPos.z, atomicPath ? 1.0f : 0.0f };
	glUniform4fv(U(u_gbParams), 1, cam);
	float wcol[4] = { CTimeCycle::GetWaterRed()/255.0f, CTimeCycle::GetWaterGreen()/255.0f,
		CTimeCycle::GetWaterBlue()/255.0f, CTimeCycle::GetWaterAlpha()/255.0f };
	glUniform4fv(U(u_rtgiGIParams), 1, wcol);
}

void
WaterRenderBegin(void)
{
	using namespace rw::gl3;

	if(!gbRayTracedGI || !gbAOEnable || !gbReflections || gWaterShader == nil)
		return;

	gWaterShader->use();
	waterUploadUniforms(false);
	im3dOverrideShader = gWaterShader;
}

void
WaterRenderEnd(void)
{
	rw::gl3::im3dOverrideShader = nil;
}

// the near-camera wavy/mask water renders as ATOMICS, which bypass the
// im3d override — vanilla they pop to the plain texture look right where
// the player can see the water best. Draw them with the same water shader
// (caustics + RT reflection) instead.
bool
RenderWaterAtomic(rw::Atomic *atomic)
{
	using namespace rw::gl3;

	if(!gbRayTracedGI || !gbAOEnable || !gbReflections || gWaterShader == nil)
		return false;

	rw::Geometry *geo = atomic->geometry;
	if(geo == nil || geo->flags & rw::Geometry::NATIVE)
		return false;
	atomic->getPipeline()->instance(atomic);
	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(header == nil || header->platform != rw::PLATFORM_GL3)
		return false;

	gWaterShader->use();
	waterUploadUniforms(true);
	setWorldMatrix(atomic->getFrame()->getLTM());
	setupVertexInput(header);
	InstanceData *inst = header->inst;
	for(rw::uint32 i = 0; i < header->numMeshes; i++, inst++){
		setTexture(0, inst->material->texture);
		drawInst(header, inst);
	}
	teardownVertexInput(header);
	return true;
}

bool
PedSkinRenderCB(rw::Atomic *atomic, rw::gl3::InstanceDataHeader *header)
{
	using namespace rw;
	using namespace rw::gl3;

	if(!gbRayTracedGI || !gbAOEnable || gSkinShader == nil)
		return false;

	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);
	setupVertexInput(header);

	gSkinShader->use();
	uploadCompositeUniforms();
	uploadSkinMatrices(atomic);

	pedDrawMeshes(atomic->geometry->flags, header);
	return true;
}

}

#endif
