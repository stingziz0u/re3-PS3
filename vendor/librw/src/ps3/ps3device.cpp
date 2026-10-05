// PS3 render device: render states, programs, constants, cameras, video
// modes, driver registration.
//
// Built with RW_PS3_RSX (stage 2) it draws through the RSX; without it
// (stage 1, headless) the drawing functions are stubs that keep the state,
// so the whole game loop runs with the real data and no GPU.
//
// The RSX side follows librw's gl3 backend (same render state semantics,
// same shader maths), with these differences:
//  - vertex program constants live in fixed registers (shaders/common.cgh),
//    uploaded only when they change;
//  - the matrices are combined on the CPU (proj*view*world, one dp4 per row
//    in the program) and the lights are turned into model space (the
//    program never needs the world matrix);
//  - the alpha test is the RSX's own; the fog colour and factor reach the
//    fragment program as an interpolant (no fragment program constants:
//    those are patched in memory the RSX may be reading).

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
#include "../rwrender.h"
#include "../rwanim.h"
#include "../rwplugins.h"
#include "rwps3.h"
#ifdef RW_PS3_RSX
#include "ps3rsx.h"
#include "ps3shaders.h"
#include <sys/systime.h>
#endif

namespace rw {
namespace ps3 {

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster*, int32 level, int32 lockMode);
void rasterUnlock(Raster*, int32);
int32 rasterNumLevels(Raster*);
bool32 imageFindRasterFormat(Image *img, int32 type,
	int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);

// ---- video modes: internal resolutions (16:9), scaled to the TV ------------

static const struct { int32 w, h; } videoModes[] = {
	{ 1280, 720 },
	{ 1152, 648 },
	{ 1024, 576 },
	{ 960, 544 },
	{ 854, 480 },
	{ 640, 360 },
};
static int32 currentMode;

int32
getNumVideoModes(void)
{
	return nelem(videoModes);
}

void
getVideoModeSize(int32 mode, int32 *width, int32 *height)
{
	if(mode < 0 || mode >= (int32)nelem(videoModes))
		mode = 0;
	*width = videoModes[mode].w;
	*height = videoModes[mode].h;
}

#ifndef RW_PS3_RSX
// ============================================================================
// Stage 1: headless
// ============================================================================

void setVideoSettings(const VideoSettings *s) { (void)s; }
void getVideoSettings(VideoSettings *s) { s->screenFit = 100; s->bilinear = 1; s->reserved = 0; }

#define NUM_RENDERSTATES (GSALPHATESTREF+1)
static void *renderStates[NUM_RENDERSTATES];

static void
resetRenderStates(void)
{
	memset(renderStates, 0, sizeof(renderStates));
	renderStates[TEXTUREFILTER] = (void*)(uintptr)Texture::LINEAR;
	renderStates[TEXTUREADDRESS] = (void*)(uintptr)Texture::WRAP;
	renderStates[TEXTUREADDRESSU] = (void*)(uintptr)Texture::WRAP;
	renderStates[TEXTUREADDRESSV] = (void*)(uintptr)Texture::WRAP;
	renderStates[SRCBLEND] = (void*)(uintptr)BLENDSRCALPHA;
	renderStates[DESTBLEND] = (void*)(uintptr)BLENDINVSRCALPHA;
	renderStates[ZTESTENABLE] = (void*)(uintptr)1;
	renderStates[ZWRITEENABLE] = (void*)(uintptr)1;
	renderStates[CULLMODE] = (void*)(uintptr)CULLNONE;
	renderStates[STENCILFUNCTIONMASK] = (void*)(uintptr)0xFFFFFFFF;
	renderStates[STENCILFUNCTIONWRITEMASK] = (void*)(uintptr)0xFFFFFFFF;
	renderStates[ALPHATESTFUNC] = (void*)(uintptr)ALPHAGREATEREQUAL;
	renderStates[ALPHATESTREF] = (void*)(uintptr)10;
}

static void
setRenderState(int32 state, void *value)
{
	if(state >= 0 && state < NUM_RENDERSTATES){
		renderStates[state] = value;
		if(state == TEXTUREADDRESS){
			renderStates[TEXTUREADDRESSU] = value;
			renderStates[TEXTUREADDRESSV] = value;
		}
	}
}

static void*
getRenderState(int32 state)
{
	if(state == TEXTUREADDRESS){
		if(renderStates[TEXTUREADDRESSU] == renderStates[TEXTUREADDRESSV])
			return renderStates[TEXTUREADDRESSU];
		return 0;
	}
	if(state >= 0 && state < NUM_RENDERSTATES)
		return renderStates[state];
	return 0;
}

static void beginUpdate(Camera*) { }
static void endUpdate(Camera*) { }
static void clearCamera(Camera*, RGBA*, uint32) { }
static void showRaster(Raster*, uint32) { }
static bool32 rasterRenderFast(Raster*, int32, int32) { return 0; }
static void im2DRenderLine(void*, int32, int32, int32) { }
static void im2DRenderTriangle(void*, int32, int32, int32, int32) { }
static void im2DRenderPrimitive(PrimitiveType, void*, int32) { }
static void im2DRenderIndexedPrimitive(PrimitiveType, void*, int32, void*, int32) { }
static void im3DTransform(void*, int32, Matrix*, uint32) { }
static void im3DRenderPrimitive(PrimitiveType) { }
static void im3DRenderIndexedPrimitive(PrimitiveType, void*, int32) { }
static void im3DEnd(void) { }

static bool deviceInit(void) { return true; }
static bool deviceFinalize(void) { return true; }
static void deviceTerm(void) { }

#else
// ============================================================================
// Stage 2: RSX
// ============================================================================

// ---- programs ----------------------------------------------------------------

struct VertexProgram
{
	rsxVertexProgram *vp;
	void *ucode;
	uint32 internalMask;	// constant registers 0..31 the program loads itself
};
struct FragmentProgram
{
	rsxFragmentProgram *fp;
	void *ucode;		// copy in RSX memory
	uint32 size;
	uint32 offset;
	int32 numSamplers;
};

static VertexProgram vps[NUM_VP];
static FragmentProgram fps[NUM_FP];
static int32 curVP = -1, curFP = -1;
static int32 wantVP = VP_DEFAULT, wantFP = FP_SIMPLE;
static int32 drawFP = FP_SIMPLE;	// wantFP, or its additive variant (flushCache)
bool additiveKill = true;		// the autotest's rain test turns it off to compare
bool zcullOn = true;			// the autotest's GPU test turns it off to compare
int32 blendAlphaRef = 0;		// the autotest's GPU test tries it on the smoke
static bool zcullStale;			// depth written without the test since the last clear
static int32 zcullHave = -1;		// what the RSX was told (-1: nothing yet)

// constant registers: what the RSX has (shadow) and what we want
#define NUM_CONSTS 32
static float32 constWant[NUM_CONSTS][4];
static float32 constHave[NUM_CONSTS][4];
static uint32 constValid;	// bit n: constHave[n] is what the RSX has

enum {
	C_MVP = 0,
	C_MATCOLOR = 4,
	C_SURFPROPS = 5,
	C_AMBLIGHT = 6,
	C_FOGDATA = 7,
	C_FOGCOLOR = 8,
	C_XFORM = 9,
	C_ENVROW = 10,
	C_FXPARAMS = 12,
	C_COLORCLAMP = 13,
	C_LIGHTS = 16,
	C_BONES = 32,
	MAX_LIGHTS = 8
};

static void
loadVP(int32 id, const unsigned char *data, const char *name)
{
	VertexProgram *p = &vps[id];
	u32 size;
	p->vp = (rsxVertexProgram*)data;
	rsxVertexProgramGetUCode(p->vp, &p->ucode, &size);
	p->internalMask = 0;
	rsxProgramConst *consts = rsxVertexProgramGetConsts(p->vp);
	for(int i = 0; i < p->vp->num_const; i++)
		if(consts[i].is_internal && consts[i].index + p->vp->const_start < NUM_CONSTS)
			p->internalMask |= 1u << (consts[i].index + p->vp->const_start);
	rsxLog("[rsx] vertex program %s: %u instructions, inputs 0x%x, outputs 0x%x, internal consts 0x%x",
	       name, p->vp->num_insn, p->vp->input_mask, p->vp->output_mask, p->internalMask);
}

static bool
loadFP(int32 id, const unsigned char *data, const char *name, int32 numSamplers)
{
	FragmentProgram *p = &fps[id];
	void *ucode;
	p->fp = (rsxFragmentProgram*)data;
	rsxFragmentProgramGetUCode(p->fp, &ucode, &p->size);
	// the fragment program must be in RSX memory
	p->ucode = rsxMemalign(64, p->size);
	if(p->ucode == nil){
		rsxLog("[rsx] FATAL: rsxMemalign failed for fragment program %s", name);
		return false;
	}
	memcpy(p->ucode, ucode, p->size);
	vramFlush(p->ucode, p->size);
	rsxAddressToOffset(p->ucode, &p->offset);
	p->numSamplers = numSamplers;
	rsxLog("[rsx] fragment program %s: %u instructions, %u bytes", name, p->fp->num_insn, p->size);
	return true;
}

void
setVertexProgram(int vp)
{
	wantVP = vp;
}

void
setFragmentProgram(int fp)
{
	wantFP = fp;
}

// ---- render state --------------------------------------------------------------

struct RwRasterStateCache {
	Raster *raster;
	Texture::Addressing addressingU;
	Texture::Addressing addressingV;
	Texture::FilterMode filter;
};

#define MAXNUMSTAGES 2

struct RwStateCache {
	bool32 vertexAlpha;
	uint32 alphaTestEnable;
	uint32 alphaFunc;
	uint32 alphaRef;	// 0..255
	bool32 textureAlpha;
	bool32 blendEnable;
	uint32 srcblend, destblend;
	uint32 zwrite;
	uint32 ztest;
	uint32 cullmode;
	uint32 stencilenable;
	uint32 stencilpass;
	uint32 stencilfail;
	uint32 stencilzfail;
	uint32 stencilfunc;
	uint32 stencilref;
	uint32 stencilmask;
	uint32 stencilwritemask;
	uint32 fogEnable;
	float32 fogStart;
	float32 fogEnd;
	RGBAf fogColor;

