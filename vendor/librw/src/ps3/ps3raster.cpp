// PS3 native rasters.
//
// Stage 1 (headless, no RW_PS3_RSX): the texels stay in main memory, in the
// format the game locks them in (RGBA8, or DXT blocks).
//
// Stage 2 (RW_PS3_RSX): the storage is in VRAM, A8R8G8B8:
//   - power of two textures: swizzled, all levels back to back (linear
//     textures only filter right with CLAMP; GTA repeats its textures)
//   - anything else, and render targets: linear, rows padded to 64 bytes
// A lock hands out a staging buffer in main memory with the level in the
// game's format (RGBA8 bytes, or DXT blocks); the unlock converts it into
// VRAM. The CPU never reads VRAM except for the rare read locks.
// Levels the TXD doesn't have are not sampled (the descriptor only counts
// the levels uploaded from 0 on), so no black mips in the distance.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwps3.h"
#ifdef RW_PS3_RSX
#include "ps3rsx.h"
#endif
#include <sys/systime.h>
#ifdef RW_PS3_RSX
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#endif

#define PLUGIN_ID ID_DRIVER

// ps3vmx.cpp (built with -maltivec): a 2x2 mip level with AltiVec; 0 when
// the sizes or the buffers don't suit it (the scalar code does it then)
extern "C" int vmxDownsample(unsigned char *dst, int dw, int dh, const unsigned char *src, int sw, int sh, int coverage);

namespace rw {
namespace ps3 {

int32 nativeRasterOffset;
bool dxt1As16 = true;
bool dxtNative = true;
uint32 loadHints;
bool vmxMips = true;
bool forceMipmaps = false;
int32 texFilterMode = 0;
int32 texAniso = 1;
RasterStats rasterStats;
TexProfile texProfile;

// microseconds from the PPU's timebase: sysGetSystemTime is a system call
// (microseconds each), and the texture code times every level it writes
uint64
texProfNow(void)
{
	static uint64 freq;
	if(freq == 0)
		freq = sysGetTimebaseFrequency();
	uint64 tb;
	__asm__ volatile("mftb %0" : "=r"(tb));
	return tb * 1000000 / freq;
}

// Hashes of "name|mask" (lower case) looked for as image files and not
// found. Open addressing; emptied when 3/4 full (a few hundred names).
#define IMGMISS_SIZE 2048
static uint32 imgMiss[IMGMISS_SIZE];
static int32 imgMissCount;

static uint32
imgMissHash(const char *name, const char *mask)
{
	uint32 h = 2166136261u;
	for(const char *s = name; s && *s; s++)
		h = (h ^ (uint8)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s)) * 16777619u;
	h = (h ^ '|') * 16777619u;
	for(const char *s = mask; s && *s; s++)
		h = (h ^ (uint8)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s)) * 16777619u;
	return h ? h : 1;	// 0 is an empty slot
}

bool
imageMissKnown(const char *name, const char *mask)
{
	uint32 h = imgMissHash(name, mask);
	for(uint32 i = h; ; i++){
		uint32 v = imgMiss[i % IMGMISS_SIZE];
		if(v == 0)
			return false;
		if(v == h)
			return true;
	}
}

void
imageMissRemember(const char *name, const char *mask)
{
	if(imgMissCount >= IMGMISS_SIZE*3/4){
		memset(imgMiss, 0, sizeof(imgMiss));
		imgMissCount = 0;
	}
	uint32 h = imgMissHash(name, mask);
	for(uint32 i = h; ; i++){
		uint32 &v = imgMiss[i % IMGMISS_SIZE];
		if(v == h)
			return;
		if(v == 0){
			v = h;
			imgMissCount++;
			return;
		}
	}
}

static int32
dxtBlockSize(uint8 texFormat)
{
	return texFormat == PS3_TEX_DXT1 ? 8 : 16;
}

static int32
levelDim(int32 base, int32 level)
{
	int32 d = base >> level;
	return d ? d : 1;
}

// bytes of a DXT level (a block of 4x4 even when the level is smaller)
static inline uint32
dxtLevelBytes(int32 w, int32 h, int32 bs)
{
	int32 bw = (w + 3)/4, bh = (h + 3)/4;
	return (bw ? bw : 1) * (bh ? bh : 1) * bs;
}

// Size of one level as the game locks it, from the level 0 dimensions.
static uint32
levelSize(Ps3Raster *natras, int32 width, int32 height, int32 level)
{
	int32 w = levelDim(width, level), h = levelDim(height, level);
	if(natras->isCompressed)
		return ((w+3)/4) * ((h+3)/4) * dxtBlockSize(natras->texFormat);
	return w*h*natras->bpp;
}

static int32
fullMipChain(int32 w, int32 h)
{
	int32 n = 1;
	while(w != 1 || h != 1){
		n++;
		if(w > 1) w /= 2;
		if(h > 1) h /= 2;
	}
	return n;
}

static bool
isPow2(int32 v)
{
	return v > 0 && (v & (v-1)) == 0;
}

// Graphics menu MIPMAPS: a texture loaded without (all of) its mip levels
// gets the whole chain, made on the CPU from level 0 when it is written.
// Only power of two textures keep levels (see allocateTexels).
static void
forceMipChain(Raster *raster, Ps3Raster *natras)
{
	if(!forceMipmaps || (loadHints & PS3_LOAD_NOMIPS) || raster->type != Raster::TEXTURE)
		return;
	if(!isPow2(raster->width) || !isPow2(raster->height) || (raster->width == 1 && raster->height == 1))
		return;
	int32 full = fullMipChain(raster->width, raster->height);
	if(natras->numLevels >= full)
		return;
	natras->numLevels = full;
	natras->autogenMipmap = 1;
	natras->forcedMips = 1;
}

// 2x2 box filter for the MIPMAPS option: colour weighted by alpha (the
// colour of transparent texels, often black, doesn't bleed into the edges)
// and alpha halfway to the largest of the four, so cut-out textures (trees,
// fences: alpha tested) don't thin out and vanish in the distance
static void
downsampleCoverage(uint8 *dst, int32 dw, int32 dh, uint8 *src, int32 sw, int32 sh)
{
	for(int32 y = 0; y < dh; y++){
		int32 y0 = y*2 < sh ? y*2 : sh-1;
		int32 y1 = y*2+1 < sh ? y*2+1 : sh-1;
		for(int32 x = 0; x < dw; x++){
			int32 x0 = x*2 < sw ? x*2 : sw-1;
			int32 x1 = x*2+1 < sw ? x*2+1 : sw-1;
			uint8 *p[4] = { &src[(y0*sw + x0)*4], &src[(y0*sw + x1)*4],
			                &src[(y1*sw + x0)*4], &src[(y1*sw + x1)*4] };
			uint8 *d = &dst[(y*dw + x)*4];
			if((p[0][3] & p[1][3] & p[2][3] & p[3][3]) == 255){
				// opaque (most of them): the plain average, no divisions
				for(int32 k = 0; k < 3; k++)
					d[k] = (p[0][k] + p[1][k] + p[2][k] + p[3][k] + 2) >> 2;
				d[3] = 255;
				continue;
			}
			int32 asum = 0, amax = 0;
			for(int32 i = 0; i < 4; i++){
				asum += p[i][3];
				if(p[i][3] > amax) amax = p[i][3];
			}
			for(int32 k = 0; k < 3; k++){
				if(asum > 0){
					int32 c = 0;
					for(int32 i = 0; i < 4; i++)
						c += p[i][k] * p[i][3];
					d[k] = (c + asum/2) / asum;
				}else
					d[k] = (p[0][k] + p[1][k] + p[2][k] + p[3][k] + 2) >> 2;
			}
			d[3] = ((asum + 2)/4 + amax + 1) / 2;
		}
	}
}

// 2x2 box filter of an RGBA8 level into the next one
static void
downsampleRGBA8(uint8 *dst, int32 dw, int32 dh, uint8 *src, int32 sw, int32 sh)
{
	for(int32 y = 0; y < dh; y++){
		int32 y0 = y*2 < sh ? y*2 : sh-1;
		int32 y1 = y*2+1 < sh ? y*2+1 : sh-1;
		for(int32 x = 0; x < dw; x++){
			int32 x0 = x*2 < sw ? x*2 : sw-1;
			int32 x1 = x*2+1 < sw ? x*2+1 : sw-1;
			uint8 *a = &src[(y0*sw + x0)*4];
			uint8 *b = &src[(y0*sw + x1)*4];
			uint8 *c = &src[(y1*sw + x0)*4];
			uint8 *d = &src[(y1*sw + x1)*4];
			for(int32 k = 0; k < 4; k++)
				dst[(y*dw + x)*4 + k] = (a[k] + b[k] + c[k] + d[k] + 2) >> 2;
		}
	}
}

#ifndef RW_PS3_RSX
// ============================================================================
// Stage 1: main memory
// ============================================================================

bool texAsync = false;	// no texture thread without the RSX
uint32 textureBacklog(void) { return 0; }
void textureJobsSync(void) { }
void textureJobsCollect(void) { }

static bool
allocateTexels(Raster *raster, Ps3Raster *natras)
{
	uint32 total = 0;
	if(natras->numLevels > 16)
		natras->numLevels = 16;
	for(int32 i = 0; i < natras->numLevels; i++){
		natras->levelOffset[i] = total;
		total += (levelSize(natras, raster->width, raster->height, i) + 15) & ~15;
	}
	natras->texels = (uint8*)rwMalloc(total, MEMDUR_EVENT | ID_DRIVER);
	if(natras->texels == nil){
		RWERROR((ERR_ALLOC, total));
		return false;
	}
	memset(natras->texels, 0, total);
	natras->texelSize = total;
	rasterStats.numTextures++;
	rasterStats.textureBytes += total;
	return true;
}

static void
freeStorage(Ps3Raster *natras)
{
	if(natras->texels){
		rasterStats.numTextures--;
		rasterStats.textureBytes -= natras->texelSize;
		rwFree(natras->texels);
		natras->texels = nil;
	}
}

static bool
allocateSurface(Raster *raster, Ps3Raster *natras, bool texture)
{
	if(texture)
		return allocateTexels(raster, natras);
	return true;
}

static bool
hasStorage(Ps3Raster *natras)
{
	return natras->texels != nil;
}

static void
generateMipmaps(Raster *raster, Ps3Raster *natras)
{
	int32 w = raster->originalWidth;
	int32 h = raster->originalHeight;
	for(int32 i = 1; i < natras->numLevels; i++){
		int32 nw = w > 1 ? w/2 : 1;
		int32 nh = h > 1 ? h/2 : 1;
		downsampleRGBA8(natras->texels + natras->levelOffset[i], nw, nh,
		                natras->texels + natras->levelOffset[i-1], w, h);
		w = nw;
		h = nh;
	}
}

static uint8*
lockTexture(Raster *raster, Ps3Raster *natras, int32 level, int32 lockMode)
{
	if(natras->texels == nil || level < 0)
		return nil;
	if(level >= natras->numLevels){
		// a level this raster doesn't keep: written and thrown away
		static uint8 *dummy;
		static uint32 dummySize;
		uint32 size = levelSize(natras, raster->originalWidth, raster->originalHeight, level);
		if(size > dummySize){
			free(dummy);
			dummy = (uint8*)malloc(size);
			dummySize = dummy ? size : 0;
		}
		return dummy;
	}
	return natras->texels + natras->levelOffset[level];
}

static void
unlockTexture(Raster *raster, Ps3Raster *natras, int32 level)
{
	if((raster->privateFlags & Raster::LOCKWRITE) && level < natras->numLevels){
		if(level == 0 && natras->autogenMipmap && !natras->isCompressed)
			generateMipmaps(raster, natras);
		natras->serial++;
	}
}

static uint8*
lockSurface(Raster *raster, Ps3Raster *natras, int32 lockMode)
{
	// no framebuffer to read back: a black frame
	uint32 sz = raster->width*raster->height*4;
	uint8 *px = (uint8*)rwMalloc(sz, MEMDUR_EVENT | ID_DRIVER);
	if(px)
		memset(px, 0, sz);
	return px;
}

static void
unlockSurface(Raster *raster, Ps3Raster *natras)
{
	if(raster->pixels)
		rwFree(raster->pixels);
}

#else
// ============================================================================
// Stage 2: VRAM
// ============================================================================

static inline VramAlloc
getAlloc(Ps3Raster *natras)
{
	VramAlloc a;
	a.ptr = natras->vram;
	a.offset = natras->vramOffset;
	a.size = natras->vramSize;
	a.block = (VramBlock*)natras->vramBlock;
	return a;
}

static inline void
setAlloc(Ps3Raster *natras, const VramAlloc &a)
{
	natras->vram = a.ptr;
	natras->vramOffset = a.offset;
	natras->vramSize = a.size;
	natras->vramBlock = a.block;
}

static bool texCacheDirty;
static void tjCancelRaster(Ps3Raster *natras);	// the texture thread (below)

void textureCacheTouched(void) { texCacheDirty = true; }
bool textureCacheDirty(void) { return texCacheDirty; }
void textureCacheClean(void) { texCacheDirty = false; }

static void
freeStorage(Ps3Raster *natras)
{
	// levels still queued for the texture thread: dropped (the one being
	// converted finishes first: it writes into this VRAM)
	tjCancelRaster(natras);
	if(natras->vramBlock){
		VramAlloc a = getAlloc(natras);
		rasterStats.numTextures--;
		rasterStats.textureBytes -= a.size;
		if(natras->tile){
			SurfaceTiles t;
			t.tile = natras->tile;
			t.zcomp = natras->zcomp;
			t.zcull = natras->zcull;
			vramFreeSurface(&a, &t);
			natras->tile = 0;
			natras->zcomp = natras->zcull = false;
		}else
			vramFree(&a);
		natras->vram = nil;
		natras->vramBlock = nil;
		natras->vramSize = 0;
		natras->vramOffset = 0;
	}
	if(natras->staging){
		rwFree(natras->staging);
		natras->staging = nil;
	}
	natras->validLevels = 0;
}

