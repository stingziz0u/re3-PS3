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

#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace ps3 {

int32 nativeRasterOffset;
RasterStats rasterStats;
TexProfile texProfile;

uint64
texProfNow(void)
{
	return sysGetSystemTime();
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

void textureCacheTouched(void) { texCacheDirty = true; }
bool textureCacheDirty(void) { return texCacheDirty; }
void textureCacheClean(void) { texCacheDirty = false; }

static void
freeStorage(Ps3Raster *natras)
{
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

	natras->swizzled = isPow2(w) && isPow2(h);
	if(!natras->swizzled){
		// linear: one level, rows padded to 64 bytes
		natras->numLevels = 1;
		natras->autogenMipmap = 0;
		natras->pitch = (w*4 + 63) & ~63;
		natras->levelOffset[0] = 0;
		total = natras->pitch*h;
	}else{
		if(natras->numLevels > 13)
			natras->numLevels = 13;
		natras->pitch = 0;
		for(int32 i = 0; i < natras->numLevels; i++){
			natras->levelOffset[i] = total;
			total += levelDim(w, i)*levelDim(h, i)*4;
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

// Conversion buffer, grown on demand
static uint8 *scratch;
static uint32 scratchSize;

static uint8*
getScratch(uint32 size)
{
	if(size > scratchSize){
		free(scratch);
		scratch = (uint8*)malloc(size);
		scratchSize = scratch ? size : 0;
	}
	return scratch;
}

// One RGBA8 level (bytes R G B A, w x h) into VRAM as A8R8G8B8
static void
uploadLevel(Raster *raster, Ps3Raster *natras, int32 level, uint8 *rgba)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	if(level >= natras->numLevels || natras->vram == nil)
		return;

	if(natras->swizzled){
		uint32 bytes = w*h*4;
		uint8 *buf = getScratch(bytes + (w+h)*4);
		if(buf == nil)
			return;
		uint32 *pix = (uint32*)buf;
		uint32 *tx = (uint32*)(buf + bytes);
		uint32 *ty = tx + w;
		uint64 t0 = texProfNow();
		swizzleTables(tx, ty, w, h);
		uint8 *s = rgba;
		for(int32 y = 0; y < h; y++){
			uint32 oy = ty[y];
			for(int32 x = 0; x < w; x++, s += 4)
				pix[tx[x] | oy] = (uint32)s[3]<<24 | (uint32)s[0]<<16 | (uint32)s[1]<<8 | s[2];
		}
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
		uint32 *row = (uint32*)getScratch(w*4);
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
	natras->validLevels |= 1<<level;
	textureCacheTouched();
}

// VRAM (A8R8G8B8) back into RGBA8 bytes. Slow (the CPU reads VRAM): only
// for read locks, which the game hardly ever does.
static void
readbackLevel(Raster *raster, Ps3Raster *natras, int32 level, uint8 *rgba)
{
	int32 w = levelDim(raster->originalWidth, level);
	int32 h = levelDim(raster->originalHeight, level);
	uint8 *d = rgba;
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

static uint8*
lockTexture(Raster *raster, Ps3Raster *natras, int32 level, int32 lockMode)
{
	// no VRAM (it was full): the lock still hands out a buffer, the unlock
	// throws it away; a nil lock is a crash in librw's converters
	if(level < 0 || level >= 16)
		return nil;
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
		uint8 *rgba = natras->staging;
		Image *img = nil;
		int32 w = levelDim(raster->originalWidth, level);
		int32 h = levelDim(raster->originalHeight, level);
		if(natras->isCompressed){
			// DXT: decompressed here (the RSX could take the blocks, but
			// linear compressed textures don't repeat)
			int32 dw = w < 4 ? 4 : w;
			int32 dh = h < 4 ? 4 : h;
			img = Image::create(dw, dh, 32);
			img->allocate();
			int dxt = natras->texFormat == PS3_TEX_DXT1 ? 1 :
			          natras->texFormat == PS3_TEX_DXT3 ? 3 : 5;
			img->setPixelsDXT(dxt, natras->staging);
			if(dw != w || dh != h){
				// tiny level: the top left corner
				for(int32 y = 0; y < h; y++)
					memmove(img->pixels + y*w*4, img->pixels + y*img->stride, w*4);
			}
			rgba = img->pixels;
		}
		uploadLevel(raster, natras, level, rgba);
		if(level == 0 && natras->autogenMipmap && natras->numLevels > 1){
			uint8 *prev = rgba;
			int32 pw = w, ph = h;
			for(int32 i = 1; i < natras->numLevels; i++){
				int32 nw = levelDim(raster->originalWidth, i);
				int32 nh = levelDim(raster->originalHeight, i);
				uint8 *next = (uint8*)rwMalloc(nw*nh*4, MEMDUR_EVENT | ID_DRIVER);
				if(next == nil)
					break;
				uint64 tm = texProfNow();
				downsampleRGBA8(next, nw, nh, prev, pw, ph);
				texProfile.us[TEXPROF_MIPS] += texProfNow() - tm;
				uploadLevel(raster, natras, i, next);
				if(prev != rgba)
					rwFree(prev);
				prev = next;
				pw = nw;
				ph = nh;
			}
			if(prev != rgba)
				rwFree(prev);
		}
		if(img)
			img->destroy();
		natras->serial++;
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
	tex->format = GCM_TEXTURE_FORMAT_A8R8G8B8 |
		(natras->swizzled ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
	tex->mipmap = n;
	tex->dimension = GCM_TEXTURE_DIMS_2D;
	tex->cubemap = GCM_FALSE;
	tex->remap = 0x00AAE4;	// identity (ps3gl)
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
}

void
rasterShutdownRSX(void)
{
	if(white){
		white->destroy();
		white = nil;
	}
	free(scratch);
	scratch = nil;
	scratchSize = 0;
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
	if(natras->swizzled){
		uint32 bytes = w*h*4;
		uint8 *buf = getScratch(bytes + (w+h)*4);
		if(buf == nil)
			return;
		uint32 *pix = (uint32*)buf;
		uint32 *tx = (uint32*)(buf + bytes);
		uint32 *ty = tx + w;
		swizzleTables(tx, ty, w, h);
		for(int32 y = 0; y < h; y++){
			uint32 oy = ty[y];
			for(int32 x = 0; x < w; x++)
				pix[tx[x] | oy] = pal[*idx++];
		}
		memcpy(natras->vram + natras->levelOffset[level], pix, bytes);
		vramFlush(natras->vram + natras->levelOffset[level], bytes);
	}else{
		uint32 *row = (uint32*)getScratch(w*4);
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

	if(!allocateTexels(raster, natras)){
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
	// no CPU mipmap generation for DXT: use what the TXD has
	natras->autogenMipmap = 0;

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