	// emulation of PS2 GS
	bool32 gsalpha;
	uint32 gsalpharef;

	RwRasterStateCache texstage[MAXNUMSTAGES];
};
static RwStateCache rwStateCache;

// what the RSX has
struct HwState {
	int32 blend, src, dst;
	int32 alphaTest, alphaFunc, alphaRef;
	int32 depthTest, depthFunc, depthMask;
	int32 cull, cullFace;
	int32 stencil, stFunc, stRef, stMask, stWriteMask, stFail, stZFail, stPass;
};
static HwState hw;

struct HwSampler {
	Raster *raster;
	uint32 offset;		// the raster's storage (a new raster can reuse the address)
	int32 levels;
	int32 filter, addrU, addrV;
	bool enabled;
};
static HwSampler hwSampler[MAXNUMSTAGES];

static uint32 blendMap[] = {
	GCM_ZERO,	// actually invalid
	GCM_ZERO,
	GCM_ONE,
	GCM_SRC_COLOR,
	GCM_ONE_MINUS_SRC_COLOR,
	GCM_SRC_ALPHA,
	GCM_ONE_MINUS_SRC_ALPHA,
	GCM_DST_ALPHA,
	GCM_ONE_MINUS_DST_ALPHA,
	GCM_DST_COLOR,
	GCM_ONE_MINUS_DST_COLOR,
	GCM_SRC_ALPHA_SATURATE,
};

static uint32 stencilOpMap[] = {
	GCM_KEEP,	// actually invalid
	GCM_KEEP,
	GCM_ZERO,
	GCM_REPLACE,
	GCM_INCR,
	GCM_DECR,
	GCM_INVERT,
	GCM_INCR_WRAP,
	GCM_DECR_WRAP
};

static uint32 stencilFuncMap[] = {
	GCM_NEVER,	// actually invalid
	GCM_NEVER,
	GCM_LESS,
	0x0202,		// EQUAL
	GCM_LEQUAL,
	0x0204,		// GREATER
	0x0205,		// NOTEQUAL
	GCM_GEQUAL,
	GCM_ALWAYS
};

// does the current camera draw anywhere
static bool targetValid;
static int32 targetW, targetH;	// the bound surface

void
setAlphaBlend(bool32 enable)
{
	rwStateCache.blendEnable = enable;
}

bool32
getAlphaBlend(void)
{
	return rwStateCache.blendEnable;
}

static void
setAlphaTest(bool32 enable)
{
	rwStateCache.alphaTestEnable = enable;
}

static void
setVertexAlpha(bool32 enable)
{
	if(rwStateCache.vertexAlpha != enable){
		if(!rwStateCache.textureAlpha){
			setAlphaBlend(enable);
			setAlphaTest(enable);
		}
		rwStateCache.vertexAlpha = enable;
	}
}

static bool32
rasterHasAlpha(Raster *raster)
{
	return GETPS3RASTEREXT(raster->parent)->hasAlpha;
}

static void
setRasterStage(uint32 stage, Raster *raster)
{
	if(raster != rwStateCache.texstage[stage].raster){
		rwStateCache.texstage[stage].raster = raster;
		bool32 alpha = raster ? rasterHasAlpha(raster) : 0;
		if(stage == 0){
			if(alpha != rwStateCache.textureAlpha){
				rwStateCache.textureAlpha = alpha;
				if(!rwStateCache.vertexAlpha){
					setAlphaBlend(alpha);
					setAlphaTest(alpha);
				}
			}
		}
	}
}

void
setTexture(int32 stage, Texture *tex)
{
	if(stage >= MAXNUMSTAGES)
		return;
	if(tex == nil || tex->raster == nil){
		setRasterStage(stage, nil);
		return;
	}
	setRasterStage(stage, tex->raster);
	rwStateCache.texstage[stage].filter = (Texture::FilterMode)tex->getFilter();
	rwStateCache.texstage[stage].addressingU = (Texture::Addressing)tex->getAddressU();
	rwStateCache.texstage[stage].addressingV = (Texture::Addressing)tex->getAddressV();
}

static void
setRenderState(int32 state, void *pvalue)
{
	uint32 value = (uint32)(uintptr)pvalue;
	switch(state){
	case TEXTURERASTER:
		setRasterStage(0, (Raster*)pvalue);
		break;
	case TEXTUREADDRESS:
		rwStateCache.texstage[0].addressingU = (Texture::Addressing)value;
		rwStateCache.texstage[0].addressingV = (Texture::Addressing)value;
		break;
	case TEXTUREADDRESSU:
		rwStateCache.texstage[0].addressingU = (Texture::Addressing)value;
		break;
	case TEXTUREADDRESSV:
		rwStateCache.texstage[0].addressingV = (Texture::Addressing)value;
		break;
	case TEXTUREFILTER:
		rwStateCache.texstage[0].filter = (Texture::FilterMode)value;
		break;
	case VERTEXALPHA:
		setVertexAlpha(value);
		break;
	case SRCBLEND:
		if(value < nelem(blendMap))
			rwStateCache.srcblend = value;
		break;
	case DESTBLEND:
		if(value < nelem(blendMap))
			rwStateCache.destblend = value;
		break;
	case ZTESTENABLE:
		rwStateCache.ztest = value;
		break;
	case ZWRITEENABLE:
		rwStateCache.zwrite = value ? 1 : 0;
		break;
	case FOGENABLE:
		rwStateCache.fogEnable = value;
		break;
	case FOGCOLOR: {
		RGBA c;
		c.red = value;
		c.green = value>>8;
		c.blue = value>>16;
		c.alpha = value>>24;
		convColor(&rwStateCache.fogColor, &c);
		break;
	}
	case CULLMODE:
		rwStateCache.cullmode = value;
		break;

	case STENCILENABLE:
		rwStateCache.stencilenable = value;
		break;
	case STENCILFAIL:
		if(value < nelem(stencilOpMap))
			rwStateCache.stencilfail = value;
		break;
	case STENCILZFAIL:
		if(value < nelem(stencilOpMap))
			rwStateCache.stencilzfail = value;
		break;
	case STENCILPASS:
		if(value < nelem(stencilOpMap))
			rwStateCache.stencilpass = value;
		break;
	case STENCILFUNCTION:
		if(value < nelem(stencilFuncMap))
			rwStateCache.stencilfunc = value;
		break;
	case STENCILFUNCTIONREF:
		rwStateCache.stencilref = value;
		break;
	case STENCILFUNCTIONMASK:
		rwStateCache.stencilmask = value;
		break;
	case STENCILFUNCTIONWRITEMASK:
		rwStateCache.stencilwritemask = value;
		break;

	case ALPHATESTFUNC:
		rwStateCache.alphaFunc = value;
		break;
	case ALPHATESTREF:
		rwStateCache.alphaRef = value > 255 ? 255 : value;
		break;
	case GSALPHATEST:
		rwStateCache.gsalpha = value;
		break;
	case GSALPHATESTREF:
		rwStateCache.gsalpharef = value;
		break;
	}
}

static void*
getRenderState(int32 state)
{
	uint32 val;
	RGBA rgba;
	switch(state){
	case TEXTURERASTER:
		return rwStateCache.texstage[0].raster;
	case TEXTUREADDRESS:
		if(rwStateCache.texstage[0].addressingU == rwStateCache.texstage[0].addressingV)
			val = rwStateCache.texstage[0].addressingU;
		else
			val = 0;	// invalid
		break;
	case TEXTUREADDRESSU:
		val = rwStateCache.texstage[0].addressingU;
		break;
	case TEXTUREADDRESSV:
		val = rwStateCache.texstage[0].addressingV;
		break;
	case TEXTUREFILTER:
		val = rwStateCache.texstage[0].filter;
		break;
	case VERTEXALPHA:
		val = rwStateCache.vertexAlpha;
		break;
	case SRCBLEND:
		val = rwStateCache.srcblend;
		break;
	case DESTBLEND:
		val = rwStateCache.destblend;
		break;
	case ZTESTENABLE:
		val = rwStateCache.ztest;
		break;
	case ZWRITEENABLE:
		val = rwStateCache.zwrite;
		break;
	case FOGENABLE:
		val = rwStateCache.fogEnable;
		break;
	case FOGCOLOR:
		convColor(&rgba, &rwStateCache.fogColor);
		val = RWRGBAINT(rgba.red, rgba.green, rgba.blue, rgba.alpha);
		break;
	case CULLMODE:
		val = rwStateCache.cullmode;
		break;
	case STENCILENABLE:
		val = rwStateCache.stencilenable;
		break;
	case STENCILFAIL:
		val = rwStateCache.stencilfail;
		break;
	case STENCILZFAIL:
		val = rwStateCache.stencilzfail;
		break;
	case STENCILPASS:
		val = rwStateCache.stencilpass;
		break;
	case STENCILFUNCTION:
		val = rwStateCache.stencilfunc;
		break;
	case STENCILFUNCTIONREF:
		val = rwStateCache.stencilref;
		break;
	case STENCILFUNCTIONMASK:
		val = rwStateCache.stencilmask;
		break;
	case STENCILFUNCTIONWRITEMASK:
		val = rwStateCache.stencilwritemask;
		break;
	case ALPHATESTFUNC:
		val = rwStateCache.alphaFunc;
		break;
	case ALPHATESTREF:
		val = rwStateCache.alphaRef;
		break;
	case GSALPHATEST:
		val = rwStateCache.gsalpha;
		break;
	case GSALPHATESTREF:
		val = rwStateCache.gsalpharef;
		break;
	default:
		val = 0;
	}
	return (void*)(uintptr)val;
}

static void
resetRenderState(void)
{
	memset(&rwStateCache, 0, sizeof(rwStateCache));
	rwStateCache.alphaFunc = ALPHAGREATEREQUAL;
	rwStateCache.alphaRef = 10;
	rwStateCache.fogColor.red = rwStateCache.fogColor.green = 1.0f;
	rwStateCache.fogColor.blue = rwStateCache.fogColor.alpha = 1.0f;
	rwStateCache.gsalpha = 0;
	rwStateCache.gsalpharef = 128;
	rwStateCache.srcblend = BLENDSRCALPHA;
	rwStateCache.destblend = BLENDINVSRCALPHA;
	rwStateCache.zwrite = 1;
	rwStateCache.ztest = 1;
	rwStateCache.cullmode = CULLNONE;
	rwStateCache.stencilfail = STENCILKEEP;
	rwStateCache.stencilzfail = STENCILKEEP;
	rwStateCache.stencilpass = STENCILKEEP;
	rwStateCache.stencilfunc = STENCILALWAYS;
	rwStateCache.stencilmask = 0xFFFFFFFF;
	rwStateCache.stencilwritemask = 0xFFFFFFFF;
	for(int i = 0; i < MAXNUMSTAGES; i++){
		rwStateCache.texstage[i].filter = Texture::LINEAR;
		rwStateCache.texstage[i].addressingU = Texture::WRAP;
		rwStateCache.texstage[i].addressingV = Texture::WRAP;
	}

	// everything goes to the RSX again
	memset(&hw, 0xFF, sizeof(hw));
	for(int i = 0; i < MAXNUMSTAGES; i++){
		hwSampler[i].raster = (Raster*)~(uintptr)0;
		hwSampler[i].enabled = true;
	}
	constValid = 0;
	curVP = curFP = -1;
}

// RSX defaults that never change
static void
initHardwareState(void)
{
	gcmContextData *ctx = rsxCtx;
	rsxSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxSetPolygonOffsetFillEnable(ctx, GCM_FALSE);
	rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	rsxSetDitherEnable(ctx, GCM_TRUE);
}

// The RSX's half of the render state
static void
applyStates(void)
{
	gcmContextData *ctx = rsxCtx;
	RwStateCache &s = rwStateCache;

	int32 blend = s.blendEnable ? 1 : 0;
	if(hw.blend != blend){
		hw.blend = blend;
		rsxSetBlendEnable(ctx, blend ? GCM_TRUE : GCM_FALSE);
	}
	if(blend && (hw.src != (int32)s.srcblend || hw.dst != (int32)s.destblend)){
		hw.src = s.srcblend;
		hw.dst = s.destblend;
		rsxSetBlendFunc(ctx, blendMap[s.srcblend], blendMap[s.destblend],
		                blendMap[s.srcblend], blendMap[s.destblend]);
	}

	int32 afunc = s.alphaTestEnable ? s.alphaFunc : ALPHAALWAYS;
	int32 atest = afunc != ALPHAALWAYS;
	if(hw.alphaTest != atest){
		hw.alphaTest = atest;
		rsxSetAlphaTestEnable(ctx, atest ? GCM_TRUE : GCM_FALSE);
	}
	// alpha blended without Z write (smoke, particles): fragments too
	// transparent to show skip the blend
	int32 aref = s.alphaRef;
	if(blendAlphaRef > aref && blend && !s.zwrite && afunc == ALPHAGREATEREQUAL &&
	   s.srcblend == BLENDSRCALPHA && s.destblend == BLENDINVSRCALPHA)
		aref = blendAlphaRef;
	if(atest && (hw.alphaFunc != afunc || hw.alphaRef != aref)){
		hw.alphaFunc = afunc;
		hw.alphaRef = aref;
		rsxSetAlphaFunc(ctx, afunc == ALPHALESS ? GCM_LESS : GCM_GEQUAL, aref);
	}

	// as gl3: writing without testing = test ALWAYS
	int32 dtest, dfunc;
	if(s.ztest){
		dtest = 1;
		dfunc = GCM_LEQUAL;
	}else if(s.zwrite){
		dtest = 1;
		dfunc = GCM_ALWAYS;
	}else{
		dtest = 0;
		dfunc = GCM_LEQUAL;
	}
	if(hw.depthTest != dtest){
		hw.depthTest = dtest;
		rsxSetDepthTestEnable(ctx, dtest ? GCM_TRUE : GCM_FALSE);
	}
	if(hw.depthFunc != dfunc){
		hw.depthFunc = dfunc;
		rsxSetDepthFunc(ctx, dfunc);
	}
	if(hw.depthMask != (int32)s.zwrite){
		hw.depthMask = s.zwrite;
		rsxSetDepthWriteEnable(ctx, s.zwrite ? GCM_TRUE : GCM_FALSE);
	}
	// ZCULL keeps the farthest depth of each block, for LESS/LEQUAL tests:
	// depth written with ALWAYS can go farther, so after that it is
	// rebuilt (invalidated) before the next tested draw
	if(dfunc == GCM_ALWAYS && s.zwrite)
		zcullStale = true;
	else if(zcullStale && dtest){
		rsxSetZCullInvalidate(ctx);
		zcullStale = false;
	}

	int32 cull = s.cullmode == CULLBACK || s.cullmode == CULLFRONT;
	if(hw.cull != cull){
		hw.cull = cull;
		rsxSetCullFaceEnable(ctx, cull ? GCM_TRUE : GCM_FALSE);
	}
	if(cull){
		int32 face = s.cullmode == CULLBACK ? GCM_CULL_BACK : GCM_CULL_FRONT;
		if(hw.cullFace != face){
			hw.cullFace = face;
			rsxSetCullFace(ctx, face);
		}
	}

	int32 stencil = s.stencilenable ? 1 : 0;
	if(hw.stencil != stencil){
		hw.stencil = stencil;
		rsxSetStencilTestEnable(ctx, stencil ? GCM_TRUE : GCM_FALSE);
	}
	if(stencil){
		if(hw.stFunc != (int32)s.stencilfunc || hw.stRef != (int32)s.stencilref || hw.stMask != (int32)s.stencilmask){
			hw.stFunc = s.stencilfunc;
			hw.stRef = s.stencilref;
			hw.stMask = s.stencilmask;
			rsxSetStencilFunc(ctx, stencilFuncMap[s.stencilfunc], s.stencilref, s.stencilmask);
		}
		if(hw.stFail != (int32)s.stencilfail || hw.stZFail != (int32)s.stencilzfail || hw.stPass != (int32)s.stencilpass){
			hw.stFail = s.stencilfail;
			hw.stZFail = s.stencilzfail;
			hw.stPass = s.stencilpass;
			rsxSetStencilOp(ctx, stencilOpMap[s.stencilfail], stencilOpMap[s.stencilzfail], stencilOpMap[s.stencilpass]);
		}
		if(hw.stWriteMask != (int32)s.stencilwritemask){
			hw.stWriteMask = s.stencilwritemask;
			rsxSetStencilMask(ctx, s.stencilwritemask);
		}
	}
}

// ---- samplers -------------------------------------------------------------------

// ---- diagnostics: watched rasters --------------------------------------------------

struct DebugWatch
{
	Raster *raster;
	char tag[24];
	int32 logs;
	uint64 lastUs;
	uint32 crc;
};
static DebugWatch watches[8];
static int32 numWatches;

void
debugWatchRaster(Raster *raster, const char *tag)
{
	if(raster == nil)
		return;
	DebugWatch *w = nil;
	for(int32 i = 0; i < numWatches; i++)
		if(watches[i].raster == raster)
			w = &watches[i];
	if(w == nil){
		if(numWatches >= (int32)nelem(watches))
			return;
		w = &watches[numWatches++];
	}
	w->raster = raster;
	strncpy(w->tag, tag, sizeof(w->tag)-1);
	w->tag[sizeof(w->tag)-1] = '\0';
	w->logs = 0;
	w->lastUs = 0;
	w->crc = rasterCrc(raster);
	rsxLog("[watch] %s: raster %p %dx%d, crc %08x", w->tag, raster, raster->width, raster->height, w->crc);
}

// called after flushCache: the RSX state is the one this draw uses
void
debugIm2DDraw(uint32 prim, const void *verts, int32 numVerts, const void *indices, int32 numIndices)
{
	if(numWatches == 0)
		return;
	Raster *r = rwStateCache.texstage[0].raster;
	if(r == nil)
		return;
	DebugWatch *w = nil;
	for(int32 i = 0; i < numWatches; i++)
		if(watches[i].raster == r || watches[i].raster == r->parent)
			w = &watches[i];
	if(w == nil || w->logs >= 12)
		return;
	uint64 now = sysGetSystemTime();
	if(w->logs > 0 && now - w->lastUs < 4000000ull)
		return;
	w->logs++;
	w->lastUs = now;

	RwStateCache &s = rwStateCache;
	HwSampler &h = hwSampler[0];
	gcmTexture tex;
	bool swz;
	int32 levels;
	rasterBindInfo(r, &tex, &swz, &levels);
	uint32 crc = rasterCrc(r);
	rsxLog("[watch] %s draw %d (frame %u): prim %u, %d verts, %d indices; blend %d (%u,%u) hw %d; atest %u func %u ref %u hw %d; "
	       "ztest %u zwrite %u; cull %u; fog %u; filter %d addr %d/%d; hw sampler %s raster %p levels %d; "
	       "tex %ux%u fmt 0x%x off 0x%x pitch %u mip %u; vram crc %08x (%s)",
	       w->tag, w->logs, currentFrame(), prim, numVerts, numIndices,
	       s.blendEnable, s.srcblend, s.destblend, hw.blend,
	       s.alphaTestEnable, s.alphaFunc, s.alphaRef, hw.alphaTest,
	       s.ztest, s.zwrite, s.cullmode, s.fogEnable,
	       s.texstage[0].filter, s.texstage[0].addressingU, s.texstage[0].addressingV,
	       h.enabled ? "on" : "off", h.raster, h.levels,
	       tex.width, tex.height, tex.format, tex.offset, tex.pitch, tex.mipmap,
	       crc, crc == w->crc ? "unchanged" : "CHANGED");
	const Im2DVertex *v = (const Im2DVertex*)verts;
	const uint16 *idx = (const uint16*)indices;
	int32 n = numIndices > 0 ? numIndices : numVerts;
	if(n > 6) n = 6;
	for(int32 i = 0; i < n; i++){
		const Im2DVertex *p = idx ? &v[idx[i]] : &v[i];
		rsxLog("[watch]   v%d: pos %.2f %.2f %.4f w %.4f  rgba %u %u %u %u  uv %.4f %.4f",
		       idx ? idx[i] : i, p->x, p->y, p->z, p->w, p->r, p->g, p->b, p->a, p->u, p->v);
	}
}

static uint8
minFilter(int32 filter, int32 levels)
{
	if(levels <= 1)
		return (filter == Texture::NEAREST || filter == Texture::MIPNEAREST ||
		        filter == Texture::LINEARMIPNEAREST) ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	switch(filter){
	case Texture::NEAREST: return GCM_TEXTURE_NEAREST;
	case Texture::MIPNEAREST: return GCM_TEXTURE_NEAREST_MIPMAP_NEAREST;
	case Texture::MIPLINEAR: return GCM_TEXTURE_LINEAR_MIPMAP_NEAREST;
	case Texture::LINEARMIPNEAREST: return GCM_TEXTURE_NEAREST_MIPMAP_LINEAR;
	case Texture::LINEARMIPLINEAR: return GCM_TEXTURE_LINEAR_MIPMAP_LINEAR;
	default: return GCM_TEXTURE_LINEAR;
	}
}

static uint8
magFilter(int32 filter)
{
	return (filter == Texture::NEAREST || filter == Texture::MIPNEAREST ||
	        filter == Texture::LINEARMIPNEAREST) ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
}

static uint8
wrapMode(int32 addr, bool swizzled)
{
	// linear textures only filter right clamped
	if(!swizzled)
		return GCM_TEXTURE_CLAMP_TO_EDGE;
	switch(addr){
	case Texture::MIRROR: return GCM_TEXTURE_MIRRORED_REPEAT;
	case Texture::CLAMP: return GCM_TEXTURE_CLAMP_TO_EDGE;
	case Texture::BORDER: return GCM_TEXTURE_BORDER;
	default: return GCM_TEXTURE_REPEAT;
	}
}

// Workarounds under test (patch 16): see selfTest()
static bool invalidateOnBind = true;	// texture cache invalidated whenever a sampler is (re)loaded
static bool resyncEachFrame = true;	// hardware state shadow forgotten at every camera begin

static void
applySamplers(int32 numSamplers)
{
	gcmContextData *ctx = rsxCtx;
	bool invalidated = false;

	if(textureCacheDirty()){
		// the CPU's texture writes must land before the RSX samples them
		__asm__ volatile("sync" ::: "memory");
		rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
		textureCacheClean();
		invalidated = true;
	}

	for(int32 i = 0; i < MAXNUMSTAGES; i++){
		HwSampler &h = hwSampler[i];
		if(i >= numSamplers){
			if(h.enabled){
				rsxTextureControl(ctx, i, GCM_FALSE, 0, 0, 0);
				h.enabled = false;
				h.raster = (Raster*)~(uintptr)0;
			}
			continue;
		}

		RwRasterStateCache &st = rwStateCache.texstage[i];
		Raster *raster = st.raster ? st.raster : whiteRaster();
		if(raster == nil)
			continue;
		gcmTexture tex;
		bool swizzled;
		int32 levels;
		rasterBindInfo(raster, &tex, &swizzled, &levels);
		if(levels == 0){
			// nothing uploaded yet
			raster = whiteRaster();
			if(raster == nil)
				continue;
			rasterBindInfo(raster, &tex, &swizzled, &levels);
			if(levels == 0)
				continue;
		}
		int32 filter = st.raster ? st.filter : Texture::LINEAR;
		int32 addrU = st.raster ? st.addressingU : Texture::WRAP;
		int32 addrV = st.raster ? st.addressingV : Texture::WRAP;

		if(h.enabled && h.raster == raster && h.offset == tex.offset && h.levels == levels &&
		   h.filter == filter && h.addrU == addrU && h.addrV == addrV)
			continue;

		if(invalidateOnBind && !invalidated){
			// as Tiny3D, rsxtest and RSXGL: a new descriptor gets a clean cache
			rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_VERTEX_TEXTURE);
			rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
			invalidated = true;
		}
		rsxLoadTexture(ctx, i, &tex);
		// maxlod is 4.8 fixed point
		rsxTextureControl(ctx, i, GCM_TRUE, 0, (levels-1) << 8, GCM_TEXTURE_MAX_ANISO_1);
		rsxTextureFilter(ctx, i, 0, minFilter(filter, levels), magFilter(filter),
		                 GCM_TEXTURE_CONVOLUTION_QUINCUNX);
		rsxTextureWrapMode(ctx, i, wrapMode(addrU, swizzled), wrapMode(addrV, swizzled),
		                   GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_UNSIGNED_REMAP_NORMAL,
		                   GCM_TEXTURE_ZFUNC_NEVER, 0);
		h.enabled = true;
		h.raster = raster;
		h.offset = tex.offset;
		h.levels = levels;
		h.filter = filter;
		h.addrU = addrU;
		h.addrV = addrV;
		drawStats.texBinds++;
	}
}