// Texture storage: swizzled chain or one linear level
static bool
allocateTexels(Raster *raster, Ps3Raster *natras)
{
	uint32 total = 0;
	int32 w = raster->width, h = raster->height;

	int32 tb = natras->vramFmt != PS3_VRAM_ARGB8 ? 2 : 4;	// bytes per texel in VRAM

	natras->swizzled = isPow2(w) && isPow2(h);
	if(natras->vramFmt == PS3_VRAM_DXT){
		// the blocks, level after level (allocateDXT only picks this for
		// power of two sizes). The SWZ format bit: the levels repeat
		int32 bs = natras->vramDXT ? (natras->vramDXT == 1 ? 8 : 16) :
		           natras->texFormat == PS3_TEX_DXT1 ? 8 : 16;
		natras->swizzled = true;
		if(natras->numLevels > 13)
			natras->numLevels = 13;
		natras->pitch = 0;
		for(int32 i = 0; i < natras->numLevels; i++){
			natras->levelOffset[i] = total;
			total += dxtLevelBytes(levelDim(w, i), levelDim(h, i), bs);
		}
	}else if(!natras->swizzled){
		// linear: one level, rows padded to 64 bytes
		natras->numLevels = 1;
		natras->autogenMipmap = 0;
		natras->pitch = (w*tb + 63) & ~63;
		natras->levelOffset[0] = 0;
		total = natras->pitch*h;
	}else{
		if(natras->numLevels > 13)
			natras->numLevels = 13;
		natras->pitch = 0;
		for(int32 i = 0; i < natras->numLevels; i++){
			natras->levelOffset[i] = total;
			total += levelDim(w, i)*levelDim(h, i)*tb;
		}
	}
	VramAlloc a;
	if(!vramAlloc(&a, total, "texture")){
		RWERROR((ERR_ALLOC, total));
		return false;
	}
	setAlloc(natras, a);
	natras->validLevels = 0;
	rasterStats.numTextures++;
	rasterStats.textureBytes += a.size;
	return true;
}

// Render targets (and camera textures): linear A8R8G8B8 / Z24S8
static bool
allocateSurface(Raster *raster, Ps3Raster *natras, bool texture)
{
	natras->swizzled = false;
	natras->numLevels = 1;
	natras->pitch = (raster->width*4 + 63) & ~63;
	natras->levelOffset[0] = 0;
	VramAlloc a;
	if(!texture && (raster->type == Raster::CAMERA || raster->type == Raster::ZBUFFER)){
		SurfaceTiles t;
		uint32 pitch;
		if(vramAllocSurface(&a, raster->width, raster->height, raster->type == Raster::ZBUFFER, &pitch, &t)){
			natras->pitch = pitch;
			natras->tile = t.tile;
			natras->zcomp = t.zcomp;
			natras->zcull = t.zcull;
			setAlloc(natras, a);
			natras->validLevels = 1;
			rasterStats.numTextures++;
			rasterStats.textureBytes += a.size;
			return true;
		}
	}
	if(!vramAlloc(&a, natras->pitch*raster->height,
	              raster->type == Raster::ZBUFFER ? "depth buffer" : "render target")){
		RWERROR((ERR_ALLOC, natras->pitch*raster->height));
		return false;
	}
	setAlloc(natras, a);
	natras->validLevels = 1;
	rasterStats.numTextures++;
	rasterStats.textureBytes += a.size;
	return true;
}

static bool
hasStorage(Ps3Raster *natras)
{
	return natras->vram != nil;
}

// Morton order of a 2^lw x 2^lh image: x and y bits interleaved, x first;
// once one runs out the other's bits follow (RPCS3's layout, checked on
// hardware by Quake2PS3). offset(x, y) = tx[x] | ty[y].
static void
swizzleTables(uint32 *tx, uint32 *ty, int32 w, int32 h)
{
	int32 lw = 0, lh = 0;
	while((1<<lw) < w) lw++;
	while((1<<lh) < h) lh++;
	for(int32 x = 0; x < w; x++){
		uint32 off = 0, bit = 0;
		int32 v = x, bw = lw, bh = lh;
		while(bw || bh){
			if(bw){ off |= (uint32)(v & 1) << bit; v >>= 1; bit++; bw--; }
			if(bh){ bit++; bh--; }
		}
		tx[x] = off;
	}
	for(int32 y = 0; y < h; y++){
		uint32 off = 0, bit = 0;
		int32 v = y, bw = lw, bh = lh;
		while(bw || bh){
			if(bw){ bit++; bw--; }
			if(bh){ off |= (uint32)(v & 1) << bit; v >>= 1; bit++; bh--; }
		}
		ty[y] = off;
	}
}

// Conversion buffers. The game thread's grow on demand; the texture
// thread's are sized by the game thread before it queues a level (malloc
// only ever runs on the game thread)
struct ConvBuf { uint8 *p; uint32 size; };
struct ConvCtx {
	ConvBuf scratch, decoded, mipA, mipB;
	bool canGrow;		// the game thread's
	bool defer;		// levels written go in validBits, made usable later by the game thread
	uint16 validBits;
};
static ConvCtx mainCtx = { {nil, 0}, {nil, 0}, {nil, 0}, {nil, 0}, true, false, 0 };

static uint8*
convBuf(ConvCtx *cx, ConvBuf *b, uint32 need)
{
	if(need <= b->size)
		return b->p;
	if(!cx->canGrow)
		return nil;
	free(b->p);
	b->p = (uint8*)malloc(need);
	b->size = b->p ? need : 0;
	return b->p;
}

static void
convFree(ConvCtx *cx)
{
	ConvBuf *bufs[4] = { &cx->scratch, &cx->decoded, &cx->mipA, &cx->mipB };
	for(int32 i = 0; i < 4; i++){
		free(bufs[i]->p);
		bufs[i]->p = nil;
		bufs[i]->size = 0;
	}
}

static inline uint8* getScratch(ConvCtx *cx, uint32 size) { return convBuf(cx, &cx->scratch, size); }

// a level is in VRAM: usable now, or once the game thread collects the job
static void
levelDone(ConvCtx *cx, Ps3Raster *natras, int32 level)
{
	if(cx->defer)
		cx->validBits |= 1<<level;
	else{
		natras->validLevels |= 1<<level;
		textureCacheTouched();
	}
}

// Swizzled stores in 8x8 tiles: with both sides at least 8 the 64 texels
// of a tile are contiguous in the Morton order (the low 3 bits of x and y
// interleave first), so the writes go out in order instead of all over a
// buffer of up to 4 MB (every store a cache miss: most of the time of
// turning a texture into a PS3 one, and so of the hitches when new areas
// stream in). get(i) gives texel i of the row-major source.
static const uint8 mortX8[8] = { 0, 1, 4, 5, 16, 17, 20, 21 };
static const uint8 mortY8[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };

template<typename T, typename Get>
static void
swizzleStore(T *dst, int32 w, int32 h, const uint32 *tx, const uint32 *ty, Get get)
{
	if(w >= 8 && h >= 8){
		for(int32 y0 = 0; y0 < h; y0 += 8)
			for(int32 x0 = 0; x0 < w; x0 += 8){
				T *d = dst + (tx[x0] | ty[y0]);
				for(int32 dy = 0; dy < 8; dy++){
					int32 i = (y0+dy)*w + x0;
					uint32 my = mortY8[dy];
					for(int32 dx = 0; dx < 8; dx++)
						d[mortX8[dx] | my] = get(i + dx);
				}
			}
	}else{
		for(int32 y = 0; y < h; y++)
			for(int32 x = 0; x < w; x++)
				dst[tx[x] | ty[y]] = get(y*w + x);
	}
}

// DXT decoding. The same texels as librw's Image::setPixelsDXT (its rules:
// DXT3 always four colours, DXT1 and DXT5 three and transparent when
// col0 <= col1) at a fraction of the cost: tables for the 5/6 bit
// expansion, the thirds as a multiply, and no load of something stored a
// moment ago (the PPU stalls ~40 cycles on that, load-hit-store): a
// block's colours stay in registers, two 64-bit words (c0 c1 / c2 c3) a
// 2-bit index picks from without a branch.
static uint8 expand5[32], expand6[64];

static inline uint32 le16(const uint8 *p) { return p[0] | p[1]<<8; }
static inline uint32 le32(const uint8 *p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32)p[3]<<24; }
static inline uint32 third(uint32 x) { return (x * 0xAAABu) >> 17; }	// x/3, x < 65536

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PACK_RGBA(r,g,b,a) ((uint32)(a)<<24 | (uint32)(b)<<16 | (uint32)(g)<<8 | (uint32)(r))
#define RGBA_R(v) ((v) & 0xFF)
#define RGBA_G(v) (((v) >> 8) & 0xFF)
#define RGBA_B(v) (((v) >> 16) & 0xFF)
#define RGBA_A(v) ((v) >> 24)
#define RGBA_SETA(v, a) (((v) & 0x00FFFFFFu) | (uint32)(a)<<24)
#else
#define PACK_RGBA(r,g,b,a) ((uint32)(r)<<24 | (uint32)(g)<<16 | (uint32)(b)<<8 | (uint32)(a))
#define RGBA_R(v) ((v) >> 24)
#define RGBA_G(v) (((v) >> 16) & 0xFF)
#define RGBA_B(v) (((v) >> 8) & 0xFF)
#define RGBA_A(v) ((v) & 0xFF)
#define RGBA_SETA(v, a) (((v) & 0xFFFFFF00u) | (uint32)(a))
#endif

static inline void
dxtExpandInit(void)
{
	if(expand5[31] == 0){
		for(int32 i = 0; i < 32; i++) expand5[i] = i*0xFF/0x1F;
		for(int32 i = 0; i < 64; i++) expand6[i] = i*0xFF/0x3F;
	}
}

// the four colours of a block's colour part, as RGBA words
static inline void
dxtColors(int32 dxt, const uint8 *cb, uint64 *lo, uint64 *hi)
{
	uint32 col0 = le16(cb), col1 = le16(cb+2);
	uint32 r0 = expand5[(col0>>11) & 0x1F], g0 = expand6[(col0>>5) & 0x3F], b0 = expand5[col0 & 0x1F];
	uint32 r1 = expand5[(col1>>11) & 0x1F], g1 = expand6[(col1>>5) & 0x3F], b1 = expand5[col1 & 0x1F];
	uint32 c2, c3;
	if(dxt == 3 || col0 > col1){
		c2 = PACK_RGBA(third(2*r0 + r1), third(2*g0 + g1), third(2*b0 + b1), 0xFF);
		c3 = PACK_RGBA(third(r0 + 2*r1), third(g0 + 2*g1), third(b0 + 2*b1), 0xFF);
	}else{
		c2 = PACK_RGBA((r0 + r1)/2, (g0 + g1)/2, (b0 + b1)/2, 0xFF);
		c3 = 0;
	}
	*lo = (uint64)PACK_RGBA(r0, g0, b0, 0xFF) << 32 | PACK_RGBA(r1, g1, b1, 0xFF);
	*hi = (uint64)c2 << 32 | c3;
}

static inline uint32
dxtPick(uint64 lo, uint64 hi, uint32 i)
{
	uint64 m = (uint64)0 - (uint64)((i >> 1) & 1);
	uint64 w = (lo & ~m) | (hi & m);
	return (uint32)(w >> ((~i & 1) << 5));
}

// DXT3/5: the alphas of the block's 16 texels, 8 bits each (texels 0-7 in
// a0, 8-15 in a1, texel k at (k & 7)*8)
static inline void
dxtAlphas(int32 dxt, const uint8 *src, uint64 *a0, uint64 *a1)
{
	uint64 r0 = 0, r1 = 0;
	if(dxt == 3){
		for(int32 k = 0; k < 8; k++){
			r0 |= (uint64)(((src[k>>1] >> ((k & 1)*4)) & 0xF)*17) << (k*8);
			r1 |= (uint64)(((src[4 + (k>>1)] >> ((k & 1)*4)) & 0xF)*17) << (k*8);
		}
	}else{
		uint32 p0 = src[0], p1 = src[1];
		uint64 pal = (uint64)p0 | (uint64)p1 << 8;
		if(p0 > p1){
			for(uint32 i = 1; i < 7; i++)
				pal |= (uint64)(((7-i)*p0 + i*p1) / 7) << ((i+1)*8);
		}else{
			for(uint32 i = 1; i < 5; i++)
				pal |= (uint64)(((5-i)*p0 + i*p1) / 5) << ((i+1)*8);
			pal |= (uint64)0xFF << 56;
		}
		uint64 bits = (uint64)le32(src+2) | (uint64)le16(src+6) << 32;
		for(int32 k = 0; k < 8; k++, bits >>= 3)
			r0 |= ((pal >> ((bits & 7)*8)) & 0xFF) << (k*8);
		for(int32 k = 0; k < 8; k++, bits >>= 3)
			r1 |= ((pal >> ((bits & 7)*8)) & 0xFF) << (k*8);
	}
	*a0 = r0;
	*a1 = r1;
}

