// ps3video.cpp -- RSX and video output for the RSX renderer (stage 2).
//
// From the PS3 ports proven on hardware (Quake2PS3 / Doom64-PS3 /
// TyrQuakeCell): rsxInit once, 720p if the TV takes it, three display
// buffers flipped on vsync, the picture scaled to the TV by the transfer
// unit (rsxSetTransferScaleSurface) with a screen fit area, flip sequence
// flip / flush / wait-flip.
//
// The game renders into the camera raster (any size: the internal
// resolution); videoPresent scales it into the next display buffer.
//
// Rules learned the hard way (handoff notes of those ports):
//  - rsxInit only once; on shutdown unhook the flip handler FIRST.
//  - never read RSX memory from the CPU.
//  - rsxFinish needs a new reference value every call.
//  - the 3D unit and the transfer unit run on their own: a wait-for-idle
//    between drawing an image and scaling it (and before drawing into it
//    again).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwengine.h"
#include "rwps3.h"

#ifdef RW_PS3_RSX
#include "ps3rsx.h"

#include <sys/systime.h>
#include <sysutil/video.h>

namespace rw {
namespace ps3 {

#define FB_COUNT	3
#define RSX_CB_SIZE	(2 * 1024 * 1024)	// command buffer
#define RSX_IO_SIZE	(16 * 1024 * 1024)	// IO mapped main memory (holds the CB)

#define LABEL_FRAME	70	// GCM labels 0-63 are the system's

#define RING_SEGMENTS	3
#define RING_SEG_SIZE	(4 * 1024 * 1024)

gcmContextData *rsxCtx;
DrawStats drawStats;

static void *ioBuffer;
static bool ready;

static u32 dispW = 1280, dispH = 720, dispPitch;
static u32 *fb[FB_COUNT];
static u32 fbOffset[FB_COUNT];
static int cur;
static int clearFrames;

static volatile u32 flipQueued, flipCompleted;
static volatile u32 vblankCount;
static volatile u64 lastFlipUs;		// when the last flip happened (flip handler)
static volatile u64 lastVblankUs;
static volatile u32 vblankPeriodUs = 16683;	// measured (60 Hz; 50 Hz TVs: 20000)
static int32 frameRate = 60;
static bool flipsOnVsync = true;

static volatile u32 *frameLabel;
static uint32 frame;
static void gpuCollect(uint32 doneFrame);

static VramAlloc ring;
static uint32 ringSeg, ringHead, ringPeak;

static VideoSettings settings = { 90, 1, 0 };
static float presentAspect;
static float dispAspect = 16.0f/9.0f;	// 0: the picture's own (its size); else w/h on the TV

// ---- log --------------------------------------------------------------------

void
rsxLog(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf)-1, fmt, ap);
	va_end(ap);
	strcat(buf, "\n");
	printf("%s", buf);	// goes to the log (see the game's ps3_log.cpp)
}

// ---- frames -----------------------------------------------------------------

uint32
currentFrame(void)
{
	return frame;
}

uint32
completedFrame(void)
{
	return frameLabel ? *frameLabel : 0;
}

void
waitFrame(uint32 f)
{
	int waited = 0;
	while((int32)(completedFrame() - f) < 0){
		usleep(20);
		if(++waited > 250000){
			rsxLog("[rsx] WARNING: frame fence timed out (want %u, RSX at %u)", f, completedFrame());
			break;
		}
	}
}

void
rsxFinishAll(void)
{
	static u32 ref = 0x1000;
	if(rsxCtx == nil)
		return;
	if(++ref == 0 || ref == 0xFFFFFFFF)
		ref = 0x1000;
	rsxFinish(rsxCtx, ref);
}

static void
flipHandler(const u32 head)
{
	(void)head;
	lastFlipUs = sysGetSystemTime();
	flipCompleted++;
}

static void
vblankHandler(const u32 head)
{
	(void)head;
	u64 now = sysGetSystemTime();
	if(lastVblankUs){
		u64 dt = now - lastVblankUs;
		if(dt > 12000 && dt < 25000)	// a sane period: smoothed
			vblankPeriodUs = (vblankPeriodUs*7 + (u32)dt) / 8;
	}
	lastVblankUs = now;
	vblankCount++;
}