// ---- constants --------------------------------------------------------------------

// GL style column-major 4x4: m[c*4+r]
static float32 projMat[16], viewMat[16], viewProjMat[16];
static RawMatrix worldMat;
static bool viewProjDirty = true;
static bool mvpDirty = true;
static bool lightsDirty = true;

struct LightSlot {
	V3d dir;	// world space, as RW has it (the light's 'at')
	RGBAf color;
};
static LightSlot lights[MAX_LIGHTS];
static int32 numLights;
static RGBAf ambLight;

static void
matMul(float32 *out, const float32 *a, const float32 *b)
{
	float32 t[16];
	for(int c = 0; c < 4; c++)
		for(int r = 0; r < 4; r++)
			t[c*4+r] = a[0*4+r]*b[c*4+0] + a[1*4+r]*b[c*4+1] +
			           a[2*4+r]*b[c*4+2] + a[3*4+r]*b[c*4+3];
	memcpy(out, t, sizeof(t));
}

static void
setProjectionMatrix(float32 *mat)
{
	memcpy(projMat, mat, 64);
	viewProjDirty = true;
}

static void
setViewMatrix(float32 *mat)
{
	memcpy(viewMat, mat, 64);
	viewProjDirty = true;
}

void
setWorldMatrix(Matrix *mat)
{
	convMatrix(&worldMat, mat);
	mvpDirty = true;
	lightsDirty = true;
}

