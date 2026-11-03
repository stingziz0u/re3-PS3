// spumix.c -- from res-PS3 (patch 49): the audio mixer on an SPU.
//
// One SPU thread waits on signal 1 for the address of a SpuMixJob (spumix.h),
// mixes it the way sampman_ps3.cpp's MixChannel/MixStream do (linear
// interpolation, the same loop rules), puts the 256 stereo floats at the
// job's outEa, writes the job back with the new positions and sends an event
// to the PPU. Signal 0 ends the program.
//
// The samples stay in main memory: each channel reads a window of up to 4096
// samples by DMA and reads the next window when it runs out of it (or loops
// back); the streams read their ring the same way, 1024 frames at a time.
// The DMA reads are 16-byte aligned blocks: the bytes before and after the
// data in those blocks are in the same page, so they can be read.

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>

#include "spumix.h"

#define TAG		1
#define WIN_SAMPLES	4096
#define SWIN_FRAMES	1024
#define FRAC_TO_FLOAT	(1.0f / 4294967296.0f)

static SpuMixJob job __attribute__((aligned(128)));
static float mix[SPUMIX_BLOCK * 2] __attribute__((aligned(128)));
static int16_t win[WIN_SAMPLES + 64] __attribute__((aligned(128)));
static int16_t swin[(SWIN_FRAMES + 8) * 2] __attribute__((aligned(128)));

// the sample window: win[0] is sample winBase (may be < 0: the bytes before
// the data in its first 16-byte block) of the data at winEa
static uint64_t winEa;
static int32_t winBase, winCount;
// the ring window: swin frame 0 is ring frame swBase of the ring at swEa
static uint64_t swEa;
static uint32_t swBase, swCount;

static void
dmaGet(volatile void *ls, uint64_t ea, uint32_t size)
{
	// size: a multiple of 16, at most 16 KB; ls and ea 16-byte aligned
	mfc_get(ls, ea, size, TAG, 0, 0);
	mfc_write_tag_mask(1 << TAG);
	mfc_read_tag_status_all();
}

static void
dmaPut(volatile void *ls, uint64_t ea, uint32_t size)
{
	mfc_put(ls, ea, size, TAG, 0, 0);
}

// samples [from, min(len, from + WIN_SAMPLES)) of the data at ea
static void
fetchSamples(uint64_t ea, int32_t len, int32_t from)
{
	int32_t to = from + WIN_SAMPLES;
	if(to > len)
		to = len;
	uint64_t a = (ea + (uint64_t)from * 2) & ~15ULL;
	uint64_t b = (ea + (uint64_t)to * 2 + 15) & ~15ULL;
	uint32_t size = (uint32_t)(b - a);
	dmaGet(win, a, size);
	winEa = ea;
	winBase = (int32_t)((int64_t)(a - ea) / 2);
	winCount = (int32_t)(size / 2);
}

// ring frames [r, min(ringFrames, r + SWIN_FRAMES)): never across the end of
// the ring (16-byte aligned, a multiple of 4 frames)
static void
fetchFrames(uint64_t ringEa, uint32_t ringFrames, uint32_t r)
{
	uint32_t to = r + SWIN_FRAMES;
	if(to > ringFrames)
		to = ringFrames;
	uint64_t a = (ringEa + (uint64_t)r * 4) & ~15ULL;
	uint64_t b = (ringEa + (uint64_t)to * 4 + 15) & ~15ULL;
	uint32_t size = (uint32_t)(b - a);
	dmaGet(swin, a, size);
	swEa = ringEa;
	swBase = (uint32_t)((a - ringEa) / 4);
	swCount = size / 4;
}