// 30 fps: each picture stays two vblanks. The frame's drawing is sent to
// the RSX first (it draws while the CPU waits), then the flip is queued
// right after the vblank before its target, so it happens on the target:
// two vblanks after the last one (or the next vblank, if the frame is late).
// Patch 30 waited from the flip handler's time with the frame still in the
// command buffer: the RSX only started drawing after the wait and missed
// the vblank, 15 fps.
static u32 flipTargetVblank;

static void
paceFlip(void)
{
	if(frameRate >= 60 || !flipsOnVsync)
		return;
	rsxFlushBuffer(rsxCtx);
	u32 now = vblankCount;
	u32 target = flipTargetVblank + 2;
	if((int32)(target - now) < 1)
		target = now + 1;	// late (or the first frame): the next vblank
	// at most ~50 ms (no vblank handler: no pacing rather than a hang)
	for(int waited = 0; (int32)(vblankCount - (target - 1)) < 0 && waited < 250; waited++)
		usleep(200);
	flipTargetVblank = target;
}

void
setFrameRate(int32 fps)
{
	fps = fps <= 30 ? 30 : 60;
	if(fps != frameRate)
		rsxLog("[rsx] frame rate %d", fps);
	frameRate = fps;
}

// Blocks while both display buffers that aren't being built are in flight.
static void
waitFlips(void)
{
	int waited = 0;
	while((int)(flipQueued - flipCompleted) > FB_COUNT - 2){
		usleep(100);
		if(++waited > 20000){
			rsxLog("[rsx] WARNING: flip fence timed out, force-sync");
			flipCompleted = flipQueued;
			break;
		}
	}
}

// ---- settings ---------------------------------------------------------------

// The game's aspect ratio option: the picture was drawn for this shape, so
// it is shown with this shape (black bars) instead of filling a 16:9 TV
void
setPresentAspect(float aspect)
{
	if(aspect < 0.5f || aspect > 4.0f)
		aspect = 0.0f;
	if(aspect != presentAspect)
		clearFrames = FB_COUNT;
	presentAspect = aspect;
}

// Benchmarks: flips on hsync, so the frame rate is what the CPU and the RSX
// give (above 60, with tearing) instead of the vsync's cap
void
setFlipVsync(bool on)
{
	static int have = 1;
	if(!ready || have == (on ? 1 : 0))
		return;
	have = on ? 1 : 0;
	flipsOnVsync = on;
	// no flip queued while the mode changes
	rsxFinishAll();
	for(int waited = 0; flipQueued != flipCompleted && waited < 1000; waited++)
		usleep(100);
	gcmSetFlipMode(on ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);
	rsxLog("[rsx] flips on %s", on ? "vsync" : "hsync (no cap)");
}

void
setVideoSettings(const VideoSettings *s)
{
	VideoSettings n = *s;
	if(n.screenFit < 50) n.screenFit = 50;
	if(n.screenFit > 100) n.screenFit = 100;
	if(n.screenFit != settings.screenFit)
		clearFrames = FB_COUNT;
	settings = n;
}

void
getVideoSettings(VideoSettings *s)
{
	*s = settings;
}

int
videoWidth(void)
{
	return dispW;
}

int
videoHeight(void)
{
	return dispH;
}

// ---- init -------------------------------------------------------------------

