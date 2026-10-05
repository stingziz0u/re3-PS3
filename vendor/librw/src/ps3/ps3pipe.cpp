// ps3pipe.cpp -- object pipelines of the RSX renderer: instancing into VRAM
// and the render callbacks of the default, skin and MatFX pipelines
// (librw's gl3 ones: gl3pipe.cpp, gl3render.cpp, gl3skin.cpp, gl3matfx.cpp).
//
// Instancing builds the vertex buffer in main memory and copies it to VRAM in
// one go (the RSX reads it from there; the CPU only writes). Indices are
// 16 bit, native (big-endian), drawn with rsxDrawIndexArray.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwps3.h"
#ifdef RW_PS3_RSX
#include "ps3rsx.h"
#endif

namespace rw {
namespace ps3 {

#ifndef RW_PS3_RSX

void *destroyNativeData(void *object, int32, int32) { return object; }
void initSkin(void) { }
void initMatFX(void) { }

#else

// attribute slots: the ATTRn of the vertex programs (shaders/)
enum {
	ATTR_POS = GCM_VERTEX_ATTRIB_POS,		// 0
	ATTR_WEIGHTS = GCM_VERTEX_ATTRIB_WEIGHT,	// 1
	ATTR_NORMAL = GCM_VERTEX_ATTRIB_NORMAL,		// 2
	ATTR_COLOR = GCM_VERTEX_ATTRIB_COLOR0,		// 3
	ATTR_INDICES = 7,
	ATTR_TEX0 = GCM_VERTEX_ATTRIB_TEX0,		// 8
	NUM_ATTRS = 16
};

struct AttribDesc
{
	uint8 index;
	uint8 type;	// GCM_VERTEX_DATA_TYPE_*
	uint8 size;
	uint8 offset;
};

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;
	int32     numVertices;
	Material *material;
	bool32    vertexAlpha;
	uint32    offset;	// in the index buffer, bytes
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32      serialNumber;
	uint32      numMeshes;
	uint32      primType;	// GCM_TYPE_*
	uint32      totalNumIndex;
	uint32      totalNumVertex;
	int32       numAttribs;
	AttribDesc  attribs[8];
	uint32      stride;
	VramAlloc   vb;
	VramAlloc   ib;
	InstanceData *inst;
};

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

// ---- instance data --------------------------------------------------------------

static void
freeInstanceData(Geometry *geometry)
{
	if(geometry->instData == nil ||
	   geometry->instData->platform != PLATFORM_PS3)
		return;
	InstanceDataHeader *header = (InstanceDataHeader*)geometry->instData;
	geometry->instData = nil;
	vramFree(&header->vb);
	vramFree(&header->ib);
	rwFree(header->inst);
	rwFree(header);
}

void*
destroyNativeData(void *object, int32, int32)
{
	freeInstanceData((Geometry*)object);
	return object;
}

static InstanceDataHeader*
instanceMesh(rw::ObjPipeline *rwpipe, Geometry *geo)
{
	InstanceDataHeader *header = rwNewT(InstanceDataHeader, 1, MEMDUR_EVENT | ID_GEOMETRY);
	memset(header, 0, sizeof(*header));
	MeshHeader *meshh = geo->meshHeader;
	geo->instData = header;
	header->platform = PLATFORM_PS3;

	header->serialNumber = meshh->serialNum;
	header->numMeshes = meshh->numMeshes;
	header->primType = meshh->flags == 1 ? GCM_TYPE_TRIANGLE_STRIP : GCM_TYPE_TRIANGLES;
	header->totalNumVertex = geo->numVertices;
	header->totalNumIndex = meshh->totalIndices;
	header->inst = rwNewT(InstanceData, header->numMeshes, MEMDUR_EVENT | ID_GEOMETRY);

	if(!vramAlloc(&header->ib, header->totalNumIndex*2 + 16, "index buffer")){
		header->numMeshes = 0;
		return header;
	}

	uint16 *indices = (uint16*)rwMalloc(header->totalNumIndex*2 + 16, MEMDUR_FUNCTION | ID_GEOMETRY);
	InstanceData *inst = header->inst;
	Mesh *mesh = meshh->getMeshes();
	uint32 offset = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		findMinVertAndNumVertices(mesh->indices, mesh->numIndices,
		                          &inst->minVert, &inst->numVertices);
		inst->numIndex = mesh->numIndices;
		inst->material = mesh->material;
		inst->vertexAlpha = 0;
		inst->offset = offset;
		memcpy((uint8*)indices + offset, mesh->indices, inst->numIndex*2);
		offset += inst->numIndex*2;
		mesh++;
		inst++;
	}
	memcpy(header->ib.ptr, indices, offset);
	vramFlush(header->ib.ptr, offset);
	vertexCacheTouched();
	rwFree(indices);
	return header;
}