// DXT blocks into RGBA8 bytes, row by row (w and h multiples of 4)
static void
decodeDXT(int32 dxt, uint8 *dst, int32 w, int32 h, const uint8 *src)
{
	dxtExpandInit();
	uint32 *d32 = (uint32*)dst;
	int32 bsize = dxt == 1 ? 8 : 16;
	for(int32 by = 0; by < h; by += 4)
		for(int32 bx = 0; bx < w; bx += 4, src += bsize){
			const uint8 *cb = dxt == 1 ? src : src + 8;
			uint64 lo, hi, a0 = 0, a1 = 0;
			dxtColors(dxt, cb, &lo, &hi);
			if(dxt != 1)
				dxtAlphas(dxt, src, &a0, &a1);
			uint32 idx = le32(cb+4);
			for(int32 k = 0; k < 16; k++, idx >>= 2){
				uint32 v = dxtPick(lo, hi, idx & 3);
				if(dxt != 1)
					v = RGBA_SETA(v, ((k < 8 ? a0 : a1) >> ((k & 7)*8)) & 0xFF);
				d32[(by + (k>>2))*w + bx + (k&3)] = v;
			}
		}
}

// DXT1 level: has it a transparent texel (a 3 colour block using index 3)?
static bool
dxt1HasCutout(const uint8 *src, int32 w, int32 h)
{
	int32 n = (w/4)*(h/4);
	for(int32 i = 0; i < n; i++, src += 8)
		if(le16(src) <= le16(src+2)){
			uint32 idx = le32(src+4);
			if(idx & (idx >> 1) & 0x55555555)
				return true;
		}
	return false;
}

// DXT blocks straight into the swizzled VRAM layout (w, h >= 4: a block's
// 16 texels are contiguous in the Morton order): 16 bit (565 or 1555, DXT1)
// or A8R8G8B8. The same texels as decodeDXT + uploadLevel, in one pass.
static void
dxtToSwizzled(int32 dxt, int32 fmt, const uint8 *src, int32 w, int32 h,
              const uint32 *tx, const uint32 *ty, void *out)
{
	dxtExpandInit();
	uint16 *o16 = (uint16*)out;
	uint32 *o32 = (uint32*)out;
	int32 bsize = dxt == 1 ? 8 : 16;
	for(int32 by = 0; by < h; by += 4)
		for(int32 bx = 0; bx < w; bx += 4, src += bsize){
			const uint8 *cb = dxt == 1 ? src : src + 8;
			uint32 base = tx[bx] | ty[by];
			uint64 lo, hi;
			dxtColors(dxt, cb, &lo, &hi);
			uint32 idx = le32(cb+4);
			if(fmt != PS3_VRAM_ARGB8){
				// the four colours in 16 bits, in one register
				uint64 pal = 0;
				for(uint32 i = 0; i < 4; i++){
					uint32 v = dxtPick(lo, hi, i);
					uint32 r = RGBA_R(v), g = RGBA_G(v), b = RGBA_B(v), a = RGBA_A(v);
					uint32 p = fmt == PS3_VRAM_ARGB1555 ?
						(a >= 128 ? 0x8000 : 0) | (r>>3)<<10 | (g>>3)<<5 | b>>3 :
						(r>>3)<<11 | (g>>2)<<5 | b>>3;
					pal |= (uint64)p << (i*16);
				}
				uint16 *d = o16 + base;
				for(int32 k = 0; k < 16; k++, idx >>= 2)
					d[mortX8[k & 3] | mortY8[k >> 2]] = (uint16)(pal >> ((idx & 3)*16));
			}else{
				uint64 a0 = 0, a1 = 0;
				if(dxt != 1)
					dxtAlphas(dxt, src, &a0, &a1);
				uint32 *d = o32 + base;
				for(int32 k = 0; k < 16; k++, idx >>= 2){
					uint32 v = dxtPick(lo, hi, idx & 3);
					uint32 a = dxt != 1 ? (uint32)(((k < 8 ? a0 : a1) >> ((k & 7)*8)) & 0xFF) : RGBA_A(v);
					d[mortX8[k & 3] | mortY8[k >> 2]] = a<<24 | RGBA_R(v)<<16 | RGBA_G(v)<<8 | RGBA_B(v);
				}
			}
		}
}

static inline uint16
rgbaTo565(const uint8 *s)
{
	return (uint16)((s[0]>>3)<<11 | (s[1]>>2)<<5 | s[2]>>3);
}

static inline uint16
rgbaTo1555(const uint8 *s)
{
	return (uint16)((s[3] >= 128 ? 0x8000 : 0) | (s[0]>>3)<<10 | (s[1]>>3)<<5 | s[2]>>3);
}

static inline uint32
rgbaToARGB(const uint8 *s)
{
	return (uint32)s[3]<<24 | (uint32)s[0]<<16 | (uint32)s[1]<<8 | s[2];
}

// One RGBA8 level (bytes R G B A, w x h) into VRAM as A8R8G8B8
static void encodeDXTLevel(Raster *raster, Ps3Raster *natras, int32 level, const uint8 *rgba, ConvCtx *cx);

static void
uploadLevel(Raster *raster, Ps3Raster *natras, int32 level, uint8 *rgba, ConvCtx *cx)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	if(level >= natras->numLevels || natras->vram == nil)
		return;
	if(natras->vramFmt == PS3_VRAM_DXT){
		// an RGBA texture kept as DXT
		encodeDXTLevel(raster, natras, level, rgba, cx);
		return;
	}

	if(natras->vramFmt != PS3_VRAM_ARGB8){
		// DXT1 in 16 bits. Level 0 picks the format: A1R5G5B5 if it has a
		// transparent texel, else R5G6B5 (the levels after it follow)
		if(level == 0){
			bool cut = false;
			for(int32 i = 0; i < w*h && !cut; i++)
				cut = rgba[i*4+3] < 128;
			natras->vramFmt = cut ? PS3_VRAM_ARGB1555 : PS3_VRAM_RGB565;
		}
		bool a1 = natras->vramFmt == PS3_VRAM_ARGB1555;
		uint64 t0 = texProfNow();
		uint32 bytes;
		if(natras->swizzled){
			bytes = w*h*2;
			uint8 *buf = getScratch(cx, bytes + (w+h)*4);
			if(buf == nil)
				return;
			uint16 *pix = (uint16*)buf;
			uint32 *tx = (uint32*)(buf + ((bytes + 3) & ~3));
			uint32 *ty = tx + w;
			swizzleTables(tx, ty, w, h);
			const uint8 *s = rgba;
			if(a1)
				swizzleStore(pix, w, h, tx, ty, [s](int32 i){ return rgbaTo1555(s + i*4); });
			else
				swizzleStore(pix, w, h, tx, ty, [s](int32 i){ return rgbaTo565(s + i*4); });
			memcpy(natras->vram + natras->levelOffset[level], pix, bytes);
			vramFlush(natras->vram + natras->levelOffset[level], bytes);
		}else{
			bytes = w*h*2;
			uint16 *row = (uint16*)getScratch(cx, w*2 + 4);
			if(row == nil)
				return;
			uint8 *s = rgba;
			for(int32 y = 0; y < h; y++){
				for(int32 x = 0; x < w; x++, s += 4)
					row[x] = a1 ? (uint16)((s[3] >= 128 ? 0x8000 : 0) | (s[0]>>3)<<10 | (s[1]>>3)<<5 | s[2]>>3)
					            : (uint16)((s[0]>>3)<<11 | (s[1]>>2)<<5 | s[2]>>3);
				memcpy(natras->vram + y*natras->pitch, row, w*2);
			}
			vramFlush(natras->vram, natras->pitch*h);
		}
		texProfile.us[TEXPROF_SWIZZLE] += texProfNow() - t0;
		texProfile.levels++;
		texProfile.texels += w*h;
		drawStats.texUploadBytes += bytes;
		levelDone(cx, natras, level);
		return;
	}

	if(natras->swizzled){
		uint32 bytes = w*h*4;
		uint8 *buf = getScratch(cx, bytes + (w+h)*4);
		if(buf == nil)
			return;
		uint32 *pix = (uint32*)buf;
		uint32 *tx = (uint32*)(buf + bytes);
		uint32 *ty = tx + w;
		uint64 t0 = texProfNow();
		swizzleTables(tx, ty, w, h);
		const uint8 *s = rgba;
		swizzleStore(pix, w, h, tx, ty, [s](int32 i){ return rgbaToARGB(s + i*4); });
		uint64 t1 = texProfNow();
		memcpy(natras->vram + natras->levelOffset[level], pix, bytes);
		vramFlush(natras->vram + natras->levelOffset[level], bytes);
		uint64 t2 = texProfNow();
		texProfile.us[TEXPROF_SWIZZLE] += t1 - t0;
		texProfile.us[TEXPROF_VRAM] += t2 - t1;
		texProfile.levels++;
		texProfile.texels += w*h;
		drawStats.texUploadBytes += bytes;
	}else{
		uint32 *row = (uint32*)getScratch(cx, w*4);
		if(row == nil)
			return;
		uint8 *s = rgba;
		uint64 t0 = texProfNow();
		for(int32 y = 0; y < h; y++){
			for(int32 x = 0; x < w; x++, s += 4)
				row[x] = (uint32)s[3]<<24 | (uint32)s[0]<<16 | (uint32)s[1]<<8 | s[2];
			memcpy(natras->vram + y*natras->pitch, row, w*4);
		}
		vramFlush(natras->vram, natras->pitch*h);
		texProfile.us[TEXPROF_VRAM] += texProfNow() - t0;
		texProfile.levels++;
		texProfile.texels += w*h;
		drawStats.texUploadBytes += w*h*4;
	}
	levelDone(cx, natras, level);
}

static void encodeDXT(int32 dxt, const uint8 *rgba, int32 w, int32 h, uint8 *out);

static inline int32
rasterDXT(Ps3Raster *natras)
{
	if(natras->vramDXT)
		return natras->vramDXT;
	return natras->texFormat == PS3_TEX_DXT1 ? 1 :
	       natras->texFormat == PS3_TEX_DXT3 ? 3 : 5;
}

// A level of a native DXT texture (PS3_VRAM_DXT): its blocks into VRAM as
// they are
static void
copyDXTLevel(Raster *raster, Ps3Raster *natras, int32 level, const uint8 *blocks, ConvCtx *cx)
{
	if(level >= natras->numLevels || natras->vram == nil)
		return;
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	uint32 bytes = dxtLevelBytes(w, h, rasterDXT(natras) == 1 ? 8 : 16);
	uint64 t0 = texProfNow();
	memcpy(natras->vram + natras->levelOffset[level], blocks, bytes);
	vramFlush(natras->vram + natras->levelOffset[level], bytes);
	texProfile.us[TEXPROF_VRAM] += texProfNow() - t0;
	texProfile.levels++;
	texProfile.dxtLevels++;
	texProfile.texels += w*h;
	drawStats.texUploadBytes += bytes;
	levelDone(cx, natras, level);
}

// A mip level made on the CPU (RGBA8 bytes) for a native DXT texture:
// compressed, then copied
static void
encodeDXTLevel(Raster *raster, Ps3Raster *natras, int32 level, const uint8 *rgba, ConvCtx *cx)
{
	if(level >= natras->numLevels || natras->vram == nil)
		return;
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	int32 dxt = rasterDXT(natras);
	uint8 *out = getScratch(cx, dxtLevelBytes(w, h, dxt == 1 ? 8 : 16));
	if(out == nil)
		return;
	uint64 t0 = texProfNow();
	encodeDXT(dxt, rgba, w, h, out);
	texProfile.us[TEXPROF_ENCODE] += texProfNow() - t0;
	texProfile.encLevels++;
	copyDXTLevel(raster, natras, level, out, cx);
}

// a made mip level into VRAM, in the raster's format
static inline void
putMipLevel(Raster *raster, Ps3Raster *natras, int32 level, uint8 *rgba, ConvCtx *cx)
{
	if(natras->vramFmt == PS3_VRAM_DXT)
		encodeDXTLevel(raster, natras, level, rgba, cx);
	else
		uploadLevel(raster, natras, level, rgba, cx);
}

// Levels from+1.. made from level "from" (RGBA8 bytes) by 2x2 box
// filtering, each uploaded as it is made (two buffers in turn)
static void
uploadMipChain(Raster *raster, Ps3Raster *natras, uint8 *rgba, int32 from, ConvCtx *cx)
{
	if(from+1 >= natras->numLevels)
		return;
	uint8 *prev = rgba;
	int32 pw = levelDim(raster->originalWidth, from), ph = levelDim(raster->originalHeight, from);
	uint32 need = levelDim(raster->originalWidth, from+1)*levelDim(raster->originalHeight, from+1)*4;
	uint8 *a = convBuf(cx, &cx->mipA, need);
	uint8 *b = convBuf(cx, &cx->mipB, need);
	if(a == nil || b == nil)
		return;
	for(int32 i = from+1; i < natras->numLevels; i++){
		int32 nw = levelDim(raster->originalWidth, i);
		int32 nh = levelDim(raster->originalHeight, i);
		uint8 *next = prev == a ? b : a;
		uint64 tm = texProfNow();
		if(vmxMips && vmxDownsample(next, nw, nh, prev, pw, ph, natras->forcedMips))
			texProfile.vmxLevels++;
		else if(natras->forcedMips)
			downsampleCoverage(next, nw, nh, prev, pw, ph);
		else
			downsampleRGBA8(next, nw, nh, prev, pw, ph);
		texProfile.us[TEXPROF_MIPS] += texProfNow() - tm;
		putMipLevel(raster, natras, i, next, cx);
		prev = next;
		pw = nw;
		ph = nh;
	}
}