bool
videoInit(void)
{
	s32 ret;
	s32 vidRes = VIDEO_RESOLUTION_720;
	u8 vidAspect = VIDEO_ASPECT_16_9;
	videoResolution res;
	videoConfiguration vconfig;
	videoState state;
	int i, waited;

	if(ready)
		return true;

	rsxLog("[rsx] init");
	ioBuffer = memalign(1024*1024, RSX_IO_SIZE);
	if(ioBuffer == nil){
		rsxLog("[rsx] FATAL: memalign failed for the %d MB IO buffer", RSX_IO_SIZE>>20);
		return false;
	}
	ret = rsxInit(&rsxCtx, RSX_CB_SIZE, RSX_IO_SIZE, ioBuffer);
	if(ret != 0 || rsxCtx == nil){
		rsxLog("[rsx] FATAL: rsxInit failed (%d)", (int)ret);
		return false;
	}

	// 720p if the TV takes it, otherwise whatever it is set to
	if(!videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9, 0)){
		videoGetState(0, 0, &state);
		vidRes = state.displayMode.resolution;
		vidAspect = state.displayMode.aspect;
		rsxLog("[rsx] 720p not available, using the TV's mode (%d)", (int)vidRes);
	}
	if(videoGetResolution(vidRes, &res) != 0){
		rsxLog("[rsx] FATAL: videoGetResolution failed");
		return false;
	}
	dispW = res.width;
	dispH = res.height;
	dispPitch = dispW*4;
	// shape of the TV picture (720x480 can be either)
	dispAspect = vidAspect == VIDEO_ASPECT_4_3 ? 4.0f/3.0f :
	             vidAspect == VIDEO_ASPECT_16_9 ? 16.0f/9.0f : (float)dispW / (float)dispH;

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = vidRes;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = dispPitch;
	vconfig.aspect = vidAspect;
	if(videoConfigure(0, &vconfig, NULL, 0) != 0){
		rsxLog("[rsx] FATAL: videoConfigure failed");
		return false;
	}
	waited = 0;
	do{
		usleep(10000);
		if(videoGetState(0, 0, &state) != 0)
			break;
	}while(state.state == 3 && ++waited < 300);

	gcmSetFlipMode(GCM_FLIP_VSYNC);

	for(i = 0; i < FB_COUNT; i++){
		fb[i] = (u32*)rsxMemalign(64, dispPitch*dispH);
		if(fb[i] == nil){
			rsxLog("[rsx] FATAL: rsxMemalign failed for display buffer %d", i);
			return false;
		}
		memset(fb[i], 0, dispPitch*dispH);
		vramFlush(fb[i], dispPitch*dispH);
		rsxAddressToOffset(fb[i], &fbOffset[i]);
		gcmSetDisplayBuffer(i, fbOffset[i], dispPitch, dispW, dispH);
	}
	cur = 0;
	flipQueued = flipCompleted = 0;
	clearFrames = FB_COUNT;
	gcmSetFlipHandler(flipHandler);
	gcmSetVBlankHandler(vblankHandler);

	frameLabel = (volatile u32*)gcmGetLabelAddress(LABEL_FRAME);
	*frameLabel = 0;
	frame = 1;

	if(!vramInit())
		return false;
	if(!vramAlloc(&ring, RING_SEGMENTS*RING_SEG_SIZE, "vertex ring")){
		rsxLog("[rsx] FATAL: no VRAM for the vertex ring");
		return false;
	}
	ringSeg = frame % RING_SEGMENTS;
	ringHead = 0;

	ready = true;
	rsxLog("[rsx] ready: TV %ux%u, %d display buffers, ring %d x %d MB",
	       dispW, dispH, FB_COUNT, RING_SEGMENTS, RING_SEG_SIZE>>20);
	return true;
}

void
videoShutdown(void)
{
	int i;
	if(!ready)
		return;
	rsxLog("[rsx] shutdown");
	// ORDER MATTERS: unhook the handlers first
	gcmSetFlipHandler(NULL);
	gcmSetVBlankHandler(NULL);
	rsxFinishAll();
	vramShutdown();
	for(i = 0; i < FB_COUNT; i++)
		if(fb[i]){
			rsxFree(fb[i]);
			fb[i] = nil;
		}
	ready = false;
}

// ---- ring -------------------------------------------------------------------

uint32
ringWrite(const void *data, uint32 size)
{
	if(!ready || ring.ptr == nil)
		return 0xFFFFFFFF;
	if(ringHead + size > RING_SEG_SIZE){
		static int warned;
		if(!warned){
			rsxLog("[rsx] WARNING: vertex ring segment full (%u + %u bytes), draws dropped",
			       ringHead, size);
			warned = 1;
		}
		drawStats.dropped++;
		return 0xFFFFFFFF;
	}
	uint32 at = ringSeg*RING_SEG_SIZE + ringHead;
	memcpy(ring.ptr + at, data, size);
	vramFlush(ring.ptr + at, size);
	vertexCacheTouched();
	// own cache lines (128 bytes): a draw's fetch never holds the start of
	// the next one's data from before it was written
	ringHead = (ringHead + size + 127) & ~127;
	drawStats.ringBytes += size;
	return ring.offset + at;
}