static void
instance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	// don't try to (re)instance native data
	if(geo->flags & Geometry::NATIVE)
		return;

	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(geo->instData){
		// Already have instanced data, so check if we have to reinstance
		if(header->platform != PLATFORM_PS3 || header->serialNumber != geo->meshHeader->serialNum){
			// Mesh changed, so reinstance everything
			freeInstanceData(geo);
		}
	}

	// no instance or complete reinstance
	if(geo->instData == nil){
		uint64 t0 = texProfNow();
		geo->instData = instanceMesh(rwpipe, geo);
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 0);
		uint64 dt = texProfNow() - t0;
		texProfile.instUs += dt;
		texProfile.instCount++;
		if(dt > texProfile.instWorstUs)
			texProfile.instWorstUs = dt;
		drawStats.instanced++;
	}else if(geo->lockedSinceInst)
		pipe->instanceCB(geo, (InstanceDataHeader*)geo->instData, 1);

	geo->lockedSinceInst = 0;
}

static void
uninstance(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	assert(0 && "can't uninstance");
}

static void
render(rw::ObjPipeline *rwpipe, Atomic *atomic)
{
	ObjPipeline *pipe = (ObjPipeline*)rwpipe;
	Geometry *geo = atomic->geometry;
	pipe->instance(atomic);
	InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
	if(header == nil || header->platform != PLATFORM_PS3 || header->vb.ptr == nil || header->ib.ptr == nil)
		return;
	if(pipe->renderCB)
		pipe->renderCB(atomic, header);
}

void
ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_PS3);
	this->impl.instance = ps3::instance;
	this->impl.uninstance = ps3::uninstance;
	this->impl.render = ps3::render;
	this->instanceCB = nil;
	this->renderCB = nil;
}