// A DXT level written by the game straight into its swizzled VRAM (see
// dxtToSwizzled). false: not this way (linear, tiny, or level 0 of a
// texture whose mip levels are made from it: decodeDXT + uploadLevel)
static bool
uploadDXTDirect(Raster *raster, Ps3Raster *natras, int32 level, const uint8 *blocks, ConvCtx *cx, bool mipsAfter = false)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	if(!natras->swizzled || w < 4 || h < 4 || level >= natras->numLevels || natras->vram == nil)
		return false;
	if(level == 0 && natras->autogenMipmap && natras->numLevels > 1 && !mipsAfter)
		return false;
	int32 dxt = natras->texFormat == PS3_TEX_DXT1 ? 1 :
	            natras->texFormat == PS3_TEX_DXT3 ? 3 : 5;
	if(natras->vramFmt != PS3_VRAM_ARGB8){
		if(dxt != 1)
			return false;
		// level 0 picks the format (as uploadLevel)
		if(level == 0)
			natras->vramFmt = dxt1HasCutout(blocks, w, h) ? PS3_VRAM_ARGB1555 : PS3_VRAM_RGB565;
	}
	uint64 t0 = texProfNow();
	uint32 bytes = w*h*(natras->vramFmt != PS3_VRAM_ARGB8 ? 2 : 4);
	uint8 *buf = getScratch(cx, bytes + (w+h)*4);
	if(buf == nil)
		return false;
	uint32 *tx = (uint32*)(buf + ((bytes + 3) & ~3));
	uint32 *ty = tx + w;
	swizzleTables(tx, ty, w, h);
	dxtToSwizzled(dxt, natras->vramFmt, blocks, w, h, tx, ty, buf);
	memcpy(natras->vram + natras->levelOffset[level], buf, bytes);
	vramFlush(natras->vram + natras->levelOffset[level], bytes);
	texProfile.us[TEXPROF_DXT] += texProfNow() - t0;
	texProfile.levels++;
	texProfile.texels += w*h;
	drawStats.texUploadBytes += bytes;
	levelDone(cx, natras, level);
	return true;
}

// VRAM (A8R8G8B8) back into RGBA8 bytes. Slow (the CPU reads VRAM): only
// for read locks, which the game hardly ever does.
static void
readbackLevel(Raster *raster, Ps3Raster *natras, int32 level, uint8 *rgba)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	uint8 *d = rgba;
	if(natras->vramFmt == PS3_VRAM_DXT){
		// blocks: not decoded back (the game never reads a DXT texture)
		memset(rgba, 0, w*h*4);
		return;
	}
	if(natras->vramFmt != PS3_VRAM_ARGB8){
		bool a1 = natras->vramFmt == PS3_VRAM_ARGB1555;
		uint32 *tx = natras->swizzled ? (uint32*)malloc((w+h)*4) : nil;
		if(natras->swizzled && tx == nil){
			memset(rgba, 0, w*h*4);
			return;
		}
		uint32 *ty = tx ? tx + w : nil;
		if(tx)
			swizzleTables(tx, ty, w, h);
		for(int32 y = 0; y < h; y++){
			volatile uint16 *src = natras->swizzled ? (volatile uint16*)(natras->vram + natras->levelOffset[level])
			                                        : (volatile uint16*)(natras->vram + y*natras->pitch);
			for(int32 x = 0; x < w; x++, d += 4){
				uint16 c = natras->swizzled ? src[tx[x] | ty[y]] : src[x];
				if(a1){
					d[0] = ((c>>10)&31)<<3; d[1] = ((c>>5)&31)<<3; d[2] = (c&31)<<3; d[3] = c & 0x8000 ? 255 : 0;
				}else{
					d[0] = ((c>>11)&31)<<3; d[1] = ((c>>5)&63)<<2; d[2] = (c&31)<<3; d[3] = 255;
				}
			}
		}
		free(tx);
		return;
	}
	if(natras->swizzled){
		uint32 *tx = (uint32*)malloc((w+h)*4);
		if(tx == nil){
			memset(rgba, 0, w*h*4);
			return;
		}
		uint32 *ty = tx + w;
		swizzleTables(tx, ty, w, h);
		volatile uint32 *src = (volatile uint32*)(natras->vram + natras->levelOffset[level]);
		for(int32 y = 0; y < h; y++)
			for(int32 x = 0; x < w; x++, d += 4){
				uint32 c = src[tx[x] | ty[y]];
				d[0] = c>>16; d[1] = c>>8; d[2] = c; d[3] = c>>24;
			}
		free(tx);
	}else{
		for(int32 y = 0; y < h; y++){
			volatile uint32 *src = (volatile uint32*)(natras->vram + y*natras->pitch);
			for(int32 x = 0; x < w; x++, d += 4){
				uint32 c = src[x];
				d[0] = c>>16; d[1] = c>>8; d[2] = c; d[3] = c>>24;
			}
		}
	}
}

// The MIPMAPS option on a DXT texture: its level 1 straight from the
// blocks (a 4x4 block is 2x2 texels of it), with the filter of
// downsampleCoverage (coverage) or downsampleRGBA8. Level 0 goes into VRAM
// by dxtToSwizzled, so no full size RGBA copy of the texture is made: the
// decode, the swizzle of that copy and the first mip level each went over
// all of it (most of the time of a TXD with the option on).
// the rounded average of four RGBA words, byte by byte (two bytes per
// 16 bit lane: no carries between them)
static inline uint32
avg4(uint32 a, uint32 b, uint32 c, uint32 d)
{
	uint32 lo = (a & 0x00FF00FFu) + (b & 0x00FF00FFu) + (c & 0x00FF00FFu) + (d & 0x00FF00FFu) + 0x00020002u;
	uint32 hi = ((a >> 8) & 0x00FF00FFu) + ((b >> 8) & 0x00FF00FFu) + ((c >> 8) & 0x00FF00FFu) +
	            ((d >> 8) & 0x00FF00FFu) + 0x00020002u;
	return ((lo >> 2) & 0x00FF00FFu) | (((hi >> 2) & 0x00FF00FFu) << 8);
}

static void
dxtHalf(int32 dxt, const uint8 *src, int32 w, int32 h, bool coverage, uint8 *dst)
{
	dxtExpandInit();
	int32 bsize = dxt == 1 ? 8 : 16;
	int32 hw = w/2;
	uint32 *d32 = (uint32*)dst;	// RGBA bytes, as PACK_RGBA words
	for(int32 by = 0; by < h; by += 4)
		for(int32 bx = 0; bx < w; bx += 4, src += bsize){
			const uint8 *cb = dxt == 1 ? src : src + 8;
			uint64 lo, hi, a0 = 0, a1 = 0;
			dxtColors(dxt, cb, &lo, &hi);
			if(dxt != 1)
				dxtAlphas(dxt, src, &a0, &a1);
			uint32 idx = le32(cb+4);
			uint32 *row = d32 + (by/2)*hw + bx/2;
			for(int32 qy = 0; qy < 2; qy++)
				for(int32 qx = 0; qx < 2; qx++){
					// texels k (row*4 + column) of this quarter of the block
					int32 k0 = qy*8 + qx*2;
					uint32 v0 = dxtPick(lo, hi, (idx >> (2*k0)) & 3);
					uint32 v1 = dxtPick(lo, hi, (idx >> (2*k0 + 2)) & 3);
					uint32 v2 = dxtPick(lo, hi, (idx >> (2*k0 + 8)) & 3);
					uint32 v3 = dxtPick(lo, hi, (idx >> (2*k0 + 10)) & 3);
					if(dxt != 1){
						uint64 ar = qy ? a1 : a0;	// texels 0-7 / 8-15
						int32 s = (k0 & 7)*8;
						v0 = RGBA_SETA(v0, (ar >> s) & 0xFF);
						v1 = RGBA_SETA(v1, (ar >> (s + 8)) & 0xFF);
						v2 = RGBA_SETA(v2, (ar >> (s + 32)) & 0xFF);
						v3 = RGBA_SETA(v3, (ar >> (s + 40)) & 0xFF);
					}
					uint32 *d = row + qy*hw + qx;
					if(!coverage || RGBA_A(v0 & v1 & v2 & v3) == 255){
						*d = avg4(v0, v1, v2, v3);
						continue;
					}
					// alpha weighted colour, alpha halfway to the largest
					uint32 v[4] = { v0, v1, v2, v3 };
					int32 asum = 0, amax = 0, r = 0, g = 0, b = 0;
					for(int32 n = 0; n < 4; n++){
						int32 a = RGBA_A(v[n]);
						asum += a;
						if(a > amax) amax = a;
						r += RGBA_R(v[n]) * a;
						g += RGBA_G(v[n]) * a;
						b += RGBA_B(v[n]) * a;
					}
					uint32 c;
					if(asum > 0)
						c = PACK_RGBA((r + asum/2) / asum, (g + asum/2) / asum, (b + asum/2) / asum, 0);
					else
						c = RGBA_SETA(avg4(v0, v1, v2, v3), 0);
					*d = RGBA_SETA(c, ((asum + 2)/4 + amax + 1) / 2);
				}
		}
}

// ---- native DXT (patch 41) -------------------------------------------------------
// The RSX samples DXT1/3/5 itself (patch 40's probe: the PC's block bytes,
// back to back level after level): a power of two DXT texture is kept as
// its blocks, 4x less VRAM (DXT1 vs 16 bit, DXT3/5 vs 32 bit) and
// no conversion at all, only a copy. The MIPMAPS option's levels are made
// from level 0 (level 1 straight from its blocks, dxtHalf), filtered as
// before, and compressed again here.

static inline uint32
pack565(uint32 r, uint32 g, uint32 b)
{
	return ((r*31 + 127)/255) << 11 | ((g*63 + 127)/255) << 5 | ((b*31 + 127)/255);
}

// 16 texels (PACK_RGBA words, row by row) into a colour block (8 bytes, the
// PC's byte order). All in integers: the PPU moves a value between its
// integer and float registers through memory (a load-hit-store stall each
// time), and patch 41's float version spent ~25000 cycles a block. The
// endpoints: the corners of the colours' box on the diagonal that follows
// them (the signs of the covariance with the channel that varies most),
// pulled in by 1/16 of the box (van Waveren's real-time DXT). The indices:
// each texel's position along the line between the two endpoints as the
// decoder makes them. dxt1: texels with alpha < 128 get the transparent
// index of the 3 colour mode; DXT3/5 always the 4 colour mode.
static void
encodeColorBlock(const uint32 *t, bool dxt1, uint8 *out)
{
	int32 cr[16], cg[16], cb[16], n = 0;
	uint32 opaque = 0;	// bit k: texel k has a colour (not transparent)
	for(int32 k = 0; k < 16; k++){
		uint32 v = t[k];
		if(dxt1 && RGBA_A(v) < 128)
			continue;
		opaque |= 1u << k;
		cr[n] = RGBA_R(v); cg[n] = RGBA_G(v); cb[n] = RGBA_B(v);
		n++;
	}
	if(n == 0){
		// all transparent: 3 colour mode, every texel index 3
		out[0] = out[1] = out[2] = out[3] = 0;
		out[4] = out[5] = out[6] = out[7] = 0xFF;
		return;
	}
	bool three = n < 16;
	int32 rmin = 255, gmin = 255, bmin = 255, rmax = 0, gmax = 0, bmax = 0;
	int32 sr = 0, sg = 0, sb = 0;
	for(int32 i = 0; i < n; i++){
		int32 r = cr[i], g = cg[i], b = cb[i];
		sr += r; sg += g; sb += b;
		rmin = r < rmin ? r : rmin; rmax = r > rmax ? r : rmax;
		gmin = g < gmin ? g : gmin; gmax = g > gmax ? g : gmax;
		bmin = b < bmin ? b : bmin; bmax = b > bmax ? b : bmax;
	}
	// covariances, centred values scaled by n (no division)
	int32 vrr = 0, vgg = 0, vbb = 0, vrg = 0, vrb = 0, vgb = 0;
	for(int32 i = 0; i < n; i++){
		int32 r = cr[i]*n - sr, g = cg[i]*n - sg, b = cb[i]*n - sb;
		r >>= 4; g >>= 4; b >>= 4;	// |x| < 4096: the sums fit 32 bits
		vrr += r*r; vgg += g*g; vbb += b*b;
		vrg += r*g; vrb += r*b; vgb += g*b;
	}
	// the diagonal: against the channel that varies most, a channel that
	// goes the other way has its ends swapped
	int32 hr = rmax, hg = gmax, hb = bmax, lr = rmin, lg = gmin, lb = bmin;
	int32 sgnG, sgnB, sgnR;
	if(vrr >= vgg && vrr >= vbb){ sgnR = 1; sgnG = vrg; sgnB = vrb; }
	else if(vgg >= vbb){ sgnG = 1; sgnR = vrg; sgnB = vgb; }
	else{ sgnB = 1; sgnR = vrb; sgnG = vgb; }
	if(sgnR < 0){ hr = rmin; lr = rmax; }
	if(sgnG < 0){ hg = gmin; lg = gmax; }
	if(sgnB < 0){ hb = bmin; lb = bmax; }
	// pulled in by 1/16 of the box
	int32 ir = (rmax - rmin) >> 4, ig = (gmax - gmin) >> 4, ib = (bmax - bmin) >> 4;
	hr += hr > lr ? -ir : ir; lr += hr > lr ? ir : -ir;
	hg += hg > lg ? -ig : ig; lg += hg > lg ? ig : -ig;
	hb += hb > lb ? -ib : ib; lb += hb > lb ? ib : -ib;
	uint32 c0 = pack565(hr, hg, hb);
	uint32 c1 = pack565(lr, lg, lb);
	if(three){
		if(c0 > c1){ uint32 x = c0; c0 = c1; c1 = x; }	// c0 <= c1: 3 colour mode
	}else{
		if(c0 < c1){ uint32 x = c0; c0 = c1; c1 = x; }	// c0 > c1: 4 colour mode
	}
	if(c0 == c1){
		// one colour: index 0 everywhere (the same in both modes), 3 if transparent
		uint32 idx = 0;
		for(int32 k = 0; k < 16; k++)
			if(!(opaque & (1u << k)))
				idx |= 3u << (2*k);
		out[0] = c0 & 0xFF; out[1] = c0 >> 8; out[2] = c1 & 0xFF; out[3] = c1 >> 8;
		out[4] = idx & 0xFF; out[5] = (idx >> 8) & 0xFF; out[6] = (idx >> 16) & 0xFF; out[7] = idx >> 24;
		return;
	}
	// the endpoints as the decoder expands them, and the line between them
	int32 r0 = expand5[(c0>>11) & 0x1F], g0 = expand6[(c0>>5) & 0x3F], b0 = expand5[c0 & 0x1F];
	int32 r1 = expand5[(c1>>11) & 0x1F], g1 = expand6[(c1>>5) & 0x3F], b1 = expand5[c1 & 0x1F];
	int32 dr = r1 - r0, dg = g1 - g0, db = b1 - b0;
	int32 len = dr*dr + dg*dg + db*db;	// > 0: c0 != c1
	// the position f = dot/len (0 at c0, 1 at c1) picks the index:
	// 4 colours: c0 | 2/3 c0 + 1/3 c1 | 1/3 c0 + 2/3 c1 | c1 = 0 2 3 1,
	// borders at 1/6, 3/6, 5/6; 3 colours: c0 | half | c1 = 0 2 1, at 1/4, 3/4
	uint32 idx = 0;
	int32 m = 0;
	for(int32 k = 0; k < 16; k++){
		uint32 best;
		if(!(opaque & (1u << k)))
			best = 3;
		else{
			int32 dot = (cr[m] - r0)*dr + (cg[m] - g0)*dg + (cb[m] - b0)*db;
			m++;
			if(three){
				int32 f = dot*4;	// in quarters of len
				best = f < len ? 0 : f < 3*len ? 2 : 1;
			}else{
				int32 f = dot*6;	// in sixths of len
				best = f < len ? 0 : f < 3*len ? 2 : f < 5*len ? 3 : 1;
			}
		}
		idx |= best << (2*k);
	}
	out[0] = c0 & 0xFF; out[1] = c0 >> 8; out[2] = c1 & 0xFF; out[3] = c1 >> 8;
	out[4] = idx & 0xFF; out[5] = (idx >> 8) & 0xFF; out[6] = (idx >> 16) & 0xFF; out[7] = idx >> 24;
}