// ---- frames -----------------------------------------------------------------

void
videoBeginFrame(void)
{
	// the ring segment of this frame was last used RING_SEGMENTS frames ago
	if(frame > RING_SEGMENTS){
		waitFrame(frame - RING_SEGMENTS);
		gpuCollect(frame - RING_SEGMENTS);	// done: its timestamps are there
	}
	ringSeg = frame % RING_SEGMENTS;
	if(ringHead > ringPeak)
		ringPeak = ringHead;
	ringHead = 0;
	vramProcessFrees();
	gpuMark(GPU_MARK_FRAME_START);
}

// ---- GPU timers ---------------------------------------------------------------
// RSX timestamps (report slots) around main.cpp's frame phases: how long the
// RSX itself spends on the scene, the effects, the 2D... (the CPU timers only
// say how long the PPU took to send it). Read RING_SEGMENTS frames later,
// when the frame is surely done.

#define TS_BASE		1024	// report slots 1024..1279 (nothing else uses them)
#define TS_PER_FRAME	64
#define TS_FRAMES	4
static uint64 tsMask[TS_FRAMES];
bool gpuTimersOn;	// off unless the autotest turns them on (see below)
static uint64 gpuNs[GPU_MAX_TIMERS];
static uint64 gpuFrameNs;
static uint32 gpuSamples;

void
gpuMark(int32 id)
{
	if(!ready || !gpuTimersOn || id < 0 || id >= TS_PER_FRAME)
		return;
	uint32 f = frame % TS_FRAMES;
	// NOT rsxSetTimeStamp: PSL1GHT's writes its data word one slot too far
	// (CURRENTP[2]), the RSX reads garbage and hangs (patch 25's black
	// screen). A report of type 0x10 is the same command, written right.
	rsxSetReport(rsxCtx, 0x10, TS_BASE + f*TS_PER_FRAME + id);
	tsMask[f] |= 1ull << id;
}

static void
gpuCollect(uint32 doneFrame)
{
	uint32 f = doneFrame % TS_FRAMES;
	uint64 m = tsMask[f];
	tsMask[f] = 0;
	if(m == 0)
		return;
	uint32 base = TS_BASE + f*TS_PER_FRAME;
	for(int32 i = 0; i < GPU_MAX_TIMERS; i++)
		if(((m >> (2*i)) & 3) == 3){
			uint64 a = gcmGetTimeStamp(base + 2*i), b = gcmGetTimeStamp(base + 2*i + 1);
			if(b > a && b - a < 1000000000ull)
				gpuNs[i] += b - a;
		}
	if(((m >> GPU_MARK_FRAME_START) & 1) && ((m >> GPU_MARK_FRAME_END) & 1)){
		uint64 a = gcmGetTimeStamp(base + GPU_MARK_FRAME_START), b = gcmGetTimeStamp(base + GPU_MARK_FRAME_END);
		if(b > a && b - a < 1000000000ull){
			gpuFrameNs += b - a;
			gpuSamples++;
		}
	}
}

void
gpuStatsTake(uint64 *ns, uint64 *frameNs, uint32 *samples)
{
	for(int32 i = 0; i < GPU_MAX_TIMERS; i++){
		ns[i] = gpuNs[i];
		gpuNs[i] = 0;
	}
	*frameNs = gpuFrameNs;
	*samples = gpuSamples;
	gpuFrameNs = 0;
	gpuSamples = 0;
}

// ---- present ----------------------------------------------------------------

static u64 statStart;
static u32 statFrames;
static u64 statWaitUs, statCpuUs;
static u64 frameStartUs;
static u64 lastPresentUs, worstFrameUs;
static u32 slowFrames, verySlowFrames;	// over 20 ms (a missed vsync), over 50 ms (a hitch)