ObjPipeline*
ObjPipeline::create(void)
{
	ObjPipeline *pipe = rwNewT(ObjPipeline, 1, MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

static AttribDesc*
findAttrib(InstanceDataHeader *header, int index)
{
	for(int i = 0; i < header->numAttribs; i++)
		if(header->attribs[i].index == index)
			return &header->attribs[i];
	return nil;
}

// Vertex layout + fill. skin: weights and indices too.
static void
instanceCommon(Geometry *geo, InstanceDataHeader *header, bool32 reinstance, bool skin)
{
	bool isPrelit = !!(geo->flags & Geometry::PRELIT);
	bool hasNormals = !!(geo->flags & Geometry::NORMALS);
	AttribDesc *a;

	if(!reinstance){
		uint32 stride = 0;
		a = header->attribs;

		a->index = ATTR_POS; a->type = GCM_VERTEX_DATA_TYPE_F32; a->size = 3; a->offset = stride;
		stride += 12; a++;
		if(hasNormals){
			a->index = ATTR_NORMAL; a->type = GCM_VERTEX_DATA_TYPE_F32; a->size = 3; a->offset = stride;
			stride += 12; a++;
		}
		if(isPrelit){
			a->index = ATTR_COLOR; a->type = GCM_VERTEX_DATA_TYPE_U8; a->size = 4; a->offset = stride;
			stride += 4; a++;
		}
		if(geo->numTexCoordSets > 0){
			a->index = ATTR_TEX0; a->type = GCM_VERTEX_DATA_TYPE_F32; a->size = 2; a->offset = stride;
			stride += 8; a++;
		}
		if(skin){
			a->index = ATTR_WEIGHTS; a->type = GCM_VERTEX_DATA_TYPE_F32; a->size = 4; a->offset = stride;
			stride += 16; a++;
			a->index = ATTR_INDICES; a->type = GCM_VERTEX_DATA_TYPE_U8; a->size = 4; a->offset = stride;
			stride += 4; a++;
		}
		header->numAttribs = a - header->attribs;
		header->stride = stride;

		if(!vramAlloc(&header->vb, header->totalNumVertex*stride + 16, "vertex buffer"))
			return;
	}
	if(header->vb.ptr == nil)
		return;

	uint32 stride = header->stride;
	uint32 size = header->totalNumVertex*stride;
	uint8 *verts = (uint8*)rwMalloc(size + 16, MEMDUR_FUNCTION | ID_GEOMETRY);
	if(verts == nil)
		return;
	// a reinstance only writes what changed: start from zero, the rest
	// would be rewritten from the geometry anyway (the CPU can't read VRAM)
	memset(verts, 0, size);

	a = findAttrib(header, ATTR_POS);
	instV3d(VERT_FLOAT3, verts + a->offset, geo->morphTargets[0].vertices,
	        header->totalNumVertex, stride);

	if(hasNormals){
		a = findAttrib(header, ATTR_NORMAL);
		instV3d(VERT_FLOAT3, verts + a->offset, geo->morphTargets[0].normals,
		        header->totalNumVertex, stride);
	}

	if(isPrelit){
		a = findAttrib(header, ATTR_COLOR);
		if(skin)
			instColor(VERT_RGBA, verts + a->offset, geo->colors,
			          header->totalNumVertex, stride);
		else{
			InstanceData *inst = header->inst;
			for(uint32 n = 0; n < header->numMeshes; n++, inst++){
				if(inst->minVert == 0xFFFFFFFF)
					continue;
				inst->vertexAlpha = instColor(VERT_RGBA,
					verts + a->offset + stride*inst->minVert,
					geo->colors + inst->minVert,
					inst->numVertices, stride);
			}
		}
	}

	if(geo->numTexCoordSets > 0){
		a = findAttrib(header, ATTR_TEX0);
		instTexCoords(VERT_FLOAT2, verts + a->offset, geo->texCoords[0],
		              header->totalNumVertex, stride);
	}

	if(skin){
		Skin *sk = Skin::get(geo);
		a = findAttrib(header, ATTR_WEIGHTS);
		instV4d(VERT_FLOAT4, verts + a->offset, (V4d*)sk->weights,
		        header->totalNumVertex, stride);
		a = findAttrib(header, ATTR_INDICES);
		// not colors of course, but four bytes all the same
		instColor(VERT_RGBA, verts + a->offset, (RGBA*)sk->indices,
		          header->totalNumVertex, stride);
	}

	memcpy(header->vb.ptr, verts, size);
	vramFlush(header->vb.ptr, size);
	vertexCacheTouched();
	rwFree(verts);
	// the copy must be in VRAM before the RSX reads it
	__asm__ volatile("sync" ::: "memory");
}

static void
defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	instanceCommon(geo, header, reinstance, false);
}

static void
skinInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	instanceCommon(geo, header, reinstance, true);
}

// ---- drawing --------------------------------------------------------------------------

// Binds the geometry's attributes; the ones the programs read and the
// geometry doesn't have get constant values (GL's defaults).
static uint32 boundAttribMask;