static void
mixChannel(SpuMixChannel *c)
{
	const int32_t len = c->len;
	int32_t ipos = c->ipos;
	uint32_t fpos = c->fpos;
	const uint32_t istep = c->istep, fstep = c->fstep;
	int32_t timesPlayed = c->timesPlayed;
	const float gl = c->gl, gr = c->gr;
	float *out = mix;
	int i;

	for(i = 0; i < SPUMIX_BLOCK; i++){
		int32_t end = (c->loopEnd < 0 || c->loopEnd > len) ? len : c->loopEnd;
		int looping = c->loopCount == 0 || timesPlayed + 1 < c->loopCount;
		if(!looping)
			end = len;
		if(ipos >= end){
			if(looping && end > c->loopStart){
				ipos -= end - c->loopStart;
				if(ipos < c->loopStart){
					ipos = c->loopStart;
					fpos = 0;
				}
				timesPlayed++;
			}else{
				c->flags |= SPUMIX_STOPPED;
				break;
			}
		}
		if(ipos < 0 || ipos >= len){
			c->flags |= SPUMIX_STOPPED;
			break;
		}
		int32_t i0 = ipos;
		int32_t i1 = i0 + 1 < len ? i0 + 1 : i0;
		if(winEa != c->ea || i0 < winBase || i1 >= winBase + winCount)
			fetchSamples(c->ea, len, i0);
		float d0 = (float)win[i0 - winBase];
		float d1 = (float)win[i1 - winBase];
		float t = (float)fpos * FRAC_TO_FLOAT;
		float s = d0 + (d1 - d0) * t;
		out[i * 2] += s * gl;
		out[i * 2 + 1] += s * gr;
		uint32_t f = fpos + fstep;
		ipos += (int32_t)istep + (f < fpos ? 1 : 0);
		fpos = f;
	}
	c->ipos = ipos;
	c->fpos = fpos;
	c->timesPlayed = timesPlayed;
}

static void
mixStream(SpuMixStream *s, int mono)
{
	const uint32_t ringFrames = s->ringFrames;
	uint32_t tail = s->tail;
	uint32_t frac = s->ffrac;
	const uint32_t head = s->head;
	const float gl = s->gl, gr = s->gr;
	float *out = mix;
	int i;

	for(i = 0; i < SPUMIX_BLOCK; i++){
		if(head - tail < 2)
			break;	// underrun (or the end): silence, no advance
		uint32_t ra = tail % ringFrames;
		uint32_t rb = (tail + 1) % ringFrames;
		if(swEa != s->ringEa || ra < swBase || ra >= swBase + swCount)
			fetchFrames(s->ringEa, ringFrames, ra);
		float a0 = (float)swin[(ra - swBase) * 2];
		float a1 = (float)swin[(ra - swBase) * 2 + 1];
		if(rb < swBase || rb >= swBase + swCount)
			fetchFrames(s->ringEa, ringFrames, rb);
		float b0 = (float)swin[(rb - swBase) * 2];
		float b1 = (float)swin[(rb - swBase) * 2 + 1];
		float t = (float)frac * FRAC_TO_FLOAT;
		float l = a0 + (b0 - a0) * t;
		float r = a1 + (b1 - a1) * t;
		if(mono)
			l = r = (l + r) * 0.5f;
		out[i * 2] += l * gl;
		out[i * 2 + 1] += r * gr;
		uint32_t f = frac + s->fstep;
		tail += s->istep + (f < frac ? 1 : 0);
		frac = f;
	}
	s->tail = tail;
	s->ffrac = frac;
}

int
main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4;
	for(;;){
		uint32_t jobEa = spu_read_signal1();
		if(jobEa == SPUMIX_QUIT)
			break;
		dmaGet(&job, jobEa, sizeof(job));

		vector float zero = spu_splats(0.0f);
		vector float *mv = (vector float*)mix;
		int i;
		for(i = 0; i < SPUMIX_BLOCK * 2 / 4; i++)
			mv[i] = zero;

		// the windows hold what the PPU had in main memory a block ago: the
		// stream rings are rewritten in place, so read again
		winEa = 0;
		swEa = 0;
		uint32_t n = job.numChannels < SPUMIX_MAX_CHANNELS ? job.numChannels : SPUMIX_MAX_CHANNELS;
		for(i = 0; i < (int)n; i++)
			if(job.ch[i].flags & SPUMIX_ACTIVE)
				mixChannel(&job.ch[i]);
		n = job.numStreams < SPUMIX_MAX_STREAMS ? job.numStreams : SPUMIX_MAX_STREAMS;
		for(i = 0; i < (int)n; i++)
			if(job.st[i].flags & SPUMIX_ACTIVE)
				mixStream(&job.st[i], job.mono);

		dmaPut(mix, job.outEa, sizeof(mix));
		dmaPut(&job, jobEa, sizeof(job));
		mfc_write_tag_mask(1 << TAG);
		mfc_read_tag_status_all();
		spu_thread_send_event(SPUMIX_SPUP, job.seq & 0xFFFFFF, n);
	}
	spu_thread_exit(0);
	return 0;
}