// DXT5 alpha block: the 8 alpha mode between the largest and smallest
// (index 0 the largest, 1 the smallest, 2-7 the steps between, as a
// position along the range: one division a block)
static void
encodeAlpha5(const uint32 *t, uint8 *out)
{
	int32 amin = 255, amax = 0;
	for(int32 k = 0; k < 16; k++){
		int32 a = RGBA_A(t[k]);
		amin = a < amin ? a : amin;
		amax = a > amax ? a : amax;
	}
	out[0] = amax;
	out[1] = amin;
	uint64 bits = 0;
	int32 range = amax - amin;
	if(range > 0){
		int32 inv = (7 << 16) / range;
		for(int32 k = 0; k < 16; k++){
			int32 s = ((amax - RGBA_A(t[k]))*inv + 0x8000) >> 16;	// 0 (amax) .. 7 (amin)
			uint32 i = s == 0 ? 0 : s == 7 ? 1 : s + 1;
			bits |= (uint64)i << (3*k);
		}
	}
	for(int32 i = 0; i < 6; i++)
		out[2+i] = (bits >> (8*i)) & 0xFF;
}

// DXT3 alpha block: 4 bits a texel
static void
encodeAlpha3(const uint32 *t, uint8 *out)
{
	uint32 lo = 0, hi = 0;
	for(int32 k = 0; k < 8; k++){
		lo |= ((RGBA_A(t[k])*15 + 127) / 255) << (4*k);
		hi |= ((RGBA_A(t[k+8])*15 + 127) / 255) << (4*k);
	}
	for(int32 i = 0; i < 4; i++){
		out[i] = lo >> (8*i);
		out[4+i] = hi >> (8*i);
	}
}

// an RGBA8 level (w x h, any size) into DXT blocks
static void
encodeDXT(int32 dxt, const uint8 *rgba, int32 w, int32 h, uint8 *out)
{
	dxtExpandInit();
	const uint32 *px = (const uint32*)rgba;
	int32 bw = (w + 3)/4, bh = (h + 3)/4;
	uint32 t[16];
	for(int32 by = 0; by < bh; by++)
		for(int32 bx = 0; bx < bw; bx++){
			if(w >= 4 && h >= 4){
				const uint32 *s = px + by*4*w + bx*4;
				for(int32 y = 0; y < 4; y++, s += w){
					t[y*4] = s[0]; t[y*4+1] = s[1]; t[y*4+2] = s[2]; t[y*4+3] = s[3];
				}
			}else
				// smaller than a block: the edge texels repeated
				for(int32 k = 0; k < 16; k++){
					int32 x = bx*4 + (k & 3), y = by*4 + (k >> 2);
					if(x >= w) x = w - 1;
					if(y >= h) y = h - 1;
					t[k] = px[y*w + x];
				}
			if(dxt == 1){
				encodeColorBlock(t, true, out);
				out += 8;
			}else{
				if(dxt == 3)
					encodeAlpha3(t, out);
				else
					encodeAlpha5(t, out);
				encodeColorBlock(t, false, out + 8);
				out += 16;
			}
		}
}

// One level the game wrote (RGBA8 bytes or DXT blocks) into VRAM, and the
// mip levels made from level 0 when the raster wants them
static void
convertLevel(Raster *raster, Ps3Raster *natras, int32 level, uint8 *src, ConvCtx *cx)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	bool mips = level == 0 && natras->autogenMipmap && natras->numLevels > 1;
	uint8 *rgba = src;
	if(natras->vramFmt == PS3_VRAM_DXT && natras->vramDXT){
		// an RGBA texture kept as DXT (VRAM was full): compressed here,
		// the mip levels too
		encodeDXTLevel(raster, natras, level, src, cx);
		if(mips && natras->vram)
			uploadMipChain(raster, natras, src, 0, cx);
		return;
	}
	if(natras->vramFmt == PS3_VRAM_DXT){
		// native DXT: the game's blocks as they are; the MIPMAPS option's
		// levels made from them and compressed again
		copyDXTLevel(raster, natras, level, src, cx);
		if(mips && natras->vram){
			uint8 *half = convBuf(cx, &cx->decoded, (w/2)*(h/2)*4);
			if(half == nil)
				return;
			uint64 tm = texProfNow();
			dxtHalf(rasterDXT(natras), src, w, h, natras->forcedMips, half);
			texProfile.us[TEXPROF_MIPS] += texProfNow() - tm;
			encodeDXTLevel(raster, natras, 1, half, cx);
			uploadMipChain(raster, natras, half, 1, cx);
		}
		return;
	}
	if(natras->isCompressed){
		int32 dxt = natras->texFormat == PS3_TEX_DXT1 ? 1 :
		            natras->texFormat == PS3_TEX_DXT3 ? 3 : 5;
		if(mips){
			// level 0 and level 1 straight from the blocks
			if(uploadDXTDirect(raster, natras, 0, src, cx, true)){
				uint8 *half = convBuf(cx, &cx->decoded, (w/2)*(h/2)*4);
				if(half){
					uint64 tm = texProfNow();
					dxtHalf(dxt, src, w, h, natras->forcedMips, half);
					texProfile.us[TEXPROF_MIPS] += texProfNow() - tm;
					uploadLevel(raster, natras, 1, half, cx);
					uploadMipChain(raster, natras, half, 1, cx);
				}
				return;
			}
		}else if(uploadDXTDirect(raster, natras, level, src, cx))
			return;
		// DXT decompressed here (linear and tiny levels: the RSX could take
		// the blocks, but linear compressed textures don't repeat)
		int32 dw = (w + 3) & ~3;
		int32 dh = (h + 3) & ~3;
		uint8 *decoded = convBuf(cx, &cx->decoded, dw*dh*4);
		if(decoded == nil)
			return;
		uint64 td = texProfNow();
		decodeDXT(dxt, decoded, dw, dh, src);
		texProfile.us[TEXPROF_DXT] += texProfNow() - td;
		if(dw != w || dh != h){
			// tiny level: the top left corner
			for(int32 y = 0; y < h; y++)
				memmove(decoded + y*w*4, decoded + y*dw*4, w*4);
		}
		rgba = decoded;
	}
	uploadLevel(raster, natras, level, rgba, cx);
	if(mips)
		uploadMipChain(raster, natras, rgba, 0, cx);
}

// ---- the texture thread ---------------------------------------------------------
// The PPU runs two threads at once and the game used one. The levels the
// game writes into its textures (streaming: DXT decode, mip levels, swizzle,
// the copy into VRAM) are converted by a second thread while the game
// thread goes on with its frame (logic, audio, the render list, the
// draws). A texture still queued when a draw binds it is finished first,
// the game thread converting its levels itself if the thread hasn't got
// to them: nothing is ever drawn white or half written. (Patch 36 waited
// for all of them at the start of the frame's drawing: the game thread
// did a third of them itself, ~1 ms a frame in the flights.) A raster deleted with levels queued
// waits for the one being converted and drops the others. Only new
// textures go to the thread (nothing of them drawn yet); the rest, and
// everything with USRDIR/notexthread.txt, as before on the game thread.

bool texAsync = true;

struct TexJob {
	Raster *raster;
	Ps3Raster *natras;
	int32 level;
	uint8 *src;		// the locked level (freed by the game thread)
	uint32 texels;
	volatile int32 state;
	uint16 validBits;
	TexJob *next;
};
enum { TJ_QUEUED, TJ_RUNNING, TJ_DONE, TJ_CANCELLED };

static TexJob *tjHead;
static sys_mutex_t tjMutex;
static sys_cond_t tjWork, tjDone;
static sys_ppu_thread_t tjThread;
static volatile bool tjQuit;
static bool tjOn;
static volatile uint32 tjBacklog;	// texels queued or being converted
static ConvCtx workerCtx = { {nil, 0}, {nil, 0}, {nil, 0}, {nil, 0}, false, true, 0 };
static ConvCtx helpCtx = { {nil, 0}, {nil, 0}, {nil, 0}, {nil, 0}, true, true, 0 };

uint32 textureBacklog(void) { return tjBacklog; }

// the first queued level with no unfinished job of its raster before it
// (level 0 picks the 16 bit format the others follow). Mutex held.
static TexJob*
tjPick(void)
{
	for(TexJob *j = tjHead; j; j = j->next){
		if(j->state != TJ_QUEUED)
			continue;
		bool blocked = false;
		for(TexJob *e = tjHead; e != j; e = e->next)
			if(e->natras == j->natras && (e->state == TJ_QUEUED || e->state == TJ_RUNNING)){
				blocked = true;
				break;
			}
		if(!blocked)
			return j;
	}
	return nil;
}

static bool
tjBusy(void)
{
	for(TexJob *j = tjHead; j; j = j->next)
		if(j->state == TJ_QUEUED || j->state == TJ_RUNNING)
			return true;
	return false;
}

// j is RUNNING (taken with the mutex held); called without it
static void
tjRun(TexJob *j, ConvCtx *cx)
{
	cx->validBits = 0;
	convertLevel(j->raster, j->natras, j->level, j->src, cx);
	sysMutexLock(tjMutex, 0);
	j->validBits = cx->validBits;
	j->state = TJ_DONE;
	tjBacklog -= j->texels;
	sysCondBroadcast(tjDone);
	sysMutexUnlock(tjMutex);
}

// finished and dropped jobs: their levels usable, their buffers freed.
// Game thread, mutex held.
static void
tjCollectLocked(void)
{
	TexJob **pp = &tjHead;
	while(*pp){
		TexJob *j = *pp;
		if(j->state == TJ_DONE || j->state == TJ_CANCELLED){
			if(j->state == TJ_DONE && j->validBits){
				j->natras->validLevels |= j->validBits;
				textureCacheTouched();
			}
			j->natras->pendingJobs--;
			*pp = j->next;
			rwFree(j->src);
			free(j);
		}else
			pp = &j->next;
	}
}

static void
tjThreadMain(void *arg)
{
	(void)arg;
	sysMutexLock(tjMutex, 0);
	while(!tjQuit){
		TexJob *j = tjPick();
		if(j == nil){
			sysCondWait(tjWork, 0);
			continue;
		}
		j->state = TJ_RUNNING;
		sysMutexUnlock(tjMutex);
		tjRun(j, &workerCtx);
		texProfile.asyncLevels++;
		sysMutexLock(tjMutex, 0);
	}
	sysMutexUnlock(tjMutex);
	sysThreadExit(0);
}

void
textureJobsSync(void)
{
	if(!tjOn)
		return;
	sysMutexLock(tjMutex, 0);
	if(tjHead){
		uint64 t0 = texProfNow();
		for(;;){
			TexJob *j = tjPick();
			if(j){
				// the game thread takes it on instead of waiting
				j->state = TJ_RUNNING;
				sysMutexUnlock(tjMutex);
				tjRun(j, &helpCtx);
				texProfile.helpedLevels++;
				sysMutexLock(tjMutex, 0);
				continue;
			}
			if(!tjBusy())
				break;
			sysCondWait(tjDone, 0);
		}
		tjCollectLocked();
		texProfile.syncWaitUs += texProfNow() - t0;
	}
	sysMutexUnlock(tjMutex);
}

