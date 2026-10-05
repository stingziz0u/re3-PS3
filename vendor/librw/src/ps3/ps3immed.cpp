// ps3immed.cpp -- Im2D and Im3D of the RSX renderer (librw's gl3immed.cpp):
// the vertices (and indices) go into this frame's segment of the ring.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwps3.h"

#ifdef RW_PS3_RSX
#include "ps3rsx.h"

namespace rw {
namespace ps3 {

static_assert(sizeof(Im2DVertex) == 28, "Im2DVertex layout");
static_assert(sizeof(Im3DVertex) == 24, "Im3DVertex layout");

void
openImmediate(void)
{
}

void
closeImmediate(void)
{
}

static Im2DVertex tmpprimbuf[3];

void
im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	im2DRenderPrimitive(PRIMTYPELINELIST, tmpprimbuf, 2);
}

void
im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	Im2DVertex *verts = (Im2DVertex*)vertices;
	tmpprimbuf[0] = verts[vert1];
	tmpprimbuf[1] = verts[vert2];
	tmpprimbuf[2] = verts[vert3];
	im2DRenderPrimitive(PRIMTYPETRILIST, tmpprimbuf, 3);
}

static bool
im2DSetup(void *vertices, int32 numVertices)
{
	if(numVertices <= 0)
		return false;
	uint32 off = ringWrite(vertices, numVertices*sizeof(Im2DVertex));
	if(off == 0xFFFFFFFF)
		return false;
	bindImmediateAttribs(off, sizeof(Im2DVertex), 4,
	                     offsetof(Im2DVertex, r), offsetof(Im2DVertex, u));
	setVertexProgram(VP_IM2D);
	setFragmentProgram(FP_SIMPLE);
	im2DSetXform();
	return true;
}

void
im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	if(!im2DSetup(vertices, numVertices))
		return;
	if(!flushCache())
		return;
	debugIm2DDraw(primType, vertices, numVertices, nil, 0);
	__asm__ volatile("sync" ::: "memory");
	rsxDrawVertexArray(rsxCtx, gcmPrimType(primType), 0, numVertices);
	countDraw();
	drawStats.verts += numVertices;
}

void
im2DRenderIndexedPrimitive(PrimitiveType primType,
	void *vertices, int32 numVertices,
	void *indices, int32 numIndices)
{
	if(numIndices <= 0 || !im2DSetup(vertices, numVertices))
		return;
	uint32 ioff = ringWrite(indices, numIndices*2);
	if(ioff == 0xFFFFFFFF)
		return;
	if(!flushCache())
		return;
	debugIm2DDraw(primType, vertices, numVertices, indices, numIndices);
	__asm__ volatile("sync" ::: "memory");
	drawIndexed(gcmPrimType(primType), ioff, numIndices, numVertices);
}

// ---- Im3D --------------------------------------------------------------------------

static int32 num3DVertices;
static bool im3DValid;

void
im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	if(world == nil)
		setWorldIdentity();
	else
		setWorldMatrix(world);

	if((flags & im3d::VERTEXUV) == 0)
		SetRenderStatePtr(TEXTURERASTER, nil);

	im3DValid = false;
	num3DVertices = 0;
	if(numVertices <= 0)
		return;
	uint32 off = ringWrite(vertices, numVertices*sizeof(Im3DVertex));
	if(off == 0xFFFFFFFF)
		return;
	bindImmediateAttribs(off, sizeof(Im3DVertex), 3,
	                     offsetof(Im3DVertex, r), offsetof(Im3DVertex, u));
	num3DVertices = numVertices;
	im3DValid = true;
}

void
im3DRenderPrimitive(PrimitiveType primType)
{
	if(!im3DValid)
		return;
	setVertexProgram(VP_IM3D);
	setFragmentProgram(FP_SIMPLE);
	if(!flushCache())
		return;
	__asm__ volatile("sync" ::: "memory");
	rsxDrawVertexArray(rsxCtx, gcmPrimType(primType), 0, num3DVertices);
	countDraw();
	drawStats.verts += num3DVertices;
}

void
im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	if(!im3DValid || numIndices <= 0)
		return;
	uint32 ioff = ringWrite(indices, numIndices*2);
	if(ioff == 0xFFFFFFFF)
		return;
	setVertexProgram(VP_IM3D);
	setFragmentProgram(FP_SIMPLE);
	if(!flushCache())
		return;
	__asm__ volatile("sync" ::: "memory");
	drawIndexed(gcmPrimType(primType), ioff, numIndices, num3DVertices);
}

void
im3DEnd(void)
{
	im3DValid = false;
}

}
}

#endif
