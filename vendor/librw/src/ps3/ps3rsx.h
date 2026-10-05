// ps3rsx.h -- internal interface of the RSX renderer (stage 2, RW_PS3_RSX).
// Only the ps3/*.cpp files include this.
//
// Memory: everything the RSX reads (textures, vertex and index buffers,
// render targets, the per frame ring) is in its local memory (VRAM). The CPU
// only WRITES there (reads are very slow); main memory keeps no copy.
//
// Frames: each frame ends with a backend label (the frame fence). Memory the
// RSX may still read is freed only once the frame that last used it is done
// (vramFree is deferred), and the ring segment of frame N is reused at N+3.

#ifndef RW_PS3_RSX_H
#define RW_PS3_RSX_H

#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>

namespace rw {
namespace ps3 {

extern gcmContextData *rsxCtx;

// ---- frames ---------------------------------------------------------------

uint32 currentFrame(void);	// frame being built (from 1)
uint32 completedFrame(void);	// last frame the RSX has finished
void waitFrame(uint32 frame);	// CPU waits for the RSX to finish 'frame'
void rsxFinishAll(void);	// waits until the RSX has executed everything

// ---- VRAM -----------------------------------------------------------------

struct VramBlock;

struct VramAlloc
{
	uint8     *ptr;	// CPU address (write only!)
	uint32     offset;	// RSX offset (GCM_LOCATION_RSX)
	uint32     size;
	VramBlock *block;
};

bool  vramInit(void);
void  vramShutdown(void);
bool  vramAlloc(VramAlloc *a, uint32 size, const char *what);
void  vramFree(VramAlloc *a);	// deferred until the RSX is done with it
void  vramProcessFrees(void);
void  vramStats(uint32 *used, uint32 *total, uint32 *blocks);
bool  vramTight(void);	// also in rwps3.h, for the game

// Camera colour / depth buffers in a tile region (depth: compressed + ZCULL
// when free). false: tiling off or not possible, allocate it linear.
struct SurfaceTiles
{
	int8 tile;	// region + 1, 0 none
	bool zcomp, zcull;
};
bool  vramAllocSurface(VramAlloc *a, int32 width, int32 height, bool depth, uint32 *pitch, SurfaceTiles *t);
void  vramFreeSurface(VramAlloc *a, SurfaceTiles *t);	// waits for the RSX if tiled
bool  zcullActive(void);

// ---- per frame ring (Im2D/Im3D vertices and indices) ----------------------

// Copies 'size' bytes into this frame's ring segment (16 byte aligned).
// Returns the RSX offset, or 0xFFFFFFFF when the segment is full.
uint32 ringWrite(const void *data, uint32 size);

// ---- video (ps3video.cpp) ---------------------------------------------------

bool videoInit(void);
void videoShutdown(void);
void videoBeginFrame(void);	// new frame: ring segment, deferred frees
void videoPresent(Raster *raster, bool vsync);
int  videoWidth(void);
int  videoHeight(void);

// ---- device (ps3device.cpp) -------------------------------------------------

enum VertexProgramId
{
	VP_DEFAULT,
	VP_SKIN,
	VP_ENV,
	VP_IM2D,
	VP_IM3D,
	NUM_VP
};
enum FragmentProgramId
{
	FP_SIMPLE,
	FP_ENV,
	FP_ADD,		// rw_simple for additive blending: kills fragments that add nothing
	FP_ADDALPHA,	// same, SRCALPHA/ONE
	NUM_FP
};

bool programsInit(void);
void programsShutdown(void);

void setVertexProgram(int vp);
void setFragmentProgram(int fp);

// constants
void setWorldMatrix(Matrix *mat);
void setWorldIdentity(void);
int32 setLights(WorldLights *lightData);
void setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f);
inline void setMaterial(uint32 flags, const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp = 0.0f)
{
	static RGBA white = { 255, 255, 255, 255 };
	if(flags & Geometry::MODULATE)
		setMaterial(color, surfaceprops, extraSurfProp);
	else
		setMaterial(white, surfaceprops, extraSurfProp);
}
void setEnvParams(Frame *envFrame, float coefficient, bool fbAlpha);
void setBones(const float *rows, int32 numBones);	// 3 float4 rows per bone

void setTexture(int32 stage, Texture *tex);
void setAlphaBlend(bool32 enable);
bool32 getAlphaBlend(void);

// everything above to the RSX; false = nothing may be drawn (no target)
bool flushCache(void);

// draw statistics (for the performance line in the log)
struct DrawStats
{
	uint32 draws, verts, indices;
	uint32 vpSwitches, fpSwitches, texBinds;
	uint32 constUploads;	// float4s
	uint32 ringBytes;
	uint32 texUploadBytes;
	uint32 instanced;	// geometries instanced this period
	uint32 dropped;		// draws dropped (ring full, no target)
};
extern DrawStats drawStats;

// textures (ps3raster.cpp)
void rasterBindInfo(Raster *raster, gcmTexture *tex, bool *swizzled, int32 *numLevels);
bool rasterIsRenderable(Raster *raster);
bool rasterSurface(Raster *raster, uint32 *offset, uint32 *pitch);
void vertexCacheTouched(void);		// the CPU wrote vertex/index memory (next draw invalidates the cache)
void textureCacheTouched(void);		// the CPU wrote texture memory
bool textureCacheDirty(void);		// ...and the cache wasn't invalidated yet
void textureCacheClean(void);
Raster *whiteRaster(void);
uint32 rasterCrc(Raster *raster);
// diagnostics: logs some Im2D draws that use a watched raster
void debugIm2DDraw(uint32 prim, const void *verts, int32 numVerts, const void *indices, int32 numIndices);
void rasterInitRSX(void);
void rasterShutdownRSX(void);

// pipelines (ps3pipe.cpp)
void initPipelines(void);
void bindImmediateAttribs(uint32 offset, uint32 stride, int posSize, uint32 colorOff, uint32 texOff);
void drawIndexed(uint32 prim, uint32 ibOffset, uint32 numIndices, uint32 numVertices);
void countDraw(void);

// The PPU's stores to RSX local memory can sit in its data cache (dirty
// lines) for seconds: the RSX then reads what was there before (stale
// text, white textures that "fill in" while the game runs). Every CPU write
// into VRAM is pushed out with dcbst (a no-op if the page isn't cached).
static inline void
vramFlush(const void *p, uint32 size)
{
	uintptr a = (uintptr)p & ~(uintptr)127;
	uintptr e = (uintptr)p + size;
	for(; a < e; a += 128)
		__asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
	__asm__ volatile("sync" : : : "memory");
}

// skinning: bone matrices as 3 constant rows each from c32 (rw_skin.vcg)
#define MAX_SKIN_BONES 74

// immediate mode (ps3immed.cpp)
void openImmediate(void);
void closeImmediate(void);
void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices, int32 numVertices, void *indices, int32 numIndices);
void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);
void im2DSetXform(void);

uint32 gcmPrimType(PrimitiveType primType);

// log helpers (the game's PS3_Log)
void rsxLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}
}

#endif