void
setWorldIdentity(void)
{
	RawMatrix::setIdentity(&worldMat);
	mvpDirty = true;
	lightsDirty = true;
}

int32
setLights(WorldLights *lightData)
{
	ambLight = lightData->ambient;
	numLights = 0;
	for(int32 i = 0; i < lightData->numDirectionals && numLights < MAX_LIGHTS; i++){
		Light *l = lightData->directionals[i];
		lights[numLights].dir = l->getFrame()->getLTM()->at;
		lights[numLights].color = l->color;
		numLights++;
	}
	// point and spot lights: not used by gl3's default shader either
	lightsDirty = true;
	return 0;
}

void
setMaterial(const RGBA &color, const SurfaceProperties &surfaceprops, float extraSurfProp)
{
	RGBAf col;
	convColor(&col, &color);
	float32 *c = constWant[C_MATCOLOR];
	c[0] = col.red; c[1] = col.green; c[2] = col.blue; c[3] = col.alpha;
	float32 *s = constWant[C_SURFPROPS];
	s[0] = surfaceprops.ambient;
	s[1] = surfaceprops.specular;
	s[2] = surfaceprops.diffuse;
	s[3] = extraSurfProp;
}

static RawMatrix normal2texcoord = {
	{ 0.5f,  0.0f, 0.0f }, 0.0f,
	{ 0.0f, -0.5f, 0.0f }, 0.0f,
	{ 0.0f,  0.0f, 1.0f }, 0.0f,
	{ 0.5f,  0.5f, 0.0f }, 1.0f
};

