#pragma once
// librw backend for the PS3 (RSX through PSL1GHT's GCM, no GL).
//
// Rasters live in main memory in a layout the RSX can read after a copy:
//   - uncompressed textures: 32 bit, bytes R,G,B,A (like gl3's RGBA8888)
//   - DXT1/3/5: the blocks exactly as they come from the PC TXDs
// Palettized and 16/24 bit images are expanded to 32 bit on the way in.

namespace rw {

#ifdef RW_PS3
struct EngineOpenParams
{
	int width, height;
	const char *windowtitle;
};
#endif

namespace ps3 {

void registerPlatformPlugins(void);

extern Device renderdevice;

// pipelines (stage 2; empty in stage 1)
void initSkin(void);
void initMatFX(void);
void *destroyNativeData(void *object, int32, int32);

// Same vertex layouts as gl3, so the game's Im2D/Im3D code needs no changes.
struct Im3DVertex
{
	V3d     position;
	uint8   r, g, b, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

struct Im2DVertex
{
	float32 x, y, z, w;
	uint8   r, g, b, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { this->u = u; }
	void setV(float32 v, float recipz) { this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

// Native raster

enum Ps3TexFormat
{
	PS3_TEX_NONE = 0,
	PS3_TEX_RGBA8,		// 32 bit, bytes R G B A
	PS3_TEX_DXT1,
	PS3_TEX_DXT3,
	PS3_TEX_DXT5
};

struct Ps3Raster
{
	// stage 1 (headless): all levels in main memory
	uint8  *texels;
	uint32  texelSize;
	uint32  levelOffset[16];	// of each level, in texels or in VRAM
	uint8   texFormat;	// Ps3TexFormat (of the data the game locks)
	uint8   bpp;		// bytes per pixel (uncompressed only)
	int8    numLevels;
	bool    isCompressed;
	bool    hasAlpha;
	bool    autogenMipmap;
	uint8   filterMode;
	uint8   addressU;
	uint8   addressV;
	uint32  serial;		// bumped when the texels change

	// stage 2 (RSX): A8R8G8B8 in VRAM, swizzled (power of two sizes, all
	// levels back to back) or linear (anything else, render targets)
	uint8  *vram;		// CPU address: write only
	uint32  vramOffset;	// RSX offset
	uint32  vramSize;
	void   *vramBlock;
	uint32  pitch;		// linear layout only
	uint16  validLevels;	// bit n: level n has been uploaded
	bool    swizzled;
	int8    lockedLevel;
	uint8  *staging;	// the locked level, in main memory
	int8    tile;		// camera / depth buffer: tile region + 1 (0 linear)
	bool    zcomp, zcull;
};

// Video settings of the RSX renderer (stage 2)
struct VideoSettings
{
	int32 screenFit;	// % of the TV used by the picture (overscan), 50..100
	int32 bilinear;		// scaling filter to the TV
	int32 reserved;
};
void setVideoSettings(const VideoSettings *s);
void setPresentAspect(float aspect);	// 0: the picture's own shape
// RSX timers (stage 2): marks 2*i / 2*i+1 time the GPU work between them
// (timer i), read later; gpuStatsTake hands the sums over and resets them
enum { GPU_MAX_TIMERS = 30, GPU_MARK_FRAME_START = 62, GPU_MARK_FRAME_END = 63 };
void gpuMark(int32 id);
extern bool gpuTimersOn;	// default off (the timestamp reports hang the RSX)
extern bool additiveKill;	// additive draws kill the fragments that add nothing (default on)
extern bool rsxTiling;		// camera buffers tiled + depth compressed + ZCULL (default on; set before the first camera)
extern bool zcullOn;		// ZCULL in use (default on; the autotest compares)
extern int32 blendAlphaRef;	// >0: alpha test ref for alpha blended draws without Z write (smoke...); 0 the game's
void setFlipVsync(bool on);	// false: flips on hsync, no 60 fps cap (benchmarks; tears)
void setFrameRate(int32 fps);	// 30: every picture stays two vblanks (the game's own rate); 60: one
void gpuStatsTake(uint64 *ns, uint64 *frameNs, uint32 *samples);
void getVideoSettings(VideoSettings *s);

// The internal resolutions offered as video modes (the RSX scales the
// picture to the TV)
int32 getNumVideoModes(void);
void getVideoModeSize(int32 mode, int32 *width, int32 *height);

void allocateDXT(Raster *raster, int32 dxt, int32 numLevels, bool32 hasAlpha);

#ifdef RW_PS3_RSX
// logs a texture as it is in VRAM (size, format, levels, alpha/colour stats)
void debugRaster(Raster *raster, const char *tag);
// logs a few of the Im2D draws that sample this raster: vertices, render
// states, sampler, and whether its VRAM changed since this call
void debugWatchRaster(Raster *raster, const char *tag);
// draws known patterns offscreen, reads them back, logs what the RSX did
void selfTest(const char *tag, Raster *font);
// VRAM almost full (what is waiting to be freed counts as free):
// the game's streaming throws out unused models while this is true
bool vramTight(void);
extern uint32 vramEvictions;
// d3d8.cpp: an 8/4 bit palettized PC texture straight into VRAM. false: not
// handled (the stream is untouched); true: read, *out is the raster or nil
bool readPalettedD3D8(Stream *stream, int32 width, int32 height, int32 format, int32 numLevels, Raster **out);	// models the streaming threw out for VRAM
#endif

Texture *readNativeTexture(Stream *stream);
void writeNativeTexture(Texture *tex, Stream *stream);
uint32 getSizeNativeTexture(Texture *tex);

extern int32 nativeRasterOffset;
void registerNativeRaster(void);
#define GETPS3RASTEREXT(raster) PLUGINOFFSET(rw::ps3::Ps3Raster, raster, rw::ps3::nativeRasterOffset)

// Statistics for the log (stage 1 and the memory lines of the game)
struct RasterStats
{
	int32  numTextures;
	uint32 textureBytes;
	int32  numDXT;
	int32  numExpanded;	// palettized / 16 / 24 bit turned into RGBA8
	int32  noVram;		// textures left without storage: VRAM was full
};
extern RasterStats rasterStats;

// Where the time of turning a PC texture into a PS3 one goes (microseconds,
// reset every 10 s by the [tex] log line)
enum {
	TEXPROF_CONVERT,	// librw: D3D raster -> Image (palette expanded)
	TEXPROF_FROMIMAGE,	// Image -> RGBA8 staging
	TEXPROF_SWIZZLE,	// RGBA8 -> A8R8G8B8 swizzled, in main memory
	TEXPROF_VRAM,		// copy into VRAM
	TEXPROF_MIPS,		// mip levels made on the CPU
	TEXPROF_READBACK,	// VRAM read back by the CPU (slow)
	TEXPROF_INDEXED,	// palettized D3D8 textures, fast path, all of it
	TEXPROF_NUM
};
struct TexProfile {
	uint64 us[TEXPROF_NUM];
	uint32 textures;	// converted textures
	uint32 levels;		// levels uploaded
	uint64 texels;		// texels uploaded
	uint32 readbacks;
	uint32 indexed;		// textures that took the palettized fast path
	// the game's streaming: whole conversions (CStreaming::ConvertBufferToObject
	// and FinishLoadingLargeFile), what the frame actually waits for
	uint64 txdUs, modelUs, otherUs, worstUs;
	uint32 txds, models, others;
	// models: the DFF parse (librw) vs the game's setup after it, and the
	// instancing into VRAM, which happens at the first draw (in the frame)
	uint64 dffParseUs, dffSetupUs, instUs, instWorstUs;
	uint32 instCount;
	char worstName[24];	// the slowest object of the period
	// textures a model wants and its TXD doesn't have: looked for as image
	// files (once per name now, see imageMissKnown)
	uint32 imgLookups;
	uint64 imgLookupUs;
};
// names of textures that aren't image files either (texture.cpp)
bool imageMissKnown(const char *name, const char *mask);
void imageMissRemember(const char *name, const char *mask);
extern TexProfile texProfile;
uint64 texProfNow(void);	// microseconds

}
}