// the levels of one raster, before a draw binds it (rasterBindInfo)
static void
tjWaitRaster(Ps3Raster *natras)
{
	if(!tjOn || natras->pendingJobs == 0)
		return;
	uint64 t0 = texProfNow();
	sysMutexLock(tjMutex, 0);
	for(;;){
		// its first unfinished level (the levels of a raster go in order)
		TexJob *first = nil;
		for(TexJob *j = tjHead; j; j = j->next)
			if(j->natras == natras && (j->state == TJ_QUEUED || j->state == TJ_RUNNING)){
				first = j;
				break;
			}
		if(first == nil)
			break;
		if(first->state == TJ_QUEUED){
			first->state = TJ_RUNNING;
			sysMutexUnlock(tjMutex);
			tjRun(first, &helpCtx);
			texProfile.helpedLevels++;
			sysMutexLock(tjMutex, 0);
		}else
			sysCondWait(tjDone, 0);
	}
	tjCollectLocked();
	sysMutexUnlock(tjMutex);
	texProfile.syncWaitUs += texProfNow() - t0;
}

// the finished levels usable, without waiting for the others (camera begin)
void
textureJobsCollect(void)
{
	if(!tjOn)
		return;
	sysMutexLock(tjMutex, 0);
	tjCollectLocked();
	sysMutexUnlock(tjMutex);
}

static void
tjCancelRaster(Ps3Raster *natras)
{
	if(!tjOn || natras->pendingJobs == 0)
		return;
	sysMutexLock(tjMutex, 0);
	for(;;){
		bool running = false;
		for(TexJob *j = tjHead; j; j = j->next)
			if(j->natras == natras){
				if(j->state == TJ_QUEUED){
					j->state = TJ_CANCELLED;
					tjBacklog -= j->texels;
				}else if(j->state == TJ_RUNNING)
					running = true;
			}
		if(!running)
			break;
		sysCondWait(tjDone, 0);
	}
	tjCollectLocked();
	sysMutexUnlock(tjMutex);
}

// what convertLevel needs for a level (scratch, decoded, each mip buffer)
static void
convNeeds(Raster *raster, Ps3Raster *natras, int32 level, uint32 *scr, uint32 *dec, uint32 *mip)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	*scr = w*h*4 + (w+h)*4 + 64;
	*dec = natras->isCompressed ? ((w+3) & ~3)*((h+3) & ~3)*4 : 0;
	*mip = level == 0 && natras->autogenMipmap && natras->numLevels > 1 ?
		levelDim(w, 1)*levelDim(h, 1)*4 : 0;
}

static bool
tjGrow(ConvBuf *b, uint32 need)
{
	if(need <= b->size)
		return true;
	need += need/4;
	free(b->p);
	b->p = (uint8*)malloc(need);
	b->size = b->p ? need : 0;
	return b->p != nil;
}

// true: the level is the texture thread's (and so is src)
static bool
texJobQueue(Raster *raster, Ps3Raster *natras, int32 level, uint8 *src)
{
	if(!tjOn || raster->type != Raster::TEXTURE || natras->vram == nil || natras->validLevels != 0)
		return false;
	// native DXT with nothing to make: only a copy into VRAM, quicker than
	// queuing it (in order: none of its levels queued before it)
	if(natras->vramFmt == PS3_VRAM_DXT && !natras->vramDXT && natras->pendingJobs == 0 &&
	   !(level == 0 && natras->autogenMipmap && natras->numLevels > 1))
		return false;
	// a loading screen queues a whole island: the buffers waiting would
	// take too much memory, the game thread catches up first
	if(tjBacklog > 2000000)
		textureJobsSync();
	uint32 scr, dec, mip;
	convNeeds(raster, natras, level, &scr, &dec, &mip);
	if(scr > workerCtx.scratch.size || dec > workerCtx.decoded.size || mip > workerCtx.mipA.size){
		// a bigger level than any before: the thread's buffers grow, with
		// it idle
		textureJobsSync();
		if(!tjGrow(&workerCtx.scratch, scr) || !tjGrow(&workerCtx.decoded, dec) ||
		   !tjGrow(&workerCtx.mipA, mip) || !tjGrow(&workerCtx.mipB, mip))
			return false;
	}
	TexJob *j = (TexJob*)malloc(sizeof(TexJob));
	if(j == nil)
		return false;
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	j->raster = raster;
	j->natras = natras;
	j->level = level;
	j->src = src;
	// the work, in texels converted (a native DXT level is only copied)
	j->texels = (natras->vramFmt == PS3_VRAM_DXT && !natras->vramDXT ? w*h/16 : w*h) + (mip ? w*h/3 : 0);
	j->state = TJ_QUEUED;
	j->validBits = 0;
	j->next = nil;
	natras->pendingJobs++;
	sysMutexLock(tjMutex, 0);
	tjCollectLocked();
	TexJob **pp = &tjHead;
	while(*pp)
		pp = &(*pp)->next;
	*pp = j;
	tjBacklog += j->texels;
	sysCondSignal(tjWork);
	sysMutexUnlock(tjMutex);
	return true;
}

static void
tjStart(void)
{
	if(!texAsync || tjOn)
		return;
	sys_mutex_attr_t ma;
	memset(&ma, 0, sizeof(ma));
	ma.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	ma.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	ma.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	ma.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strncpy(ma.name, "rwtex", sizeof(ma.name)-1);
	if(sysMutexCreate(&tjMutex, &ma) != 0){
		rsxLog("[rsx] texture thread: no mutex, textures on the game thread");
		return;
	}
	sys_cond_attr_t ca;
	sysCondAttrInitialize(ca);
	if(sysCondCreate(&tjWork, tjMutex, &ca) != 0 || sysCondCreate(&tjDone, tjMutex, &ca) != 0){
		rsxLog("[rsx] texture thread: no condition variables, textures on the game thread");
		return;
	}
	// below the game thread (1000): it never takes the game thread's turn
	tjQuit = false;
	if(sysThreadCreate(&tjThread, tjThreadMain, NULL, 1100, 64*1024, THREAD_JOINABLE, (char*)"rwtexconv") != 0){
		rsxLog("[rsx] texture thread: not created, textures on the game thread");
		return;
	}
	// buffers for a 512x512 level with its mip levels; bigger ones grow them
	tjGrow(&workerCtx.scratch, 512*512*4 + 4096 + 64);
	tjGrow(&workerCtx.decoded, 512*512*4);
	tjGrow(&workerCtx.mipA, 256*256*4);
	tjGrow(&workerCtx.mipB, 256*256*4);
	tjOn = true;
	rsxLog("[rsx] texture thread on (USRDIR/notexthread.txt turns it off)");
}

static void
tjStop(void)
{
	if(!tjOn)
		return;
	textureJobsSync();
	sysMutexLock(tjMutex, 0);
	tjQuit = true;
	sysCondSignal(tjWork);
	sysMutexUnlock(tjMutex);
	u64 rv;
	sysThreadJoin(tjThread, &rv);
	sysCondDestroy(tjWork);
	sysCondDestroy(tjDone);
	sysMutexDestroy(tjMutex);
	convFree(&workerCtx);
	convFree(&helpCtx);
	tjOn = false;
}

static uint8*
lockTexture(Raster *raster, Ps3Raster *natras, int32 level, int32 lockMode)
{
	// no VRAM (it was full): the lock still hands out a buffer, the unlock
	// throws it away; a nil lock is a crash in librw's converters
	if(level < 0 || level >= 16)
		return nil;
	if(natras->pendingJobs && (lockMode & Raster::LOCKREAD))
		textureJobsSync();	// reading a level still being converted (the game hardly ever reads)
	if(natras->staging){
		rwFree(natras->staging);
		natras->staging = nil;
	}
	uint32 size = levelSize(natras, raster->originalWidth, raster->originalHeight, level);
	natras->staging = (uint8*)rwMalloc(size, MEMDUR_EVENT | ID_DRIVER);
	if(natras->staging == nil)
		return nil;
	if(level >= natras->numLevels){
		// a mip level this raster doesn't keep (non power of two textures
		// are one linear level): a buffer to write into, thrown away at
		// unlock. Failing the lock made Raster::convertTexToCurrentPlatform
		// write the small level over level 0 (rasterFromImage relocks 0).
		memset(natras->staging, 0, size);
		natras->lockedLevel = level;
		return natras->staging;
	}
	if(!(lockMode & Raster::LOCKNOFETCH) && (natras->validLevels & (1<<level)) && !natras->isCompressed){
		uint64 t0 = texProfNow();
		readbackLevel(raster, natras, level, natras->staging);
		texProfile.us[TEXPROF_READBACK] += texProfNow() - t0;
		texProfile.readbacks++;
	}
	else if(lockMode & Raster::LOCKREAD && natras->isCompressed){
		// the DXT blocks aren't kept
		rwFree(natras->staging);
		natras->staging = nil;
		return nil;
	}else
		memset(natras->staging, 0, size);
	natras->lockedLevel = level;
	return natras->staging;
}

static void
unlockTexture(Raster *raster, Ps3Raster *natras, int32 level)
{
	if(natras->staging == nil)
		return;
	if((raster->privateFlags & Raster::LOCKWRITE) && level < natras->numLevels){
		natras->serial++;
		if(texJobQueue(raster, natras, level, natras->staging)){
			// the texture thread converts it and frees the buffer
			natras->staging = nil;
			natras->lockedLevel = -1;
			return;
		}
		convertLevel(raster, natras, level, natras->staging, &mainCtx);
	}
	rwFree(natras->staging);
	natras->staging = nil;
	natras->lockedLevel = -1;
}

static uint8*
lockSurface(Raster *raster, Ps3Raster *natras, int32 lockMode)
{
	// render target read back (screenshots): slow, rare
	uint32 sz = raster->width*raster->height*4;
	uint8 *px = (uint8*)rwMalloc(sz, MEMDUR_EVENT | ID_DRIVER);
	if(px == nil)
		return nil;
	if(natras->vram && raster->type != Raster::ZBUFFER){
		rsxFinishAll();
		readbackLevel(raster, natras, 0, px);
	}else
		memset(px, 0, sz);
	return px;
}

static void
unlockSurface(Raster *raster, Ps3Raster *natras)
{
	if(raster->pixels)
		rwFree(raster->pixels);
}

// ---- for the device ----------------------------------------------------------

// Texture descriptor of a raster for a sampler. numLevels 0: not usable
// (nothing uploaded yet).
void
rasterBindInfo(Raster *raster, gcmTexture *tex, bool *swizzled, int32 *numLevels)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster->parent);
	if(natras->probeFmt){
		// hwProbe's textures: as the probe describes them
		memset(tex, 0, sizeof(*tex));
		tex->format = natras->probeFmt;
		tex->mipmap = natras->probeLevels;
		tex->dimension = GCM_TEXTURE_DIMS_2D;
		tex->cubemap = GCM_FALSE;
		tex->remap = 0x00AAE4;
		tex->width = raster->parent->originalWidth;
		tex->height = raster->parent->originalHeight;
		tex->depth = 1;
		tex->location = natras->probeLoc;
		tex->pitch = natras->probePitch;
		tex->offset = natras->probeOffset;
		*numLevels = natras->probeLevels;
		*swizzled = true;	// REPEAT whatever the layout: that is what the probe tries
		return;
	}
	if(natras->pendingJobs)
		tjWaitRaster(natras);	// levels the texture thread hasn't finished
	int32 n = 0;
	while(n < natras->numLevels && (natras->validLevels & (1<<n)))
		n++;
	*numLevels = n;
	*swizzled = natras->swizzled;
	if(n == 0 || natras->vram == nil){
		*numLevels = 0;
		return;
	}
	memset(tex, 0, sizeof(*tex));
	tex->format = (natras->vramFmt == PS3_VRAM_RGB565 ? GCM_TEXTURE_FORMAT_R5G6B5 :
	               natras->vramFmt == PS3_VRAM_ARGB1555 ? GCM_TEXTURE_FORMAT_A1R5G5B5 :
	               natras->vramFmt == PS3_VRAM_DXT ?
	                 (rasterDXT(natras) == 1 ? GCM_TEXTURE_FORMAT_DXT1 :
	                  rasterDXT(natras) == 3 ? GCM_TEXTURE_FORMAT_DXT23 : GCM_TEXTURE_FORMAT_DXT45) :
	               GCM_TEXTURE_FORMAT_A8R8G8B8) |
		(natras->swizzled ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
	tex->mipmap = n;
	tex->dimension = GCM_TEXTURE_DIMS_2D;
	tex->cubemap = GCM_FALSE;
	tex->remap = 0x00AAE4;	// identity (ps3gl)
	if(natras->vramFmt == PS3_VRAM_RGB565)
		tex->remap = 0x00A9E4;	// alpha: ONE (type A, bits 8-9 = 1), colours as they are
	tex->width = raster->parent->originalWidth;
	tex->height = raster->parent->originalHeight;
	tex->depth = 1;
	tex->location = GCM_LOCATION_RSX;
	tex->pitch = natras->swizzled ? 0 : natras->pitch;
	tex->offset = natras->vramOffset;
}

bool
rasterIsRenderable(Raster *raster)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster->parent);
	return natras->vram != nil && !natras->swizzled &&
		(raster->parent->type == Raster::CAMERA || raster->parent->type == Raster::CAMERATEXTURE ||
		 raster->parent->type == Raster::ZBUFFER);
}

bool
rasterSurface(Raster *raster, uint32 *offset, uint32 *pitch)
{
	if(raster == nil || !rasterIsRenderable(raster))
		return false;
	Ps3Raster *natras = GETPS3RASTEREXT(raster->parent);
	*offset = natras->vramOffset;
	*pitch = natras->pitch;
	return true;
}

