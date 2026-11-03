// spuparticles.c -- from res-PS3 (patch 50): the particles' sprites on an SPU.
//
// Signal 1: the address of a SpuPartJob (spuparticles.h); 0 ends the program.
// For every batch, in order, every sprite is projected with the camera's view
// matrix (CSprite::CalcScreenCoors: dropped behind the near plane + 1 and past
// the far plane), sized, and built as a quad of 4 Im2DVertex (x, y, screen z,
// camera z, r g b a, u, v) the way CSprite's buffered sprites are: the plain
// ones clipped to the screen with their UVs, the rotated ones only culled.
// The visible quads of all the batches go one after the other to outEa (the
// frame's vertex ring); each batch gets back where its quads start and how
// many there are. Then an event to the PPU.

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>
#include <math.h>

#include "spuparticles.h"

#define TAG_IN		1
#define TAG_OUT		2
#define CHUNK		128	// sprites read at a time
#define OUT_QUADS	128	// quads written at a time (14336 bytes)

typedef struct Vert
{
	float x, y, z, w;
	uint8_t r, g, b, a;
	float u, v;
} __attribute__((packed, aligned(4))) Vert;	// 28 bytes, the PPU's Im2DVertex

static SpuPartJob job __attribute__((aligned(128)));
static SpuPartBatch batches[SPUPART_MAX_BATCHES] __attribute__((aligned(128)));
static SpuPartSprite sprites[CHUNK] __attribute__((aligned(128)));
static uint8_t outBuf[2][OUT_QUADS * SPUPART_QUAD_BYTES] __attribute__((aligned(128)));

static uint32_t chunkFirst = 0xFFFFFFFF;	// sprite index of sprites[0]
static int outCur;			// outBuf being filled
static uint32_t outFill;		// quads in it
static uint32_t outDone;		// quads already sent

static void
waitTag(uint32_t tag)
{
	mfc_write_tag_mask(1 << tag);
	mfc_read_tag_status_all();
}

// size: a multiple of 16, at most 16 KB
static void
dmaGet(volatile void *ls, uint64_t ea, uint32_t size)
{
	mfc_get(ls, ea, size, TAG_IN, 0, 0);
	waitTag(TAG_IN);
}

static void
dmaPutLarge(volatile void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	uint8_t *p = (uint8_t*)ls;
	while(size > 0){
		uint32_t n = size > 16384 ? 16384 : size;
		mfc_put(p, ea, n, tag, 0, 0);
		p += n;
		ea += n;
		size -= n;
	}
}

static void
dmaGetLarge(volatile void *ls, uint64_t ea, uint32_t size)
{
	uint8_t *p = (uint8_t*)ls;
	while(size > 0){
		uint32_t n = size > 16384 ? 16384 : size;
		mfc_get(p, ea, n, TAG_IN, 0, 0);
		p += n;
		ea += n;
		size -= n;
	}
	waitTag(TAG_IN);
}

static const SpuPartSprite*
getSprite(uint32_t i)
{
	if(chunkFirst == 0xFFFFFFFF || i < chunkFirst || i >= chunkFirst + CHUNK){
		uint32_t n = job.numSprites - i;
		if(n > CHUNK)
			n = CHUNK;
		dmaGet(sprites, job.spritesEa + (uint64_t)i * sizeof(SpuPartSprite), n * sizeof(SpuPartSprite));
		chunkFirst = i;
	}
	return &sprites[i - chunkFirst];
}

static void
flushOut(void)
{
	if(outFill == 0)
		return;
	dmaPutLarge(outBuf[outCur], job.outEa + (uint64_t)outDone * SPUPART_QUAD_BYTES,
	            outFill * SPUPART_QUAD_BYTES, TAG_OUT + outCur);
	outDone += outFill;
	outFill = 0;
	outCur ^= 1;
	waitTag(TAG_OUT + outCur);	// the other buffer's put is done before it's refilled
}

static Vert*
newQuad(void)
{
	if(outFill == OUT_QUADS)
		flushOut();
	return (Vert*)&outBuf[outCur][outFill++ * SPUPART_QUAD_BYTES];
}