static Frame *envFrame;
static bool envDirty;

// MatFX env map: as gl3, env uv = texMatrix * (world3x3 * normal), turned
// into one 2x4 matrix on model space normals at flush time
void
setEnvParams(Frame *frame, float coefficient, bool fbAlpha)
{
	envFrame = frame;
	envDirty = true;
	float32 *f = constWant[C_FXPARAMS];
	f[0] = coefficient;
	f[1] = fbAlpha ? 0.0f : 1.0f;
	f[2] = f[3] = 0.0f;
	float32 *cc = constWant[C_COLORCLAMP];
	float32 v = MatFX::modulateEnvMap ? 0.0f : 1.0f;
	cc[0] = cc[1] = cc[2] = cc[3] = v;
}

static void
computeEnvRows(void)
{
	Frame *frame = envFrame ? envFrame : engine->currentCamera->getFrame();
	Matrix invMat;
	RawMatrix invMtx, envMtx;
	Matrix::invert(&invMat, frame->getLTM());
	convMatrix(&invMtx, &invMat);
	invMtx.pos.set(0.0f, 0.0f, 0.0f);
	RawMatrix::mult(&envMtx, &invMtx, &normal2texcoord);

	// column i of world3x3 is worldMat.right/up/at; env (GL column-major)
	// applied to it gives column i of the combined matrix
	V3d *wcol[3] = { &worldMat.right, &worldMat.up, &worldMat.at };
	float32 col[3][2];
	for(int i = 0; i < 3; i++){
		V3d n = *wcol[i];
		col[i][0] = n.x*envMtx.right.x + n.y*envMtx.up.x + n.z*envMtx.at.x;
		col[i][1] = n.x*envMtx.right.y + n.y*envMtx.up.y + n.z*envMtx.at.y;
	}
	float32 *r0 = constWant[C_ENVROW];
	float32 *r1 = constWant[C_ENVROW+1];
	r0[0] = col[0][0]; r0[1] = col[1][0]; r0[2] = col[2][0]; r0[3] = envMtx.pos.x;
	r1[0] = col[0][1]; r1[1] = col[1][1]; r1[2] = col[2][1]; r1[3] = envMtx.pos.y;
}

void
im2DSetXform(void)
{
	Camera *cam = (Camera*)engine->currentCamera;
	float32 *x = constWant[C_XFORM];
	x[0] = 2.0f/cam->frameBuffer->width;
	x[1] = -2.0f/cam->frameBuffer->height;
	x[2] = -1.0f;
	x[3] = 1.0f;
}

void
setBones(const float *rows, int32 numBones)
{
	if(numBones > MAX_SKIN_BONES)
		numBones = MAX_SKIN_BONES;
	if(numBones <= 0 || rsxCtx == nil)
		return;
	// (not rsxSetVertexProgramConstants: PSL1GHT's copies the first half of
	// every block of 8 constants twice)
	rsxLoadVertexProgramParameterBlock(rsxCtx, C_BONES, numBones*3, rows);
	drawStats.constUploads += numBones*3;
}

static void
computeDerivedConstants(bool usesLights, bool usesEnv)
{
	if(viewProjDirty){
		matMul(viewProjMat, projMat, viewMat);
		viewProjDirty = false;
		mvpDirty = true;
	}
	if(mvpDirty){
		float32 m[16];
		matMul(m, viewProjMat, (float32*)&worldMat);
		for(int r = 0; r < 4; r++)
			for(int c = 0; c < 4; c++)
				constWant[C_MVP+r][c] = m[c*4+r];
		mvpDirty = false;
	}
	if(usesLights && lightsDirty){
		float32 *a = constWant[C_AMBLIGHT];
		a[0] = ambLight.red; a[1] = ambLight.green; a[2] = ambLight.blue; a[3] = ambLight.alpha;
		for(int i = 0; i < MAX_LIGHTS; i++){
			float32 *d = constWant[C_LIGHTS + 2*i];
			float32 *c = constWant[C_LIGHTS + 2*i + 1];
			if(i < numLights){
				// dot(world3x3 * n, -dir) == dot(n, -(world3x3^T * dir))
				V3d &l = lights[i].dir;
				d[0] = -(worldMat.right.x*l.x + worldMat.right.y*l.y + worldMat.right.z*l.z);
				d[1] = -(worldMat.up.x*l.x + worldMat.up.y*l.y + worldMat.up.z*l.z);
				d[2] = -(worldMat.at.x*l.x + worldMat.at.y*l.y + worldMat.at.z*l.z);
				d[3] = 0.0f;
				c[0] = lights[i].color.red;
				c[1] = lights[i].color.green;
				c[2] = lights[i].color.blue;
				c[3] = 0.0f;
			}else{
				d[0] = d[1] = d[2] = d[3] = 0.0f;
				c[0] = c[1] = c[2] = c[3] = 0.0f;
			}
		}
		lightsDirty = false;
	}
	if(usesEnv){
		computeEnvRows();
		envDirty = false;
	}

	float32 *f = constWant[C_FOGDATA];
	f[0] = rwStateCache.fogEnd;
	float32 range = rwStateCache.fogStart - rwStateCache.fogEnd;
	f[1] = range != 0.0f ? 1.0f/range : 0.0f;
	f[2] = rwStateCache.fogEnable ? 0.0f : 1.0f;
	f[3] = 0.0f;
	float32 *fc = constWant[C_FOGCOLOR];
	fc[0] = rwStateCache.fogColor.red;
	fc[1] = rwStateCache.fogColor.green;
	fc[2] = rwStateCache.fogColor.blue;
	fc[3] = 1.0f;
}

