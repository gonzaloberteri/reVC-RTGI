#ifdef RTGI

#include <windows.h>
#include <stdio.h>

#include "interop.h"
#include "gbuffer.h"

#include <glad/glad.h>

#include "common.h"
#include <rwcore.h>
#include <rpworld.h>
#include "rtgi.h"
#include "Renderer.h"
#include "Entity.h"
#include "World.h"
#include "Timecycle.h"

namespace RayTracedGI {

static GLuint gFbo;
static GLuint gDepthRbo;
static rw::gl3::Shader *gGbufShader;
static rw::gl3::Shader *gWorldShader;
static int gWidth, gHeight;

static int32 u_aoTex;
static int32 u_rtgiParams;
static int32 u_giTex;
static int32 u_rtgiGIParams;
static int32 u_gbParams;
static int32 u_reflTex;
static int32 u_rtgiReflParams;

#define U(i) (rw::gl3::currentShader->uniformLocations[i])

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
#include "shaders/obj/rtgiWorld_vert.inc"
#include "shaders/obj/rtgiWorld_frag.inc"
	const char *vs[] = { shaderDecl, header_vert_src, rtgiWorld_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, rtgiWorld_frag_src, nil };
	gWorldShader = Shader::create(vs, fs);
	if(gWorldShader == nil)
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
	return true;
}

void
GbufferShutdown(void)
{
	if(gGbufShader){ gGbufShader->destroy(); gGbufShader = nil; }
	if(gWorldShader){ gWorldShader->destroy(); gWorldShader = nil; }
	if(gFbo){ glDeleteFramebuffers(1, &gFbo); gFbo = 0; }
	if(gDepthRbo){ glDeleteRenderbuffers(1, &gDepthRbo); gDepthRbo = 0; }
}

static void
gbufDrawAtomic(rw::Atomic *atomic)
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

	setWorldMatrix(atomic->getFrame()->getLTM());
	setupVertexInput(header);

	InstanceData *inst = header->inst;
	for(rw::uint32 i = 0; i < header->numMeshes; i++, inst++){
		rw::Material *m = inst->material;
		// opaque meshes only; transparency can't occlude reliably
		if(inst->vertexAlpha || m->color.alpha != 255)
			continue;
		drawInst(header, inst);
	}
	teardownVertexInput(header);
}

void
GbufferRender(void)
{
	using namespace rw::gl3;

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

	gGbufShader->use();

	// world + vehicles from the renderer's visible set; peds are skinned
	// (different vertex layout) and excluded until M9
	for(int32 i = 0; i < CRenderer::GetNoOfVisibleEntities(); i++){
		CEntity *e = CRenderer::GetVisibleEntity(i);
		if(e->m_rwObject == nil || e->IsPed())
			continue;

		// vehicles are reflective (RT replacement for the env map look)
		float refl[4] = { e->IsVehicle() ? 0.35f : 0.0f, 0.0f, 0.0f, 0.0f };
		glUniform4fv(U(u_gbParams), 1, refl);

		if(RwObjectGetType(e->m_rwObject) == rpATOMIC)
			gbufDrawAtomic((rw::Atomic*)e->m_rwObject);
		else{
			rw::Clump *clump = (rw::Clump*)e->m_rwObject;
			FORLIST(lnk, clump->atomics)
				gbufDrawAtomic(rw::Atomic::fromClump(lnk));
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
	glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
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
	float shadowStrength = gbSunShadows ? (CTimeCycle::GetShadowStrength()/255.0f)*0.55f : 0.0f;
	float params[4] = { gfAOStrength, 1.0f/gWidth, 1.0f/gHeight, shadowStrength };
	glUniform4fv(U(u_rtgiParams), 1, params);
	float giParams[4] = { gbGIEnable ? gfGIBlend : 0.0f, gfGIExposure, 0.0f, 0.0f };
	glUniform4fv(U(u_rtgiGIParams), 1, giParams);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		Material *m = inst->material;

		RGBA color = { 255, 255, 255, m->color.alpha };
		setMaterial(color, m->surfaceProps);
		setTexture(0, m->texture);

		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

		drawInst(header, inst);
		inst++;
	}
	teardownVertexInput(header);
	return true;
}

}

#endif