// CSprite::CalcScreenCoors with farclip: false when not drawn
static int
project(float x, float y, float z, float *sx, float *sy, float *sz, float *recip)
{
	const float *m = job.view;
	float vx = m[0] * x + m[3] * y + m[6] * z + m[9];
	float vy = m[1] * x + m[4] * y + m[7] * z + m[10];
	float vz = m[2] * x + m[5] * y + m[8] * z + m[11];
	if(vz <= job.nearClip + 1.0f)
		return 0;
	if(vz >= job.farClip)
		return 0;
	float r = 1.0f / vz;
	*sx = vx * job.screenW * r;
	*sy = vy * job.screenH * r;
	*sz = vz;
	*recip = r;
	return 1;
}

static void
setVert(Vert *v, float x, float y, float screenz, float z, const SpuPartSprite *s, uint8_t a, float u, float vv)
{
	v->x = x;
	v->y = y;
	v->z = screenz;
	v->w = z;
	v->r = s->r;
	v->g = s->g;
	v->b = s->b;
	v->a = a;
	v->u = u;
	v->v = vv;
}

static void
buildSprite(const SpuPartSprite *s)
{
	float x, y, z, recip;
	if(!project(s->x, s->y, s->z, &x, &y, &z, &recip))
		return;
	float w = job.kw * recip + s->addW;
	float h = job.kh * recip + s->addH;
	if(s->flags & SPUPART_MIN12){
		if(w < 12.0f) w = 12.0f;
		if(h < 12.0f) h = 12.0f;
	}
	// re3/reVC: the PC's limits, then the PS3 fill limit (Particle.cpp's
	// PS3_FxQuad), on the sizes before the sprite's own multipliers
	if((s->flags & SPUPART_LIMIT_W) && w > job.screenW * job.scaleLimit)
		return;
	if((s->flags & SPUPART_LIMIT_H) && h > job.screenH * job.scaleLimit)
		return;
	uint8_t alpha = s->a;
	if(s->flags & SPUPART_FADE){
		float hw = fabsf(s->size * w), hh = fabsf(s->size * h);
		float r = (hw > hh ? hw : hh) / job.screenH;
		if(r > 0.35f){
			float k = (0.7f - r) / 0.35f;
			int a = k > 0.0f ? (int)(s->a * k) : 0;
			if(a < 4)
				return;
			alpha = (uint8_t)a;
		}
	}
	w *= s->wMul;
	h *= s->hMul;
	float rot = s->rot;
	int kind = s->kind;

	if(kind == SPUPART_SPEEDTRAIL){
		float px, py, pz, pr;
		float trail = 0.0f;
		rot = 0.0f;
		if(project(s->px, s->py, s->pz, &px, &py, &pz, &pr)){
			float dx = x - px, dy = y - py;
			float dist = sqrtf(dx * dx + dy * dy);
			if(dist > 0.0f){
				trail = dist;
				rot = asinf(dx / dist);
				if(dy < 0.0f)
					rot = -rot + 3.14159265f;
			}
		}
		h += trail * s->trailMul;
		kind = SPUPART_ROTATE;
	}

	float screenz = job.near2D + (z - job.nearClip) * (job.far2D - job.near2D) * job.farClip /
		((job.farClip - job.nearClip) * z);
	float xs[4], ys[4], us[4], vs[4];
	int i;

	if(kind == SPUPART_ROTATE){
		float c = cosf(rot), sn = sinf(rot);
		xs[0] = x - c * w - sn * h;	us[0] = 0.0f;
		xs[1] = x - c * w + sn * h;	us[1] = 0.0f;
		xs[2] = x + c * w + sn * h;	us[2] = 1.0f;
		xs[3] = x + c * w - sn * h;	us[3] = 1.0f;
		ys[0] = y - c * h + sn * w;	vs[0] = 0.0f;
		ys[1] = y + c * h + sn * w;	vs[1] = 1.0f;
		ys[2] = y + c * h - sn * w;	vs[2] = 1.0f;
		ys[3] = y - c * h - sn * w;	vs[3] = 0.0f;
		if(xs[0] < 0.0f && xs[1] < 0.0f && xs[2] < 0.0f && xs[3] < 0.0f) return;
		if(ys[0] < 0.0f && ys[1] < 0.0f && ys[2] < 0.0f && ys[3] < 0.0f) return;
		float sw = job.screenW, sh = job.screenH;
		if(xs[0] > sw && xs[1] > sw && xs[2] > sw && xs[3] > sw) return;
		if(ys[0] > sh && ys[1] > sh && ys[2] > sh && ys[3] > sh) return;
	}else{
		xs[0] = x - w;	us[0] = 0.0f;
		xs[1] = x - w;	us[1] = 0.0f;
		xs[2] = x + w;	us[2] = 1.0f;
		xs[3] = x + w;	us[3] = 1.0f;
		ys[0] = y - h;	vs[0] = 0.0f;
		ys[1] = y + h;	vs[1] = 1.0f;
		ys[2] = y + h;	vs[2] = 1.0f;
		ys[3] = y - h;	vs[3] = 0.0f;
		for(i = 0; i < 4; i++){
			if(xs[i] < 0.0f){
				us[i] = -xs[i] / (2.0f * w);
				xs[i] = 0.0f;
			}
			if(xs[i] > job.screenW){
				us[i] = 1.0f - (xs[i] - job.screenW) / (2.0f * w);
				xs[i] = job.screenW;
			}
			if(ys[i] < 0.0f){
				vs[i] = -ys[i] / (2.0f * h);
				ys[i] = 0.0f;
			}
			if(ys[i] > job.screenH){
				vs[i] = 1.0f - (ys[i] - job.screenH) / (2.0f * h);
				ys[i] = job.screenH;
			}
		}
		// clipped to nothing: off screen
		if(xs[0] == xs[2] || ys[0] == ys[1])
			return;
	}
	if(!(xs[0] == xs[0]) || !(ys[0] == ys[0]) || !(screenz == screenz))
		return;	// NaN: the PPU would draw nothing visible either

	Vert *q = newQuad();
	for(i = 0; i < 4; i++)
		setVert(&q[i], xs[i], ys[i], screenz, z, s, alpha, us[i], vs[i]);
}