// Diagnostics: a sparse checksum of a raster's level 0 in VRAM (every 61st
// word: the CPU reads VRAM slowly)
uint32
rasterCrc(Raster *raster)
{
	if(raster == nil)
		return 0;
	Ps3Raster *natras = GETPS3RASTEREXT(raster->parent);
	if(natras->vram == nil)
		return 0;
	uint32 words = natras->swizzled ? raster->parent->originalWidth*raster->parent->originalHeight
	                                : natras->pitch*raster->parent->originalHeight/4;
	if(natras->vramFmt == PS3_VRAM_DXT)
		words = dxtLevelBytes(raster->parent->originalWidth, raster->parent->originalHeight,
		                      rasterDXT(natras) == 1 ? 8 : 16)/4;
	else if(natras->vramFmt != PS3_VRAM_ARGB8 && natras->swizzled)
		words /= 2;
	volatile uint32 *p = (volatile uint32*)natras->vram;
	uint32 h = 2166136261u;
	for(uint32 i = 0; i < words; i += 61)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

// Diagnostics: what a texture looks like once in VRAM (level 0 read back)
void
debugRaster(Raster *raster, const char *tag)
{
	if(raster == nil){
		rsxLog("[tex] %s: no raster", tag);
		return;
	}
	Ps3Raster *natras = GETPS3RASTEREXT(raster);
	int32 w = raster->originalWidth, h = raster->originalHeight;
	uint32 a0 = 0, a255 = 0, black = 0, white = 0, n = w*h;
	if(natras->vram && (natras->validLevels & 1) && n){
		uint8 *px = (uint8*)malloc(n*4);
		if(px){
			readbackLevel(raster, natras, 0, px);
			for(uint32 i = 0; i < n; i++){
				uint8 *p = px + i*4;
				if(p[3] == 0) a0++;
				else if(p[3] == 255) a255++;
				if(p[0] < 16 && p[1] < 16 && p[2] < 16) black++;
				else if(p[0] > 239 && p[1] > 239 && p[2] > 239) white++;
			}
			free(px);
		}
	}
	rsxLog("[tex] %s: %dx%d type %d fmt 0x%x, %s%s, levels %d (valid 0x%x), %s; "
	       "alpha 0: %u%%, 255: %u%%; black %u%%, white %u%%",
	       tag, w, h, raster->type, raster->format,
	       natras->swizzled ? "swizzled" : "linear", natras->isCompressed ? " DXT" : "",
	       natras->numLevels, natras->validLevels, natras->hasAlpha ? "alpha" : "opaque",
	       n ? a0*100/n : 0, n ? a255*100/n : 0, n ? black*100/n : 0, n ? white*100/n : 0);
}

void
probeDecodeDXT(int32 dxt, uint8 *dst, int32 w, int32 h, const uint8 *src)
{
	decodeDXT(dxt, dst, w, h, src);
}

static Raster *white;

Raster*
whiteRaster(void)
{
	return white;
}

void
rasterInitRSX(void)
{
	white = Raster::create(2, 2, 32, Raster::C8888 | Raster::TEXTURE, PLATFORM_PS3);
	if(white){
		uint8 *px = white->lock(0, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		if(px)
			memset(px, 0xFF, 2*2*4);
		white->unlock(0);
	}
	tjStart();
}

void
rasterShutdownRSX(void)
{
	if(white){
		white->destroy();
		white = nil;
	}
	tjStop();
	convFree(&mainCtx);
}


// ---- fast path for the PC TXDs' palettized textures -------------------------
// Most of GTA III's textures are 8 bit palettized D3D8 ones. librw's way
// (readAsImage: indices -> Image -> imageFindRasterFormat scan -> lock with a
// zeroed staging buffer -> per texel converter -> swizzle -> VRAM) went over
// every texel five times and was ~17 ms per TXD on the PPU. Here the indices
// go through a 256 entry A8R8G8B8 table straight into swizzled order.

static void
uploadIndexedLevel(Raster *raster, Ps3Raster *natras, int32 level, const uint8 *idx, const uint32 *pal)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	if(level >= natras->numLevels || natras->vram == nil)
		return;
	if(natras->vramFmt == PS3_VRAM_DXT){
		// kept as DXT (VRAM full, or an HD radar tile): the texels in RGBA8,
		// then compressed. Written as A8R8G8B8 here before, past the end
		// of the smaller DXT storage: other textures' VRAM got trampled
		// (the green blocks on Ren's map)
		uint8 *rgba = convBuf(&mainCtx, &mainCtx.decoded, w*h*4);
		if(rgba == nil)
			return;
		for(int32 k = 0; k < w*h; k++){
			uint32 c = pal[idx[k]];
			rgba[k*4+0] = c>>16; rgba[k*4+1] = c>>8; rgba[k*4+2] = c; rgba[k*4+3] = c>>24;
		}
		encodeDXTLevel(raster, natras, level, rgba, &mainCtx);
		return;
	}
	if(natras->swizzled){
		uint32 bytes = w*h*4;
		uint8 *buf = getScratch(&mainCtx, bytes + (w+h)*4);
		if(buf == nil)
			return;
		uint32 *pix = (uint32*)buf;
		uint32 *tx = (uint32*)(buf + bytes);
		uint32 *ty = tx + w;
		swizzleTables(tx, ty, w, h);
		swizzleStore(pix, w, h, tx, ty, [idx, pal](int32 i){ return pal[idx[i]]; });
		memcpy(natras->vram + natras->levelOffset[level], pix, bytes);
		vramFlush(natras->vram + natras->levelOffset[level], bytes);
	}else{
		uint32 *row = (uint32*)getScratch(&mainCtx, w*4);
		if(row == nil)
			return;
		for(int32 y = 0; y < h; y++){
			for(int32 x = 0; x < w; x++)
				row[x] = pal[*idx++];
			memcpy(natras->vram + y*natras->pitch, row, w*4);
		}
		vramFlush(natras->vram, natras->pitch*h);
	}
	natras->validLevels |= 1<<level;
	drawStats.texUploadBytes += w*h*4;
	texProfile.levels++;
	texProfile.texels += w*h;
	textureCacheTouched();
}

bool
readPalettedD3D8(Stream *stream, int32 width, int32 height, int32 format, int32 numLevels, Raster **out)
{
	*out = nil;
	if(rw::platform != PLATFORM_PS3 || !(format & (Raster::PAL4|Raster::PAL8)))
		return false;
	// mip levels made on the CPU from level 0: leave those to the old path
	if((format & (Raster::MIPMAP|Raster::AUTOMIPMAP)) == (Raster::MIPMAP|Raster::AUTOMIPMAP))
		return false;
	if(width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return false;

	uint64 t0 = texProfNow();
	uint8 palette[256*4];
	memset(palette, 0, sizeof(palette));
	if(format & Raster::PAL4)
		stream->read8(palette, 4*32);
	else
		stream->read8(palette, 4*256);
	bool opaque = !Raster::formatHasAlpha(format);
	uint32 pal[256];
	uint8 palA[256];
	for(int32 i = 0; i < 256; i++){
		uint8 *c = &palette[i*4];
		uint8 a = opaque ? 0xFF : c[3];
		palA[i] = a;
		pal[i] = (uint32)a<<24 | (uint32)c[0]<<16 | (uint32)c[1]<<8 | c[2];
	}

	uint8 *data = nil;
	uint32 dataSize = 0;
	Raster *ras = nil;
	bool failed = false;
	for(int32 i = 0; i < numLevels; i++){
		uint32 size = stream->readU32();
		int32 lw = levelDim(width, i);
		int32 lh = levelDim(height, i);
		// levels the raster doesn't keep, and anything after a bad one,
		// are skipped (the stream has to end up after the texture anyway)
		if(failed || (ras && i >= ras->getNumLevels()) || size < (uint32)(lw*lh)){
			stream->seek(size);
			if(ras == nil)
				failed = true;
			continue;
		}
		if(size > dataSize){
			rwFree(data);
			data = rwNewT(uint8, size, MEMDUR_FUNCTION | ID_IMAGE);
			dataSize = data ? size : 0;
		}
		if(data == nil){
			stream->seek(size);
			failed = true;
			continue;
		}
		stream->read8(data, size);
		if(ras == nil){
			// level 0 says whether the texture has alpha (as the scan of
			// imageFindRasterFormat did)
			uint8 minA = 0xFF;
			int32 n = lw*lh;
			for(int32 k = 0; k < n; k++)
				minA &= palA[data[k]];
			int32 rf = (minA != 0xFF ? Raster::C8888 : Raster::C888) | (format & 7) |
			           (format & (Raster::MIPMAP | Raster::AUTOMIPMAP));
			ras = Raster::create(width, height, 32, rf);
			if(ras == nil){
				failed = true;
				continue;
			}
			rasterStats.numExpanded++;
		}
		uploadIndexedLevel(ras, GETPS3RASTEREXT(ras), i, data, pal);
		Ps3Raster *pn = GETPS3RASTEREXT(ras);
		if(i == 0 && pn->autogenMipmap && pn->numLevels > 1 && pn->vram){
			// MIPMAPS option: the chain from level 0 (the TXD's own levels,
			// if any, are written over it next)
			uint8 *rgba = (uint8*)rwMalloc(lw*lh*4, MEMDUR_EVENT | ID_DRIVER);
			if(rgba){
				for(int32 k = 0; k < lw*lh; k++){
					uint32 c = pal[data[k]];
					rgba[k*4+0] = c>>16; rgba[k*4+1] = c>>8; rgba[k*4+2] = c; rgba[k*4+3] = c>>24;
				}
				uploadMipChain(ras, pn, rgba, 0, &mainCtx);
				rwFree(rgba);
			}
		}
	}
	rwFree(data);
	if(ras)
		GETPS3RASTEREXT(ras)->serial++;
	texProfile.us[TEXPROF_INDEXED] += texProfNow() - t0;
	texProfile.indexed++;
	*out = ras;
	return true;
}

#endif	// RW_PS3_RSX

// ============================================================================
// Common
// ============================================================================

static Raster*
rasterCreateTexture(Raster *raster)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	if(raster->format & (Raster::PAL4 | Raster::PAL8)){
		// palettized rasters come in as D3D8 ones and get expanded
		RWERROR((ERR_NOTEXTURE));
		return nil;
	}

	switch(raster->format & 0xF00){
	case Raster::C8888:
	case Raster::C1555:
	case Raster::C4444:
		natras->hasAlpha = 1;
		break;
	case Raster::C888:
	case Raster::C565:
	case Raster::C555:
		natras->hasAlpha = 0;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}
	// everything is RGBA8 for the game; the format says what the pixels
	// look like when locked
	raster->format = (raster->format & ~0xF00) | (natras->hasAlpha ? Raster::C8888 : Raster::C888);
	natras->texFormat = PS3_TEX_RGBA8;
	natras->bpp = 4;
	raster->depth = 32;
	raster->stride = raster->width*natras->bpp;

	natras->numLevels = 1;
	if(raster->format & Raster::MIPMAP)
		natras->numLevels = fullMipChain(raster->width, raster->height);
	natras->autogenMipmap = (raster->format & (Raster::MIPMAP|Raster::AUTOMIPMAP)) == (Raster::MIPMAP|Raster::AUTOMIPMAP);
	forceMipChain(raster, natras);

#ifdef RW_PS3_RSX
	// big RGBA textures the game asked to keep as DXT (HD radar tiles)
	if((loadHints & PS3_LOAD_DXT) && dxtNative && isPow2(raster->width) && isPow2(raster->height) &&
	   raster->width*raster->height >= 256*256){
		natras->vramFmt = PS3_VRAM_DXT;
		natras->vramDXT = natras->hasAlpha ? 5 : 1;
	}
	// VRAM full for an RGBA texture as it is (HD texture packs: a map of 64
	// 512x512 tiles is 87 MB in RGBA8, Ren's re3 log): compressed into DXT
	// it takes 4 (alpha, DXT5) or 8 times (DXT1) less, instead of drawing
	// white. Power of two sizes, 4x4 or more, as the RSX's DXT needs.
	if(!allocateTexels(raster, natras) && dxtNative && natras->vram == nil &&
	   isPow2(raster->width) && isPow2(raster->height) && raster->width >= 4 && raster->height >= 4){
		natras->vramFmt = PS3_VRAM_DXT;
		natras->vramDXT = natras->hasAlpha ? 5 : 1;
		if(allocateTexels(raster, natras)){
			if(rasterStats.packedDXT++ < 5)
				printf("[rsx] VRAM full: a %dx%d texture kept as DXT%d instead\n",
				       raster->width, raster->height, natras->vramDXT);
			return raster;
		}
		natras->vramFmt = PS3_VRAM_ARGB8;
		natras->vramDXT = 0;
	}
	if(natras->vram == nil){
#else
	if(!allocateTexels(raster, natras)){
#endif
#ifdef RW_PS3_RSX
		// VRAM full: keep the raster without storage (it draws white)
		// instead of failing the whole TXD; a nil raster here was a crash
		// in Raster::convertTexToCurrentPlatform
		static int warned;
		if(warned < 10){
			printf("[rsx] WARNING: %dx%d texture left without VRAM (it will draw white)\n",
			       raster->width, raster->height);
			warned++;
		}
		rasterStats.noVram++;
		return raster;
#else
		return nil;
#endif
	}
	return raster;
}

static Raster*
rasterCreateCameraTexture(Raster *raster)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	natras->hasAlpha = 1;
	raster->format = (raster->format & ~0xF00) | Raster::C8888;
	natras->texFormat = PS3_TEX_RGBA8;
	natras->bpp = 4;
	raster->depth = 32;
	raster->stride = raster->width*natras->bpp;
	natras->numLevels = 1;
	natras->autogenMipmap = 0;
	if(!allocateSurface(raster, natras, true))
		return nil;
	return raster;
}

static Raster*
rasterCreateCamera(Raster *raster)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	raster->format = (raster->format & ~0xF00) | Raster::C888;
	natras->texFormat = PS3_TEX_NONE;
	natras->hasAlpha = 0;
	natras->bpp = 4;
	raster->depth = 32;
	raster->stride = raster->width*natras->bpp;
	natras->numLevels = 1;
	if(!allocateSurface(raster, natras, false))
		return nil;
	return raster;
}