static void
logStats(void)
{
	u64 now = sysGetSystemTime();
	if(lastPresentUs){
		u64 dt = now - lastPresentUs;
		if(dt > worstFrameUs) worstFrameUs = dt;
		if(dt > 20000) slowFrames++;
		if(dt > 50000) verySlowFrames++;
	}
	lastPresentUs = now;
	if(statStart == 0){
		statStart = now;
		return;
	}
	statFrames++;
	if(now - statStart < 10000000ull)
		return;
	double ms = (now - statStart) / 1000.0 / statFrames;
	uint32 used, total, blocks;
	vramStats(&used, &total, &blocks);
	DrawStats &s = drawStats;
	uint32 n = statFrames;
	rsxLog("[rsx] %.1f fps: frame %.1f ms (CPU %.1f, waiting for the RSX %.1f); per frame %u draws, "
	       "%u verts, %u indices, VP %u FP %u tex %u, %u consts, ring %u KB",
	       1000.0/ms, ms, statCpuUs/1000.0/n, statWaitUs/1000.0/n,
	       s.draws/n, s.verts/n, s.indices/n, s.vpSwitches/n, s.fpSwitches/n, s.texBinds/n,
	       s.constUploads/n, s.ringBytes/1024/n);
	rsxLog("[rsx]   VRAM %u/%u MB in %u blocks; textures uploaded %u KB/s; instanced %u; dropped %u; ring peak %u KB; "
	       "streaming evictions %u; textures without VRAM %d",
	       used>>20, total>>20, blocks, (uint32)(s.texUploadBytes/1024/10), s.instanced, s.dropped, ringPeak>>10,
	       vramEvictions, rasterStats.noVram);
	rsxLog("[rsx]   frames over 20 ms: %u, over 50 ms: %u, worst %.1f ms",
	       slowFrames, verySlowFrames, worstFrameUs/1000.0);
	TexProfile &t = texProfile;
	if(t.txds || t.models || t.others)
		rsxLog("[tex]   streaming: %u TXDs %.1f ms, %u models %.1f ms (DFF parse %.1f, setup %.1f), %u other %.1f ms; "
		       "slowest %.1f ms (%s); instanced %u in %.1f ms (slowest %.1f ms)",
		       t.txds, t.txdUs/1000.0, t.models, t.modelUs/1000.0, t.dffParseUs/1000.0, t.dffSetupUs/1000.0,
		       t.others, t.otherUs/1000.0, t.worstUs/1000.0, t.worstName,
		       t.instCount, t.instUs/1000.0, t.instWorstUs/1000.0);
	if(t.imgLookups)
		rsxLog("[tex]   textures not in their TXD, looked for as image files: %u names, %.1f ms (once per name)",
		       t.imgLookups, t.imgLookupUs/1000.0);
	if(t.levels || t.textures || t.readbacks || t.indexed)
		rsxLog("[tex]   %u levels, %u Ktexels; palettized fast path: %u textures %.1f ms; "
		       "others: %u converted %.1f ms, to RGBA8 %.1f, swizzle %.1f, VRAM write %.1f, mipmaps %.1f, readback %.1f (%u)",
		       t.levels, (uint32)(t.texels/1000), t.indexed, t.us[TEXPROF_INDEXED]/1000.0,
		       t.textures, t.us[TEXPROF_CONVERT]/1000.0, t.us[TEXPROF_FROMIMAGE]/1000.0, t.us[TEXPROF_SWIZZLE]/1000.0,
		       t.us[TEXPROF_VRAM]/1000.0, t.us[TEXPROF_MIPS]/1000.0, t.us[TEXPROF_READBACK]/1000.0, t.readbacks);
	memset(&texProfile, 0, sizeof(texProfile));
	slowFrames = verySlowFrames = 0;
	worstFrameUs = 0;
	memset(&drawStats, 0, sizeof(drawStats));
	ringPeak = 0;
	statStart = now;
	statFrames = 0;
	statWaitUs = statCpuUs = 0;
}