// uploads registers [first, first+n) if any of them changed
static void
uploadConsts(int32 first, int32 n)
{
	uint32 mask = ((1u << n) - 1) << first;
	if((constValid & mask) == mask &&
	   memcmp(constHave[first], constWant[first], n*16) == 0)
		return;
	memcpy(constHave[first], constWant[first], n*16);
	constValid |= mask;
	rsxLoadVertexProgramParameterBlock(rsxCtx, first, n, constWant[first]);
	drawStats.constUploads += n;
}

static void
applyProgramsAndConstants(void)
{
	gcmContextData *ctx = rsxCtx;

	if(curVP != wantVP){
		VertexProgram *p = &vps[wantVP];
		rsxLoadVertexProgram(ctx, p->vp, p->ucode);
		// the program's own constants (literals) overwrote these
		constValid &= ~p->internalMask;
		curVP = wantVP;
		drawStats.vpSwitches++;
	}
	if(curFP != drawFP){
		FragmentProgram *p = &fps[drawFP];
		rsxLoadFragmentProgramLocation(ctx, p->fp, p->offset, GCM_LOCATION_RSX);
		curFP = drawFP;
		drawStats.fpSwitches++;
	}

	bool usesLights = curVP == VP_DEFAULT || curVP == VP_SKIN || curVP == VP_ENV;
	bool usesEnv = curVP == VP_ENV;
	computeDerivedConstants(usesLights, usesEnv);

	if(curVP != VP_IM2D)
		uploadConsts(C_MVP, 4);
	if(usesLights){
		uploadConsts(C_MATCOLOR, 3);	// matColor, surfProps, ambient
		uploadConsts(C_LIGHTS, 16);
	}
	uploadConsts(C_FOGDATA, 2);
	if(curVP == VP_IM2D)
		uploadConsts(C_XFORM, 1);
	if(usesEnv)
		uploadConsts(C_ENVROW, 4);
}

// The RSX keeps the vertex (and index) data it fetched in a cache, and
// nothing tells it the CPU wrote that memory again: the ring is reused every
// RING_SEGMENTS frames and freed vertex buffers become new ones, so a draw
// could fetch a stale vertex -- the stray triangles of the menus (a green
// wedge from the selection bar, a yellow streak from a letter). The next draw
// after such a write invalidates the cache first.
static bool vtxCacheDirty = true;
void vertexCacheTouched(void) { vtxCacheDirty = true; }

bool
flushCache(void)
{
	if(!targetValid){
		drawStats.dropped++;
		return false;
	}
	if(vtxCacheDirty){
		__asm__ volatile("sync" ::: "memory");
		rsxInvalidateVertexCache(rsxCtx);
		vtxCacheDirty = false;
	}
	applyStates();
	// Additive draws (rain streaks, coronas, headlights...) through a
	// program that kills the fragments that would add nothing: the RSX
	// then skips their blend. The rain streaks are a dozen quads covering
	// the whole screen, mostly black texels: at 720p their blends took
	// ~5 ms a frame (patch 26's test). The picture is the same.
	drawFP = wantFP;
	if(additiveKill && wantFP == FP_SIMPLE && rwStateCache.blendEnable && rwStateCache.destblend == BLENDONE){
		if(rwStateCache.srcblend == BLENDONE)
			drawFP = FP_ADD;
		else if(rwStateCache.srcblend == BLENDSRCALPHA)
			drawFP = FP_ADDALPHA;
	}
	applySamplers(fps[drawFP].numSamplers);
	applyProgramsAndConstants();
	return true;
}

// ---- programs init -------------------------------------------------------------------

bool
programsInit(void)
{
	loadVP(VP_DEFAULT, rw_default_vpo, "default");
	loadVP(VP_SKIN, rw_skin_vpo, "skin");
	loadVP(VP_ENV, rw_env_vpo, "env");
	loadVP(VP_IM2D, rw_im2d_vpo, "im2d");
	loadVP(VP_IM3D, rw_im3d_vpo, "im3d");
	if(!loadFP(FP_SIMPLE, rw_simple_fpo, "simple", 1))
		return false;
	if(!loadFP(FP_ENV, rw_env_fpo, "env", 2))
		return false;
	if(!loadFP(FP_ADD, rw_add_fpo, "add", 1))
		return false;
	if(!loadFP(FP_ADDALPHA, rw_addalpha_fpo, "addalpha", 1))
		return false;
	return true;
}

void
programsShutdown(void)
{
	for(int i = 0; i < NUM_FP; i++)
		if(fps[i].ucode){
			rsxFree(fps[i].ucode);
			fps[i].ucode = nil;
		}
}

// ---- cameras --------------------------------------------------------------------------

static Raster *boundColor, *boundDepth;
static int32 vpX, vpY, vpW, vpH;

// The camera's rasters as the RSX's surface, and its rectangle as viewport
// and scissor. targetValid says whether anything can be drawn.
static void
setFrameBuffer(Camera *cam)
{
	gcmContextData *ctx = rsxCtx;
	Raster *fb = cam->frameBuffer;
	Raster *zb = cam->zBuffer;
	uint32 cOff, cPitch, zOff, zPitch;

	targetValid = false;
	if(ctx == nil || fb == nil || !rasterSurface(fb, &cOff, &cPitch))
		return;
	if(zb == nil || !rasterSurface(zb, &zOff, &zPitch) ||
	   zb->parent->width < fb->parent->width || zb->parent->height < fb->parent->height){
		static int warned;
		if(warned < 3){
			rsxLog("[rsx] WARNING: camera without a usable depth buffer, not drawn");
			warned++;
		}
		return;
	}

	if(boundColor != fb->parent || boundDepth != zb->parent){
		gcmSurface sf;
		memset(&sf, 0, sizeof(sf));
		sf.colorFormat = GCM_SURFACE_A8R8G8B8;
		sf.colorTarget = GCM_SURFACE_TARGET_0;
		sf.colorLocation[0] = GCM_LOCATION_RSX;
		sf.colorOffset[0] = cOff;
		sf.colorPitch[0] = cPitch;
		for(int i = 1; i < 4; i++){
			sf.colorLocation[i] = GCM_LOCATION_RSX;
			sf.colorOffset[i] = cOff;
			sf.colorPitch[i] = 64;
		}
		sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
		sf.depthLocation = GCM_LOCATION_RSX;
		sf.depthOffset = zOff;
		sf.depthPitch = zPitch;
		sf.type = GCM_SURFACE_TYPE_LINEAR;
		sf.antiAlias = GCM_SURFACE_CENTER_1;
		sf.width = fb->parent->width;
		sf.height = fb->parent->height;
		sf.x = 0;
		sf.y = 0;
		rsxSetSurface(ctx, &sf);
		if(zcullActive()){
			int32 want = zcullOn ? 1 : 0;
			rsxSetZCullControl(ctx, GCM_ZCULL_LESS, GCM_ZCULL_LONES);
			rsxSetZCullEnable(ctx, want ? GCM_TRUE : GCM_FALSE, GCM_FALSE);
			if(zcullHave != want){
				// switched (between frames, before the clear that resets it)
				rsxSetZCullInvalidate(ctx);
				zcullHave = want;
			}
		}
		boundColor = fb->parent;
		boundDepth = zb->parent;
		targetW = fb->parent->width;
		targetH = fb->parent->height;
	}

	// the camera's rectangle (a subraster or the whole raster), top left origin
	int32 x = fb->offsetX, y = fb->offsetY, w = fb->width, h = fb->height;
	if(x != vpX || y != vpY || w != vpW || h != vpH){
		float scale[4], offset[4];
		scale[0] = w*0.5f;
		scale[1] = h*-0.5f;
		scale[2] = 0.5f;
		scale[3] = 0.0f;
		offset[0] = x + w*0.5f;
		offset[1] = y + h*0.5f;
		offset[2] = 0.5f;
		offset[3] = 0.0f;
		rsxSetViewport(ctx, x, y, w, h, 0.0f, 1.0f, scale, offset);
		rsxSetViewportClip(ctx, 0, targetW, targetH);
		rsxSetScissor(ctx, x, y, w, h);
		vpX = x; vpY = y; vpW = w; vpH = h;
	}
	targetValid = true;
}

// Forget what the RSX has: everything is sent again (yq2 / IoQuake3 mark
// all state dirty every frame)
static void
forgetHardwareState(void)
{
	memset(&hw, 0xFF, sizeof(hw));
	for(int i = 0; i < MAXNUMSTAGES; i++){
		hwSampler[i].raster = (Raster*)~(uintptr)0;
		hwSampler[i].enabled = true;
	}
	constValid = 0;
	curVP = curFP = -1;
}