static void
bindAttribs(InstanceDataHeader *header)
{
	gcmContextData *ctx = rsxCtx;
	uint32 mask = 0;
	if(ctx == nil)
		return;
	for(int i = 0; i < header->numAttribs; i++){
		AttribDesc *a = &header->attribs[i];
		rsxBindVertexArrayAttrib(ctx, a->index, 0, header->vb.offset + a->offset,
		                         header->stride, a->size, a->type, GCM_LOCATION_RSX);
		mask |= 1 << a->index;
	}
	// disable the arrays this geometry doesn't have
	uint32 off = boundAttribMask & ~mask;
	for(int i = 0; i < NUM_ATTRS; i++)
		if(off & (1 << i))
			rsxBindVertexArrayAttrib(ctx, i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	boundAttribMask = mask;

	static const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	if(!(mask & (1 << ATTR_NORMAL)))
		rsxDrawVertex4f(ctx, ATTR_NORMAL, zero);
	if(!(mask & (1 << ATTR_COLOR)))
		rsxDrawVertex4f(ctx, ATTR_COLOR, black);
	if(!(mask & (1 << ATTR_TEX0)))
		rsxDrawVertex4f(ctx, ATTR_TEX0, zero);
}

// for the immediate mode: its own vertices in the ring
void
bindImmediateAttribs(uint32 offset, uint32 stride, int posSize, uint32 colorOff, uint32 texOff)
{
	gcmContextData *ctx = rsxCtx;
	uint32 mask = (1 << ATTR_POS) | (1 << ATTR_COLOR) | (1 << ATTR_TEX0);
	if(ctx == nil)
		return;
	rsxBindVertexArrayAttrib(ctx, ATTR_POS, 0, offset, stride, posSize, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, ATTR_COLOR, 0, offset + colorOff, stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, ATTR_TEX0, 0, offset + texOff, stride, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	uint32 off = boundAttribMask & ~mask;
	for(int i = 0; i < NUM_ATTRS; i++)
		if(off & (1 << i))
			rsxBindVertexArrayAttrib(ctx, i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	boundAttribMask = mask;
}

void
drawIndexed(uint32 prim, uint32 ibOffset, uint32 numIndices, uint32 numVertices)
{
	rsxDrawIndexArray(rsxCtx, prim, ibOffset, numIndices, GCM_INDEX_TYPE_16B, GCM_LOCATION_RSX);
	drawStats.indices += numIndices;
	drawStats.verts += numVertices;
	countDraw();
}

// the RSX starts on the frame while the CPU still builds it: commands are
// kicked every 32 draws, not only at present
void
countDraw(void)
{
	if((++drawStats.draws & 31) == 0)
		rsxFlushBuffer(rsxCtx);
}

static void
drawInst_simple(InstanceDataHeader *header, InstanceData *inst)
{
	if(inst->numIndex == 0 || !flushCache())
		return;
	drawIndexed(header->primType, header->ib.offset + inst->offset, inst->numIndex, inst->numVertices);
}

// Emulate PS2 GS alpha test FB_ONLY case: failed alpha writes to frame- but not to depth buffer
static void
drawInst_GSemu(InstanceDataHeader *header, InstanceData *inst)
{
	uint32 hasAlpha;
	int alphafunc, alpharef, gsalpharef;
	int zwrite;
	hasAlpha = getAlphaBlend();
	if(hasAlpha){
		zwrite = rw::GetRenderState(rw::ZWRITEENABLE);
		alphafunc = rw::GetRenderState(rw::ALPHATESTFUNC);
		if(zwrite){
			alpharef = rw::GetRenderState(rw::ALPHATESTREF);
			gsalpharef = rw::GetRenderState(rw::GSALPHATESTREF);

			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAGREATEREQUAL);
			SetRenderState(rw::ALPHATESTREF, gsalpharef);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHALESS);
			SetRenderState(rw::ZWRITEENABLE, 0);
			drawInst_simple(header, inst);
			SetRenderState(rw::ZWRITEENABLE, 1);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
			SetRenderState(rw::ALPHATESTREF, alpharef);
		}else{
			SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
			drawInst_simple(header, inst);
			SetRenderState(rw::ALPHATESTFUNC, alphafunc);
		}
	}else
		drawInst_simple(header, inst);
}

static void
drawInst(InstanceDataHeader *header, InstanceData *inst)
{
	if(rw::GetRenderState(rw::GSALPHATEST))
		drawInst_GSemu(header, inst);
	else
		drawInst_simple(header, inst);
}

static int32
lightingCB(Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;

	if(atomic->geometry->flags & rw::Geometry::LIGHT){
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
		if((atomic->geometry->flags & rw::Geometry::NORMALS) == 0){
			// Get rid of lights that need normals when we don't have any
			lightData.numDirectionals = 0;
			lightData.numLocals = 0;
		}
		return setLights(&lightData);
	}else{
		memset(&lightData, 0, sizeof(lightData));
		return setLights(&lightData);
	}
}

static void
defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

	bindAttribs(header);
	setVertexProgram(VP_DEFAULT);
	setFragmentProgram(FP_SIMPLE);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		m = inst->material;
		setMaterial(flags, m->color, m->surfaceProps);
		setTexture(0, m->texture);
		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		drawInst(header, inst);
		inst++;
	}
}

// ---- skin -------------------------------------------------------------------------------

static float skinRows[MAX_SKIN_BONES*12];

static void
uploadSkinMatrices(Atomic *a)
{
	int i;
	Skin *skin = Skin::get(a->geometry);
	HAnimHierarchy *hier = Skin::getHierarchy(a);
	Matrix tmp, m;
	int32 numBones = skin->numBones > MAX_SKIN_BONES ? MAX_SKIN_BONES : skin->numBones;
	if(skin->numBones > MAX_SKIN_BONES){
		static int warned;
		if(warned++ < 5)
			rsxLog("[rsx] WARNING: skin with %d bones, only %d fit", skin->numBones, MAX_SKIN_BONES);
	}

	float *row = skinRows;
	Matrix invAtmMat;
	bool local = hier && (hier->flags & HAnimHierarchy::LOCALSPACEMATRICES);
	if(hier && !local)
		Matrix::invert(&invAtmMat, a->getFrame()->getLTM());
	Matrix *invMats = (Matrix*)skin->inverseMatrices;
	for(i = 0; i < numBones; i++){
		if(hier && i < hier->numNodes){
			invMats[i].flags = 0;
			if(local)
				Matrix::mult(&m, &invMats[i], &hier->matrices[i]);
			else{
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(&m, &invMats[i], &tmp);
			}
		}else
			m.setIdentity();
		// rows of the 3x4 (the columns are right, up, at, pos)
		row[0] = m.right.x; row[1] = m.up.x; row[2] = m.at.x; row[3] = m.pos.x;
		row[4] = m.right.y; row[5] = m.up.y; row[6] = m.at.y; row[7] = m.pos.y;
		row[8] = m.right.z; row[9] = m.up.z; row[10] = m.at.z; row[11] = m.pos.z;
		row += 12;
	}
	setBones(skinRows, numBones);
}