static Raster*
rasterCreateZbuffer(Raster *raster)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	natras->texFormat = PS3_TEX_NONE;
	natras->bpp = 4;
	raster->depth = 32;
	raster->stride = 0;
	natras->numLevels = 1;
	if(!allocateSurface(raster, natras, false))
		return nil;
	return raster;
}

void
allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	assert(raster->type == Raster::TEXTURE);
	switch(dxt){
	case 1:
		natras->texFormat = PS3_TEX_DXT1;
		raster->stride = raster->width/2;	// 4x4 in 8 bytes
		break;
	case 3:
		natras->texFormat = PS3_TEX_DXT3;
		raster->stride = raster->width;		// 4x4 in 16 bytes
		break;
	case 5:
		natras->texFormat = PS3_TEX_DXT5;
		raster->stride = raster->width;
		break;
	default:
		assert(0 && "invalid DXT format");
		return;
	}
	natras->isCompressed = 1;
	natras->hasAlpha = hasAlpha;
	natras->bpp = 2;
	raster->depth = 16;
	natras->numLevels = 1;
	if(raster->format & Raster::MIPMAP)
		natras->numLevels = numLevels;
	// no CPU mipmap generation for DXT: use what the TXD has (unless the
	// MIPMAPS option asks for them: made from the decompressed level 0)
	natras->autogenMipmap = 0;
	forceMipChain(raster, natras);
#ifdef RW_PS3_RSX
	// patch 41: power of two (and 4x4 or more) DXT stays DXT, the RSX
	// samples it. Else DXT1 in 16 bits (level 0's upload picks 565 or 1555)
	if(!hasStorage(natras)){
		if(dxtNative && isPow2(raster->width) && isPow2(raster->height) &&
		   raster->width >= 4 && raster->height >= 4)
			natras->vramFmt = PS3_VRAM_DXT;
		else if(dxt == 1 && dxt1As16)
			natras->vramFmt = PS3_VRAM_ARGB1555;
	}
#endif

	if(!hasStorage(natras))
		allocateTexels(raster, natras);
	rasterStats.numDXT++;

	raster->originalStride = raster->stride;
	raster->flags &= ~Raster::DONTALLOCATE;
}

Raster*
rasterCreate(Raster *raster)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	natras->isCompressed = 0;
	natras->hasAlpha = 0;
	natras->numLevels = 1;
	natras->texFormat = PS3_TEX_NONE;
	natras->vramFmt = PS3_VRAM_ARGB8;
	natras->vramDXT = 0;
	natras->lockedLevel = -1;

	Raster *ret = raster;

	if(raster->width == 0 || raster->height == 0){
		raster->flags |= Raster::DONTALLOCATE;
		raster->stride = 0;
		goto ret;
	}
	if(raster->flags & Raster::DONTALLOCATE)
		goto ret;

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
		ret = rasterCreateTexture(raster);
		break;
	case Raster::CAMERATEXTURE:
		ret = rasterCreateCameraTexture(raster);
		break;
	case Raster::ZBUFFER:
		ret = rasterCreateZbuffer(raster);
		break;
	case Raster::CAMERA:
		ret = rasterCreateCamera(raster);
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return nil;
	}

ret:
	raster->originalWidth = raster->width;
	raster->originalHeight = raster->height;
	raster->originalStride = raster->stride;
	raster->originalPixels = raster->pixels;
	return ret;
}

uint8*
rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);
	uint8 *px;

	assert(raster->privateFlags == 0);

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		px = lockTexture(raster, natras, level, lockMode);
		if(px == nil)
			return nil;
		for(int32 i = 0; i < level; i++){
			if(raster->width > 1){
				raster->width /= 2;
				raster->stride /= 2;
			}
			if(raster->height > 1)
				raster->height /= 2;
		}
		raster->pixels = px;
		raster->privateFlags = lockMode;
		return px;

	case Raster::CAMERA:
	case Raster::ZBUFFER:
		if(lockMode & Raster::PRIVATELOCK_WRITE)
			assert(0 && "can't lock framebuffer for writing");
		px = lockSurface(raster, natras, lockMode);
		raster->pixels = px;
		raster->privateFlags = lockMode;
		return px;

	default:
		return nil;
	}
}

void
rasterUnlock(Raster *raster, int32 level)
{
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	switch(raster->type){
	case Raster::NORMAL:
	case Raster::TEXTURE:
	case Raster::CAMERATEXTURE:
		unlockTexture(raster, natras, level);
		break;
	case Raster::CAMERA:
	case Raster::ZBUFFER:
		unlockSurface(raster, natras);
		break;
	}

	raster->width = raster->originalWidth;
	raster->height = raster->originalHeight;
	raster->stride = raster->originalStride;
	raster->pixels = raster->originalPixels;
	raster->privateFlags = 0;
}

int32
rasterNumLevels(Raster *raster)
{
	return GETPS3RASTEREXT(raster)->numLevels;
}

bool32
imageFindRasterFormat(Image *img, int32 type,
	int32 *pWidth, int32 *pHeight, int32 *pDepth, int32 *pFormat)
{
	int32 width, height, depth, format;

	assert((type&0xF) == Raster::TEXTURE);

	width = img->width;
	height = img->height;
	depth = img->depth;
	if(depth <= 8)
		depth = 32;

	switch(depth){
	case 32:
	case 24:
	case 16:
		format = img->hasAlpha() ? Raster::C8888 : Raster::C888;
		break;
	default:
		RWERROR((ERR_INVRASTER));
		return 0;
	}
	depth = 32;
	format |= type;

	*pWidth = width;
	*pHeight = height;
	*pDepth = depth;
	*pFormat = format;
	return 1;
}

bool32
rasterFromImage(Raster *raster, Image *image)
{
	if((raster->type&0xF) != Raster::TEXTURE)
		return 0;

	void (*conv)(uint8 *out, uint8 *in) = nil;

	// Unpalettize image if necessary but don't change original
	Image *truecolimg = nil;
	if(image->depth <= 8){
		truecolimg = Image::create(image->width, image->height, image->depth);
		truecolimg->pixels = image->pixels;
		truecolimg->stride = image->stride;
		truecolimg->palette = image->palette;
		truecolimg->unpalettize();
		image = truecolimg;
		rasterStats.numExpanded++;
	}

	Ps3Raster *natras = GETPS3RASTEREXT(raster);
	if(natras->isCompressed || natras->texFormat != PS3_TEX_RGBA8){
		if(truecolimg)
			truecolimg->destroy();
		RWERROR((ERR_INVRASTER));
		return 0;
	}
	switch(image->depth){
	case 32: conv = conv_RGBA8888_from_RGBA8888; break;
	case 24: conv = conv_RGBA8888_from_RGB888; rasterStats.numExpanded++; break;
	case 16: conv = conv_RGBA8888_from_ARGB1555; rasterStats.numExpanded++; break;
	default:
		if(truecolimg)
			truecolimg->destroy();
		RWERROR((ERR_INVRASTER));
		return 0;
	}

	natras->hasAlpha = image->hasAlpha();

	bool unlock = false;
	if(raster->pixels == nil){
		raster->lock(0, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		unlock = true;
	}

	uint8 *pixels = raster->pixels;
	if(pixels == nil){
		if(truecolimg)
			truecolimg->destroy();
		return 0;
	}
	uint8 *imgpixels = image->pixels;

	assert(image->width == raster->width);
	assert(image->height == raster->height);
	uint64 tconv = texProfNow();
	for(int32 y = 0; y < image->height; y++){
		uint8 *imgrow = imgpixels;
		uint8 *rasrow = pixels;
		for(int32 x = 0; x < image->width; x++){
			conv(rasrow, imgrow);
			imgrow += image->bpp;
			rasrow += natras->bpp;
		}
		imgpixels += image->stride;
		pixels += raster->stride;
	}
	texProfile.us[TEXPROF_FROMIMAGE] += texProfNow() - tconv;
	if(unlock)
		raster->unlock(0);

	if(truecolimg)
		truecolimg->destroy();
	return 1;
}

Image*
rasterToImage(Raster *raster)
{
	Image *image;
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	if(!hasStorage(natras) && raster->type != Raster::CAMERA)
		return nil;

	bool unlock = false;
	if(raster->pixels == nil){
		if(raster->lock(0, Raster::LOCKREAD) == nil)
			return nil;
		unlock = true;
	}

	if(natras->isCompressed){
		int w = raster->width < 4 ? 4 : raster->width;
		int h = raster->height < 4 ? 4 : raster->height;
		image = Image::create(w, h, 32);
		image->allocate();
		int dxt = natras->texFormat == PS3_TEX_DXT1 ? 1 :
		          natras->texFormat == PS3_TEX_DXT3 ? 3 : 5;
		image->setPixelsDXT(dxt, raster->pixels);
		if(dxt == 1 && !natras->hasAlpha)
			image->removeMask();
		image->width = raster->width;
		image->height = raster->height;
		if(unlock)
			raster->unlock(0);
		return image;
	}

	image = Image::create(raster->width, raster->height, 32);
	image->allocate();
	uint8 *imgpixels = image->pixels;
	uint8 *pixels = raster->pixels;
	int32 stride = raster->type == Raster::CAMERA ? raster->width*4 : raster->stride;
	for(int32 y = 0; y < image->height; y++){
		memcpy(imgpixels, pixels, image->width*4);
		imgpixels += image->stride;
		pixels += stride;
	}
	if(unlock)
		raster->unlock(0);
	return image;
}

static void*
createNativeRaster(void *object, int32 offset, int32)
{
	Ps3Raster *ras = PLUGINOFFSET(Ps3Raster, object, offset);
	memset(ras, 0, sizeof(Ps3Raster));
	ras->lockedLevel = -1;
	return object;
}

static void*
destroyNativeRaster(void *object, int32 offset, int32)
{
	Ps3Raster *natras = PLUGINOFFSET(Ps3Raster, object, offset);
	freeStorage(natras);
	return object;
}

static void*
copyNativeRaster(void *dst, void *, int32 offset, int32)
{
	Ps3Raster *d = PLUGINOFFSET(Ps3Raster, dst, offset);
	d->texels = nil;
	d->texelSize = 0;
	d->vram = nil;
	d->vramBlock = nil;
	d->vramSize = 0;
	d->staging = nil;
	d->validLevels = 0;
	d->tile = 0;
	d->zcomp = d->zcull = false;
	return dst;
}

// Our own native texture format (for a converted texture cache later on).
Texture*
readNativeTexture(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_PS3){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	tex->filterAddressing = stream->readU32();
	stream->read8(tex->name, 32);
	stream->read8(tex->mask, 32);

	uint32 format = stream->readU32();
	int32 width = stream->readI32();
	int32 height = stream->readI32();
	int32 depth = stream->readI32();
	int32 numLevels = stream->readI32();
	int32 flags = stream->readI32();
	int32 compression = stream->readI32();

	Raster *raster;
	if(flags & 2){
		raster = Raster::create(width, height, depth, format | Raster::TEXTURE | Raster::DONTALLOCATE, PLATFORM_PS3);
		allocateDXT(raster, compression, numLevels, flags & 1);
	}else{
		raster = Raster::create(width, height, depth, format | Raster::TEXTURE, PLATFORM_PS3);
	}
	if(raster == nil){
		tex->destroy();
		return nil;
	}
	tex->raster = raster;

	uint32 size;
	uint8 *data;
	for(int32 i = 0; i < numLevels; i++){
		size = stream->readU32();
		data = raster->lock(i, Raster::LOCKWRITE|Raster::LOCKNOFETCH);
		if(data)
			stream->read8(data, size);
		else
			stream->seek(size);
		raster->unlock(i);
	}
	return tex;
}

void
writeNativeTexture(Texture *tex, Stream *stream)
{
	Raster *raster = tex->raster;
	Ps3Raster *natras = GETPS3RASTEREXT(raster);

	int32 chunksize = getSizeNativeTexture(tex);
	writeChunkHeader(stream, ID_STRUCT, chunksize-12);
	stream->writeU32(PLATFORM_PS3);

	stream->writeU32(tex->filterAddressing);
	stream->write8(tex->name, 32);
	stream->write8(tex->mask, 32);

	int32 numLevels = natras->numLevels;
	stream->writeI32(raster->format);
	stream->writeI32(raster->width);
	stream->writeI32(raster->height);
	stream->writeI32(raster->depth);
	stream->writeI32(numLevels);

	int32 flags = 0;
	int32 compression = 0;
	if(natras->hasAlpha)
		flags |= 1;
	if(natras->isCompressed){
		flags |= 2;
		compression = natras->texFormat == PS3_TEX_DXT1 ? 1 :
		              natras->texFormat == PS3_TEX_DXT3 ? 3 : 5;
	}
	stream->writeI32(flags);
	stream->writeI32(compression);

	for(int32 i = 0; i < numLevels; i++){
		uint32 size = levelSize(natras, raster->width, raster->height, i);
		stream->writeU32(size);
		uint8 *data = raster->lock(i, Raster::LOCKREAD);
		if(data)
			stream->write8(data, size);
		else{
			for(uint32 j = 0; j < size; j++)
				stream->writeU8(0);
		}
		raster->unlock(i);
	}
}

uint32
getSizeNativeTexture(Texture *tex)
{
	Ps3Raster *natras = GETPS3RASTEREXT(tex->raster);
	uint32 size = 12 + 72 + 28;
	for(int32 i = 0; i < natras->numLevels; i++)
		size += 4 + levelSize(natras, tex->raster->width, tex->raster->height, i);
	return size;
}

void
registerNativeRaster(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(Ps3Raster),
	                                            ID_RASTERPS3,
	                                            createNativeRaster,
	                                            destroyNativeRaster,
	                                            copyNativeRaster);
}

}
}