static void
beginUpdate(Camera *cam)
{
	float view[16], proj[16];
	if(resyncEachFrame)
		forgetHardwareState();
	// View Matrix
	Matrix inv;
	Matrix::invert(&inv, cam->getFrame()->getLTM());
	// Since we're looking into positive Z,
	// flip X to ge a left handed view space.
	view[0]  = -inv.right.x;
	view[1]  =  inv.right.y;
	view[2]  =  inv.right.z;
	view[3]  =  0.0f;
	view[4]  = -inv.up.x;
	view[5]  =  inv.up.y;
	view[6]  =  inv.up.z;
	view[7]  =  0.0f;
	view[8]  =  -inv.at.x;
	view[9]  =   inv.at.y;
	view[10] =  inv.at.z;
	view[11] =  0.0f;
	view[12] = -inv.pos.x;
	view[13] =  inv.pos.y;
	view[14] =  inv.pos.z;
	view[15] =  1.0f;
	memcpy(&cam->devView, &view, sizeof(RawMatrix));
	setViewMatrix(view);

	// Projection Matrix
	float32 invwx = 1.0f/cam->viewWindow.x;
	float32 invwy = 1.0f/cam->viewWindow.y;
	float32 invz = 1.0f/(cam->farPlane-cam->nearPlane);

	proj[0] = invwx;
	proj[1] = 0.0f;
	proj[2] = 0.0f;
	proj[3] = 0.0f;

	proj[4] = 0.0f;
	proj[5] = invwy;
	proj[6] = 0.0f;
	proj[7] = 0.0f;

	proj[8] = cam->viewOffset.x*invwx;
	proj[9] = cam->viewOffset.y*invwy;
	proj[12] = -proj[8];
	proj[13] = -proj[9];
	if(cam->projection == Camera::PERSPECTIVE){
		proj[10] = (cam->farPlane+cam->nearPlane)*invz;
		proj[11] = 1.0f;

		proj[14] = -2.0f*cam->nearPlane*cam->farPlane*invz;
		proj[15] = 0.0f;
	}else{
		proj[10] = -(cam->farPlane+cam->nearPlane)*invz;
		proj[11] = 0.0f;

		proj[14] = 2.0f*invz;
		proj[15] = 1.0f;
	}
	memcpy(&cam->devProj, &proj, sizeof(RawMatrix));
	setProjectionMatrix(proj);

	rwStateCache.fogStart = cam->fogPlane;
	rwStateCache.fogEnd = cam->farPlane;

	setFrameBuffer(cam);
}

static void
endUpdate(Camera *cam)
{
	(void)cam;
}

static void
clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	gcmContextData *ctx = rsxCtx;
	uint32 mask = 0;

	setFrameBuffer(cam);
	if(!targetValid)
		return;

	if(mode & Camera::CLEARIMAGE){
		rsxSetClearColor(ctx, (uint32)col->alpha<<24 | (uint32)col->red<<16 |
		                      (uint32)col->green<<8 | col->blue);
		mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	}
	if(mode & (Camera::CLEARZ | Camera::CLEARSTENCIL)){
		rsxSetClearDepthStencil(ctx, 0xFFFFFF00);
		if(mode & Camera::CLEARZ)
			mask |= GCM_CLEAR_Z;
		if(mode & Camera::CLEARSTENCIL)
			mask |= GCM_CLEAR_S;
	}
	if(mask)
		rsxClearSurface(ctx, mask);
	if(mask & GCM_CLEAR_Z)
		zcullStale = false;
}

static void
showRaster(Raster *raster, uint32 flags)
{
	videoPresent(raster ? raster->parent : nil, (flags & Raster::FLIPWAITVSYNCH) != 0);
	// the next frame: the surface is bound again
	boundColor = boundDepth = nil;
	vpX = vpY = vpW = vpH = -1;
}

// Copies the camera raster into a camera texture (the trails effect)
static bool32
rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	Raster *src = raster;
	Raster *dst = Raster::getCurrentContext();
	uint32 srcOff, srcPitch, dstOff, dstPitch;

	if(dst == nil || src == nil || rsxCtx == nil)
		return 0;
	if(dst->type != Raster::CAMERATEXTURE || src->type != Raster::CAMERA)
		return 0;
	if(!rasterSurface(src, &srcOff, &srcPitch) || !rasterSurface(dst, &dstOff, &dstPitch))
		return 0;
	int32 w = src->width, h = src->height;
	if(x + w > dst->parent->width) w = dst->parent->width - x;
	if(y + h > dst->parent->height) h = dst->parent->height - y;
	if(w <= 0 || h <= 0)
		return 0;
	rsxSetWaitForIdle(rsxCtx);
	rsxSetTransferImage(rsxCtx, GCM_TRANSFER_LOCAL_TO_LOCAL, dstOff, dstPitch, x, y,
	                    srcOff, srcPitch, src->offsetX, src->offsetY, w, h, 4);
	rsxSetWaitForIdle(rsxCtx);
	textureCacheTouched();
	return 1;
}

uint32
gcmPrimType(PrimitiveType primType)
{
	switch(primType){
	case PRIMTYPELINELIST: return GCM_TYPE_LINES;
	case PRIMTYPEPOLYLINE: return GCM_TYPE_LINE_STRIP;
	case PRIMTYPETRILIST: return GCM_TYPE_TRIANGLES;
	case PRIMTYPETRISTRIP: return GCM_TYPE_TRIANGLE_STRIP;
	case PRIMTYPETRIFAN: return GCM_TYPE_TRIANGLE_FAN;
	case PRIMTYPEPOINTLIST: return GCM_TYPE_POINTS;
	default: return GCM_TYPE_TRIANGLES;
	}
}

// ---- device init ----------------------------------------------------------------

static bool
deviceInit(void)
{
	return videoInit();
}

static bool
deviceFinalize(void)
{
	if(rsxCtx == nil)
		return false;
	if(!programsInit())
		return false;
	resetRenderState();
	initHardwareState();
	rasterInitRSX();
	openImmediate();
	rsxLog("[rsx] renderer ready");
	return true;
}

static void
deviceTerm(void)
{
	closeImmediate();
	rasterShutdownRSX();
	programsShutdown();
	videoShutdown();
}


// ---- self test (patch 16) ------------------------------------------------------
//
// Draws known things into a 64x64 offscreen camera through the normal engine
// paths, reads the result back and logs it: which of texture alpha,
// blending, alpha test, texture cache behave, with and without the
// workarounds. No eyes needed.

static void
stClassify(Raster *fb, const char *tag, const char *name)
{
	uint8 *px = fb->lock(0, Raster::LOCKREAD);
	if(px == nil){
		rsxLog("[selftest %s] %s: lock failed", tag, name);
		return;
	}
	int white = 0, blue = 0, other = 0, n = 64*64;
	for(int i = 0; i < n; i++){
		uint8 *p = px + i*4;
		if(p[0] > 200 && p[1] > 200 && p[2] > 200) white++;
		else if(p[0] < 60 && p[1] < 60 && p[2] > 180) blue++;
		else other++;
	}
	uint8 *a = px, *b = px + (9*64 + 1)*4, *c = px + (32*64 + 32)*4;
	rsxLog("[selftest %s] %-34s white %3d%% blue %3d%% other %3d%% | px(0,0) %02x%02x%02x%02x (1,9) %02x%02x%02x%02x (32,32) %02x%02x%02x%02x",
	       tag, name, white*100/n, blue*100/n, other*100/n,
	       a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], c[0], c[1], c[2], c[3]);
	fb->unlock(0);
}

static Raster*
stChecker(void)
{
	Raster *r = Raster::create(64, 64, 32, Raster::C8888 | Raster::TEXTURE, PLATFORM_PS3);
	if(r == nil)
		return nil;
	uint8 *px = r->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
	if(px){
		for(int y = 0; y < 64; y++)
			for(int x = 0; x < 64; x++){
				uint8 *p = px + (y*64 + x)*4;
				p[0] = p[1] = p[2] = 255;
				p[3] = ((x >> 3) ^ (y >> 3)) & 1 ? 255 : 0;	// 8x8 cells
			}
		r->unlock(0);
	}
	return r;
}

static void
stQuad(Camera *cam, Raster *tex, uint8 alpha, float u0, float v0, float u1, float v1)
{
	Im2DVertex v[4];
	float z = im2d::GetNearZ();
	float rz = 1.0f/cam->nearPlane;
	float X[4] = { 0, 0, 64, 64 }, Y[4] = { 0, 64, 64, 0 };
	float U[4] = { u0, u0, u1, u1 }, V[4] = { v0, v1, v1, v0 };
	for(int i = 0; i < 4; i++){
		v[i].setScreenX(X[i]);
		v[i].setScreenY(Y[i]);
		v[i].setScreenZ(z);
		v[i].setCameraZ(cam->nearPlane);
		v[i].setRecipCameraZ(rz);
		v[i].setColor(255, 255, 255, alpha);
		v[i].setU(U[i], rz);
		v[i].setV(V[i], rz);
	}
	SetRenderStatePtr(TEXTURERASTER, tex);
	im2d::RenderPrimitive(PRIMTYPETRIFAN, v, 4);
}