static void
skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	Material *m;

	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

	bindAttribs(header);
	setVertexProgram(VP_SKIN);
	setFragmentProgram(FP_SIMPLE);

	uploadSkinMatrices(atomic);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		m = inst->material;
		setMaterial(flags, m->color, m->surfaceProps);
		setTexture(0, m->texture);
		rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
		drawInst(header, inst);
		inst++;
	}
}

static void*
skinOpen(void *o, int32, int32)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = skinInstanceCB;
	pipe->renderCB = skinRenderCB;
	pipe->pluginID = ID_SKIN;
	pipe->pluginData = 1;
	skinGlobals.pipelines[PLATFORM_PS3] = pipe;
	return o;
}

static void*
skinClose(void *o, int32, int32)
{
	if(skinGlobals.pipelines[PLATFORM_PS3]){
		((ObjPipeline*)skinGlobals.pipelines[PLATFORM_PS3])->destroy();
		skinGlobals.pipelines[PLATFORM_PS3] = nil;
	}
	return o;
}

void
initSkin(void)
{
	Driver::registerPlugin(PLATFORM_PS3, 0, ID_SKIN,
	                       skinOpen, skinClose);
}

// ---- MatFX --------------------------------------------------------------------------------

static void
matfxDefaultRender(InstanceDataHeader *header, InstanceData *inst, uint32 flags)
{
	Material *m = inst->material;
	setVertexProgram(VP_DEFAULT);
	setFragmentProgram(FP_SIMPLE);
	setMaterial(flags, m->color, m->surfaceProps);
	setTexture(0, m->texture);
	rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);
	drawInst(header, inst);
}

static void
matfxEnvRender(InstanceDataHeader *header, InstanceData *inst, uint32 flags, MatFX::Env *env)
{
	Material *m = inst->material;

	if(env->tex == nil || env->coefficient == 0.0f){
		matfxDefaultRender(header, inst, flags);
		return;
	}

	setVertexProgram(VP_ENV);
	setFragmentProgram(FP_ENV);
	setTexture(0, m->texture);
	setTexture(1, env->tex);
	setEnvParams(env->frame, env->coefficient, env->fbAlpha);
	setMaterial(flags, m->color, m->surfaceProps);

	rw::SetRenderState(VERTEXALPHA, 1);
	rw::SetRenderState(SRCBLEND, BLENDONE);

	drawInst(header, inst);

	rw::SetRenderState(SRCBLEND, BLENDSRCALPHA);
	setTexture(1, nil);
}

static void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	lightingCB(atomic);

	bindAttribs(header);

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;
	while(n--){
		MatFX *matfx = MatFX::get(inst->material);

		if(matfx == nil)
			matfxDefaultRender(header, inst, flags);
		else switch(matfx->type){
		case MatFX::ENVMAP:
			matfxEnvRender(header, inst, flags, &matfx->fx[0].env);
			break;
		default:
			matfxDefaultRender(header, inst, flags);
			break;
		}
		inst++;
	}
}

static void*
matfxOpen(void *o, int32, int32)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->renderCB = matfxRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	matFXGlobals.pipelines[PLATFORM_PS3] = pipe;
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	if(matFXGlobals.pipelines[PLATFORM_PS3]){
		((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_PS3])->destroy();
		matFXGlobals.pipelines[PLATFORM_PS3] = nil;
	}
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_PS3, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

// ---- default pipeline ---------------------------------------------------------------------

void
initPipelines(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->renderCB = defaultRenderCB;
	engine->driver[PLATFORM_PS3]->defaultPipeline = pipe;
}

#endif	// RW_PS3_RSX

}
}
