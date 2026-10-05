// ps3mem.cpp -- VRAM allocator for the RSX renderer (stage 2).
//
// One big block of RSX local memory is taken from PSL1GHT's heap at start
// and managed here, with the bookkeeping in MAIN memory: PSL1GHT's own heap
// keeps its headers inside the block, i.e. in VRAM, and every allocation or
// free would make the CPU read VRAM (very slow). Thousands of textures and
// geometries come and go while the game streams.
//
// Best fit over segregated free lists (one per power of two), 128 byte
// granularity (enough for textures, vertex/index buffers and surfaces), free
// neighbours merged. Frees are deferred until the RSX has finished the frame
// in which the memory may last have been read.

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

namespace rw {
namespace ps3 {

#define VRAM_GRANULE	128u
#define NUM_BINS	32

struct VramBlock
{
	uint32 offset;		// from the pool's start
	uint32 size;
	VramBlock *prevPhys, *nextPhys;	// address order
	VramBlock *prevFree, *nextFree;	// in its bin, if free
	bool free;
};

static uint8 *poolBase;
static uint32 poolOffset;	// RSX offset of poolBase
static uint32 poolSize;
static uint32 poolUsed;
static uint32 pendingBytes;	// freed, waiting for the RSX to be done with them
static uint32 poolBlocks;
static VramBlock *bins[NUM_BINS];

// block nodes come from chunks of main memory, recycled through a free list
static VramBlock *spareNodes;

static VramBlock*
newNode(void)
{
	if(spareNodes == nil){
		const int n = 1024;
		VramBlock *chunk = (VramBlock*)malloc(n*sizeof(VramBlock));
		if(chunk == nil)
			return nil;
		for(int i = 0; i < n; i++){
			chunk[i].nextFree = spareNodes;
			spareNodes = &chunk[i];
		}
	}
	VramBlock *b = spareNodes;
	spareNodes = b->nextFree;
	memset(b, 0, sizeof(*b));
	return b;
}

static void
deleteNode(VramBlock *b)
{
	b->nextFree = spareNodes;
	spareNodes = b;
}

static int
binOf(uint32 size)
{
	int b = 0;
	while(size > 1 && b < NUM_BINS-1){
		size >>= 1;
		b++;
	}
	return b;
}

static void
binInsert(VramBlock *b)
{
	int i = binOf(b->size);
	b->free = true;
	b->prevFree = nil;
	b->nextFree = bins[i];
	if(bins[i])
		bins[i]->prevFree = b;
	bins[i] = b;
}

static void
binRemove(VramBlock *b)
{
	int i = binOf(b->size);
	if(b->prevFree)
		b->prevFree->nextFree = b->nextFree;
	else
		bins[i] = b->nextFree;
	if(b->nextFree)
		b->nextFree->prevFree = b->prevFree;
	b->free = false;
	b->prevFree = b->nextFree = nil;
}

bool
vramInit(void)
{
	// as much as PSL1GHT's heap gives, minus some for its own needs
	uint32 size;
	for(size = 240u<<20; size >= 32u<<20; size -= 4u<<20){
		poolBase = (uint8*)rsxMemalign(1<<20, size);
		if(poolBase)
			break;
	}
	if(poolBase == nil){
		printf("[rsx] FATAL: no VRAM pool\n");
		return false;
	}
	// leave a little to PSL1GHT (fragment programs etc. use rsxMemalign)
	rsxFree(poolBase);
	size -= 4u<<20;
	poolBase = (uint8*)rsxMemalign(1<<20, size);
	if(poolBase == nil){
		printf("[rsx] FATAL: no VRAM pool (second try)\n");
		return false;
	}
	poolSize = size;
	rsxAddressToOffset(poolBase, &poolOffset);
	poolUsed = 0;
	poolBlocks = 0;

	VramBlock *b = newNode();
	b->offset = 0;
	b->size = poolSize;
	binInsert(b);

	printf("[rsx] VRAM pool %u MB at offset 0x%08x\n", poolSize>>20, poolOffset);
	return true;
}

void
vramShutdown(void)
{
	if(poolBase){
		rsxFree(poolBase);
		poolBase = nil;
	}
}

static VramBlock*
allocBlock(uint32 size)
{
	// best fit in the first bin that can hold it, else first fit above
	for(int i = binOf(size); i < NUM_BINS; i++){
		VramBlock *best = nil;
		int n = 0;
		for(VramBlock *b = bins[i]; b; b = b->nextFree){
			if(b->size >= size && (best == nil || b->size < best->size)){
				best = b;
				if(b->size == size)
					break;
			}
			// bins above the requested one: any block fits, don't scan long
			if(i > binOf(size) && best)
				break;
			if(++n > 64 && best)
				break;
		}
		if(best == nil)
			continue;

		binRemove(best);
		if(best->size > size){
			// split: the rest stays free, right after
			VramBlock *rest = newNode();
			if(rest){
				rest->offset = best->offset + size;
				rest->size = best->size - size;
				rest->prevPhys = best;
				rest->nextPhys = best->nextPhys;
				if(best->nextPhys)
					best->nextPhys->prevPhys = rest;
				best->nextPhys = rest;
				best->size = size;
				binInsert(rest);
			}
		}
		return best;
	}
	return nil;
}

static void
freeBlock(VramBlock *b)
{
	poolUsed -= b->size;
	poolBlocks--;
	// merge with free neighbours
	VramBlock *n = b->nextPhys;
	if(n && n->free){
		binRemove(n);
		b->size += n->size;
		b->nextPhys = n->nextPhys;
		if(n->nextPhys)
			n->nextPhys->prevPhys = b;
		deleteNode(n);
	}
	VramBlock *p = b->prevPhys;
	if(p && p->free){
		binRemove(p);
		p->size += b->size;
		p->nextPhys = b->nextPhys;
		if(b->nextPhys)
			b->nextPhys->prevPhys = p;
		deleteNode(b);
		b = p;
	}
	binInsert(b);
}

// ---- deferred frees ---------------------------------------------------------

struct PendingFree
{
	VramBlock *block;
	uint32 frame;
};
static PendingFree *pending;
static int32 numPending, maxPending;

void
vramProcessFrees(void)
{
	uint32 done = completedFrame();
	int32 i = 0;
	while(i < numPending){
		if((int32)(done - pending[i].frame) >= 0){
			pendingBytes -= pending[i].block->size;
			freeBlock(pending[i].block);
			pending[i] = pending[--numPending];
		}else
			i++;
	}
}

void
vramFree(VramAlloc *a)
{
	if(a->block == nil)
		return;
	if(numPending >= maxPending){
		int32 n = maxPending ? maxPending*2 : 1024;
		PendingFree *p = (PendingFree*)realloc(pending, n*sizeof(PendingFree));
		if(p == nil){
			// no memory for the list: wait for the RSX, free now
			rsxFinishAll();
			freeBlock(a->block);
			memset(a, 0, sizeof(*a));
			return;
		}
		pending = p;
		maxPending = n;
	}
	// the RSX may read it until the frame being built is done
	pending[numPending].block = a->block;
	pending[numPending].frame = currentFrame();
	numPending++;
	pendingBytes += a->block->size;
	memset(a, 0, sizeof(*a));
}

bool
vramAlloc(VramAlloc *a, uint32 size, const char *what)
{
	memset(a, 0, sizeof(*a));
	if(poolBase == nil || size == 0)
		return false;
	size = (size + VRAM_GRANULE-1) & ~(VRAM_GRANULE-1);

	VramBlock *b = allocBlock(size);
	if(b == nil && numPending){
		// full: what the RSX is done with goes back first, then wait for it
		vramProcessFrees();
		b = allocBlock(size);
		if(b == nil){
			rsxFinishAll();
			vramProcessFrees();
			b = allocBlock(size);
		}
	}
	if(b == nil){
		static int warned;
		if(warned < 5){
			printf("[rsx] WARNING: VRAM full: %u KB for %s (used %u of %u MB in %u blocks)\n",
			       size>>10, what, poolUsed>>20, poolSize>>20, poolBlocks);
			warned++;
		}
		return false;
	}
	poolUsed += b->size;
	poolBlocks++;
	a->block = b;
	a->size = b->size;
	a->ptr = poolBase + b->offset;
	a->offset = poolOffset + b->offset;
	return true;
}

// The game's streaming budget counts the IMG sizes, but here every texel
// is 4 bytes (palettes and 16 bit formats get expanded): VRAM fills up
// long before the streaming memory does. CStreaming asks this before it
// loads anything and throws out unused models while it says yes.
uint32 vramEvictions;

bool
vramTight(void)
{
	if(poolBase == nil)
		return false;
	uint32 used = poolUsed - pendingBytes;
	uint32 margin = poolSize/6;	// ~36 MB of 216: room for a big texture or two
	return used + margin > poolSize;
}

// ---- tiled surfaces -----------------------------------------------------------
//
// The camera's colour and depth buffers in tile regions (as the PSL1GHT
// rsxtest_tile sample and every PS3 game): the RSX's memory controller then
// interleaves their rows across the memory banks, the depth buffer is
// compressed (Z32_SEPSTENCIL) and gets ZCULL, which throws out hidden
// fragments in blocks before they are shaded, read or blended. All of it is
// transparent to the texture, transfer and CPU accesses (except reading the
// compressed depth from the CPU, which nobody does). A linear surface makes
// the RSX read and write memory row by row: every blended fragment (smoke,
// rain, coronas) costs more than it should at 720p.
//
// rsxTiling false (USRDIR/notiles.txt, read by the game before the first
// camera): plain linear surfaces, as before patch 29.

bool rsxTiling = true;
static uint32 tilesUsed;	// bit n: tile region n (15 of them)
static bool zcullUsed, zcompUsed;

bool
zcullActive(void)
{
	return zcullUsed;
}

bool
vramAllocSurface(VramAlloc *a, int32 width, int32 height, bool depth, uint32 *pitch, SurfaceTiles *t)
{
	memset(t, 0, sizeof(*t));
	if(!rsxTiling || width <= 0 || height <= 0)
		return false;
	uint32 p = gcmGetTiledPitchSize(width*4);
	if(p == 0 || p < (uint32)width*4)
		return false;
	int32 idx;
	for(idx = 0; idx < 15; idx++)
		if((tilesUsed & (1u<<idx)) == 0)
			break;
	if(idx == 15)
		return false;
	uint32 rows = (height + GCM_TILE_LOCAL_ALIGN_HEIGHT-1) & ~(GCM_TILE_LOCAL_ALIGN_HEIGHT-1);
	uint32 size = (p*rows + GCM_TILE_ALIGN_SIZE-1) & ~(GCM_TILE_ALIGN_SIZE-1);
	// the pool hands out 128 byte granules: room to start on a 64 KB boundary
	VramAlloc raw;
	if(!vramAlloc(&raw, size + GCM_TILE_ALIGN_OFFSET, depth ? "tiled depth buffer" : "tiled render target"))
		return false;
	uint32 off = (raw.offset + GCM_TILE_ALIGN_OFFSET-1) & ~(GCM_TILE_ALIGN_OFFSET-1);

	// nothing in flight may touch the region while its layout changes
	rsxFinishAll();
	bool comp = depth && !zcompUsed;
	s32 r = gcmSetTileInfo(idx, GCM_LOCATION_RSX, off, size, p,
	                       comp ? GCM_COMPMODE_Z32_SEPSTENCIL : GCM_COMPMODE_DISABLED, 0, depth ? 2 : 0);
	if(r == 0)
		r = gcmBindTile(idx);
	if(r != 0){
		rsxLog("[rsx] WARNING: tile region %d for a %dx%d %s failed (%d): linear", idx, width, height,
		       depth ? "depth buffer" : "render target", (int)r);
		vramFree(&raw);
		return false;
	}
	tilesUsed |= 1u<<idx;
	t->tile = idx+1;
	if(comp){
		zcompUsed = true;
		t->zcomp = true;
	}
	if(depth && !zcullUsed){
		gcmSetZcull(0, off, (width+63) & ~63, (height+63) & ~63, 0, GCM_ZCULL_Z24S8, GCM_SURFACE_CENTER_1,
		            GCM_ZCULL_LESS, GCM_ZCULL_LONES, GCM_SCULL_SFUNC_LESS, 1, 0xff);
		zcullUsed = true;
		t->zcull = true;
	}

	*a = raw;
	a->ptr = raw.ptr + (off - raw.offset);
	a->offset = off;
	*pitch = p;
	rsxLog("[rsx] %dx%d %s tiled: region %d, pitch %u, %u KB%s%s", width, height,
	       depth ? "depth buffer" : "render target", idx, p, size>>10,
	       t->zcomp ? ", compressed" : "", t->zcull ? ", ZCULL" : "");
	return true;
}

void
vramFreeSurface(VramAlloc *a, SurfaceTiles *t)
{
	if(t->tile){
		rsxFinishAll();
		if(t->zcull){
			gcmUnbindZcull(0);
			zcullUsed = false;
		}
		gcmUnbindTile(t->tile-1);
		tilesUsed &= ~(1u<<(t->tile-1));
		if(t->zcomp)
			zcompUsed = false;
		rsxLog("[rsx] tile region %d released", t->tile-1);
	}
	memset(t, 0, sizeof(*t));
	vramFree(a);
}

void
vramStats(uint32 *used, uint32 *total, uint32 *blocks)
{
	*used = poolUsed;
	*total = poolSize;
	*blocks = poolBlocks;
}

}
}

#endif