int
main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
	(void)arg1; (void)arg2; (void)arg3; (void)arg4;
	for(;;){
		uint32_t jobEa = spu_read_signal1();
		if(jobEa == SPUPART_QUIT)
			break;
		dmaGet(&job, jobEa, sizeof(job));
		uint32_t nb = job.numBatches < SPUPART_MAX_BATCHES ? job.numBatches : SPUPART_MAX_BATCHES;
		if(job.numSprites > SPUPART_MAX_SPRITES)
			job.numSprites = SPUPART_MAX_SPRITES;
		uint32_t bbytes = (nb * sizeof(SpuPartBatch) + 15) & ~15;
		if(bbytes)
			dmaGetLarge(batches, job.batchesEa, bbytes);

		chunkFirst = 0xFFFFFFFF;
		outCur = 0;
		outFill = 0;
		outDone = 0;
		uint32_t b, i;
		for(b = 0; b < nb; b++){
			SpuPartBatch *bt = &batches[b];
			uint32_t start = outDone + outFill;
			uint32_t end = bt->first + bt->count;
			if(end > job.numSprites)
				end = job.numSprites;
			for(i = bt->first; i < end; i++)
				buildSprite(getSprite(i));
			bt->outFirst = start;
			bt->outCount = outDone + outFill - start;
		}
		flushOut();
		job.numQuads = outDone;
		if(bbytes)
			dmaPutLarge(batches, job.batchesEa, bbytes, TAG_IN);
		dmaPutLarge(&job, jobEa, sizeof(job), TAG_IN);
		waitTag(TAG_IN);
		waitTag(TAG_OUT);
		waitTag(TAG_OUT + 1);
		spu_thread_send_event(SPUPART_SPUP, job.seq & 0xFFFFFF, job.numQuads);
	}
	spu_thread_exit(0);
	return 0;
}