static void
stBegin(Camera *cam)
{
	RGBA blue = { 0, 0, 255, 255 };
	cam->beginUpdate();
	cam->clear(&blue, Camera::CLEARIMAGE | Camera::CLEARZ);
	SetRenderState(FOGENABLE, 0);
	SetRenderState(CULLMODE, CULLNONE);
	SetRenderState(ZTESTENABLE, 0);
	SetRenderState(ZWRITEENABLE, 0);
	SetRenderState(SRCBLEND, BLENDSRCALPHA);
	SetRenderState(DESTBLEND, BLENDINVSRCALPHA);
	SetRenderState(ALPHATESTFUNC, ALPHAGREATEREQUAL);
	SetRenderState(ALPHATESTREF, 3);
	SetRenderState(VERTEXALPHA, 1);
	SetRenderState(TEXTUREFILTER, Texture::NEAREST);
	SetRenderState(TEXTUREADDRESS, Texture::CLAMP);
}

void
selfTest(const char *tag, Raster *font)
{
	if(rsxCtx == nil)
		return;
	Camera *prev = (Camera*)engine->currentCamera;
	Frame *frm = Frame::create();
	Camera *cam = Camera::create();
	if(frm == nil || cam == nil){
		rsxLog("[selftest %s] no camera", tag);
		return;
	}
	cam->setFrame(frm);
	cam->setNearPlane(0.9f);
	cam->setFarPlane(1000.0f);
	cam->frameBuffer = Raster::create(64, 64, 32, Raster::CAMERA, PLATFORM_PS3);
	cam->zBuffer = Raster::create(64, 64, 32, Raster::ZBUFFER, PLATFORM_PS3);
	if(cam->frameBuffer == nil || cam->zBuffer == nil){
		rsxLog("[selftest %s] no target", tag);
		return;
	}
	bool saveInv = invalidateOnBind, saveResync = resyncEachFrame;
	// font glyph 'A' (code 33): column 1, row 2 of 16 x 12.8 cells
	float gu0 = 1.0f/16.0f, gv0 = 2.0f/12.8f, gu1 = 2.0f/16.0f, gv1 = 3.0f/12.8f;

	for(int round = 0; round < 2; round++){
		invalidateOnBind = round == 1;
		resyncEachFrame = round == 1;
		char t[32];
		snprintf(t, sizeof(t), "%s/%s", tag, round ? "fixes on" : "fixes off");

		stBegin(cam); stQuad(cam, nil, 128, 0, 0, 1, 1); cam->endUpdate();
		stClassify(cam->frameBuffer, t, "1 untextured, vertex alpha 128");

		Raster *chk = stChecker();
		stBegin(cam); stQuad(cam, chk, 255, 0, 0, 1, 1); cam->endUpdate();
		stClassify(cam->frameBuffer, t, "2 checker just uploaded");

		stBegin(cam); stQuad(cam, chk, 255, 0, 0, 1, 1); cam->endUpdate();
		stClassify(cam->frameBuffer, t, "3 checker again");

		stBegin(cam); stQuad(cam, nil, 255, 0, 0, 1, 1); stQuad(cam, chk, 255, 0, 0, 1, 1); cam->endUpdate();
		stClassify(cam->frameBuffer, t, "4 white quad, then checker");

		stBegin(cam);
		SetRenderState(SRCBLEND, BLENDONE); SetRenderState(DESTBLEND, BLENDZERO);
		SetRenderState(ALPHATESTREF, 128);
		stQuad(cam, chk, 255, 0, 0, 1, 1); cam->endUpdate();
		stClassify(cam->frameBuffer, t, "5 checker, no blend, alpha test");

		stBegin(cam);
		SetRenderState(ZTESTENABLE, 1); SetRenderState(ZWRITEENABLE, 1);
		stQuad(cam, chk, 255, 0, 0, 1, 1); stQuad(cam, chk, 255, 0, 0, 1, 1); cam->endUpdate();
		stClassify(cam->frameBuffer, t, "6 checker twice, z test+write");

		if(font){
			stBegin(cam); stQuad(cam, font, 255, gu0, gv0, gu1, gv1); cam->endUpdate();
			stClassify(cam->frameBuffer, t, "7 font glyph A");
			stBegin(cam); stQuad(cam, font, 255, 0, 0, 1, 1); cam->endUpdate();
			stClassify(cam->frameBuffer, t, "8 font whole");
		}
		chk->destroy();
	}
	invalidateOnBind = saveInv;
	resyncEachFrame = saveResync;

	cam->frameBuffer->destroy();
	cam->zBuffer->destroy();
	cam->frameBuffer = nil;
	cam->zBuffer = nil;
	cam->setFrame(nil);
	cam->destroy();
	frm->destroy();
	engine->currentCamera = prev;
	forgetHardwareState();
}

#endif	// RW_PS3_RSX

// ---- device system -----------------------------------------------------------------------

static int
deviceSystem(DeviceReq req, void *arg, int32 n)
{
	VideoMode *rwmode;

	switch(req){
	case DEVICEOPEN:
#ifndef RW_PS3_RSX
		resetRenderStates();
#endif
		return 1;
	case DEVICECLOSE:
		return 1;

	case DEVICEINIT:
		return deviceInit();
	case DEVICETERM:
		deviceTerm();
		return 1;
	case DEVICEFINALIZE:
		return deviceFinalize();

	case DEVICEGETNUMSUBSYSTEMS:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return n == 0;
	case DEVICEGETSUBSSYSTEMINFO:
		if(n != 0)
			return 0;
		strncpy(((SubSystemInfo*)arg)->name, "RSX", sizeof(SubSystemInfo::name));
		return 1;

	// the internal resolutions (the RSX scales the picture to the TV)
	case DEVICEGETNUMVIDEOMODES:
		return getNumVideoModes();
	case DEVICEGETCURRENTVIDEOMODE:
		return currentMode;
	case DEVICESETVIDEOMODE:
		if(n < 0 || n >= getNumVideoModes())
			return 0;
		currentMode = n;
		return 1;
	case DEVICEGETVIDEOMODEINFO:
		if(n < 0 || n >= getNumVideoModes())
			return 0;
		rwmode = (VideoMode*)arg;
		getVideoModeSize(n, &rwmode->width, &rwmode->height);
		rwmode->depth = 32;
		rwmode->flags = VIDEOMODEEXCLUSIVE;
		return 1;

	case DEVICEGETMAXMULTISAMPLINGLEVELS:
	case DEVICEGETMULTISAMPLINGLEVELS:
		return 1;
	case DEVICESETMULTISAMPLINGLEVELS:
		return 1;

	default:
		break;
	}
	return 1;
}

Device renderdevice = {
	-1.0f, 1.0f,
	beginUpdate,
	endUpdate,
	clearCamera,
	showRaster,
	rasterRenderFast,
	setRenderState,
	getRenderState,
	im2DRenderLine,
	im2DRenderTriangle,
	im2DRenderPrimitive,
	im2DRenderIndexedPrimitive,
	im3DTransform,
	im3DRenderPrimitive,
	im3DRenderIndexedPrimitive,
	im3DEnd,
	deviceSystem
};

// ---- driver --------------------------------------------------------------------------------

static void*
driverOpen(void *o, int32, int32)
{
#ifdef RW_PS3_RSX
	initPipelines();
#endif
	engine->driver[PLATFORM_PS3]->rasterNativeOffset = nativeRasterOffset;
	engine->driver[PLATFORM_PS3]->rasterCreate       = rasterCreate;
	engine->driver[PLATFORM_PS3]->rasterLock         = rasterLock;
	engine->driver[PLATFORM_PS3]->rasterUnlock       = rasterUnlock;
	engine->driver[PLATFORM_PS3]->rasterNumLevels    = rasterNumLevels;
	engine->driver[PLATFORM_PS3]->imageFindRasterFormat = imageFindRasterFormat;
	engine->driver[PLATFORM_PS3]->rasterFromImage    = rasterFromImage;
	engine->driver[PLATFORM_PS3]->rasterToImage      = rasterToImage;
	return o;
}

static void*
driverClose(void *o, int32, int32)
{
	return o;
}

void
registerPlatformPlugins(void)
{
	Driver::registerPlugin(PLATFORM_PS3, 0, PLATFORM_PS3,
	                       driverOpen, driverClose);
	registerNativeRaster();
}

}
}