void
videoPresent(Raster *raster, bool vsync)
{
	u32 srcOffset, srcPitch;
	gcmTransferScale scale;
	gcmTransferSurface surface;
	int availW, availH, outW, outH, offX, offY, srcW, srcH;

	if(!ready)
		return;

	u64 t0 = sysGetSystemTime();
	if(frameStartUs)
		statCpuUs += t0 - frameStartUs;

	if(raster == nil || !rasterSurface(raster, &srcOffset, &srcPitch)){
		// nothing to show: just end the frame
		rsxSetWriteBackendLabel(rsxCtx, LABEL_FRAME, frame);
		rsxFlushBuffer(rsxCtx);
		frame++;
		videoBeginFrame();
		frameStartUs = sysGetSystemTime();
		return;
	}
	srcW = raster->width;
	srcH = raster->height;

	waitFlips();
	u64 t1 = sysGetSystemTime();
	statWaitUs += t1 - t0;

	// keep the picture's aspect, inside the screen fit area, centered
	availW = (int)dispW * settings.screenFit / 100;
	availH = (int)dispH * settings.screenFit / 100;
	if(presentAspect > 0.0f){
		// the TV's pixels are square at 16:9 and 4:3 (1280x720, 720x480 is
		// shown 4:3 or 16:9: dispAspect says which)
		float pixelAspect = dispAspect / ((float)dispW / (float)dispH);
		outH = availH;
		outW = (int)(outH * presentAspect / pixelAspect + 0.5f);
		if(outW > availW){
			outW = availW;
			outH = (int)(outW * pixelAspect / presentAspect + 0.5f);
		}
	}else{
		outH = availH;
		outW = outH * srcW / srcH;
		if(outW > availW){
			outW = availW;
			outH = outW * srcH / srcW;
		}
	}
	outW &= ~1;
	outH &= ~1;
	offX = ((int)dispW - outW) / 2;
	offY = ((int)dispH - outH) / 2;

	if(clearFrames > 0){
		// black borders (only after a fit change: the CPU writes VRAM)
		memset(fb[cur], 0, dispPitch*dispH);
		vramFlush(fb[cur], dispPitch*dispH);
		clearFrames--;
	}

	memset(&scale, 0, sizeof(scale));
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = offX;
	scale.clipY = offY;
	scale.clipW = outW;
	scale.clipH = outH;
	scale.outX = offX;
	scale.outY = offY;
	scale.outW = outW;
	scale.outH = outH;
	scale.ratioX = rsxGetFixedSint32((float)srcW / (float)outW);
	scale.ratioY = rsxGetFixedSint32((float)srcH / (float)outH);
	scale.inW = srcW;
	scale.inH = srcH;
	scale.pitch = srcPitch;
	scale.origin = GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = settings.bilinear ? GCM_TRANSFER_INTERPOLATOR_LINEAR : GCM_TRANSFER_INTERPOLATOR_NEAREST;
	scale.offset = srcOffset;
	scale.inX = 0;
	scale.inY = 0;

	memset(&surface, 0, sizeof(surface));
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = dispPitch;
	surface.offset = fbOffset[cur];

	__asm__ volatile("sync" ::: "memory");
	// the 3D unit must be done with the image before the scaler reads it,
	// and the scaler done before the next frame draws into it again
	rsxSetWaitForIdle(rsxCtx);
	rsxSetTransferScaleSurface(rsxCtx, &scale, &surface);
	rsxSetWaitForIdle(rsxCtx);

	// no gcmSetWaitFlip: the RSX goes on with the next frame while this
	// flip waits for the vsync (waitFlips keeps the CPU from reusing a
	// display buffer that is still on screen or queued)
	gpuMark(GPU_MARK_FRAME_END);
	{
		u64 p0 = sysGetSystemTime();
		paceFlip();
		statWaitUs += sysGetSystemTime() - p0;
	}
	gcmSetFlip(rsxCtx, cur);
	// the frame fence: written once everything before it is done
	rsxSetWriteBackendLabel(rsxCtx, LABEL_FRAME, frame);
	rsxFlushBuffer(rsxCtx);

	flipQueued++;
	if(flipQueued <= 3)
		rsxLog("[rsx] frame %u presented: %dx%d -> %dx%d at %d,%d", flipQueued, srcW, srcH, outW, outH, offX, offY);
	cur = (cur + 1) % FB_COUNT;
	frame++;

	(void)vsync;	// flips are always on vsync (no tearing)

	logStats();
	videoBeginFrame();
	frameStartUs = sysGetSystemTime();
}

}
}

#endif
