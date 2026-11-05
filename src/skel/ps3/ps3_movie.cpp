// ps3_movie.cpp -- the PC intro movies (movies/Logo.mpg, movies/GTAtitles.mpg)
// at start up, only with an empty USRDIR/videointrotest.txt (a test for now).
//
// From reVC-PS3-Optimised (Gamer100Dev, src/skel/ps3/ps3_movie.cpp), changed
// here: the frame buffers are allocated only while a movie plays, the decoder
// thread is joined at the end, the sound is resampled to the 48 kHz of the
// PS3 audio port (the PC movies are 44.1 kHz) and X, O or Start skips.
// Decoder: pl_mpeg (MIT), vendor/pl_mpeg/pl_mpeg.h. MPEG-1 video, MP2 sound.
//
// The decoder runs on its own PPU thread, the sound on a second one with its
// own audio port (the game's audio isn't up yet: CGame::InitialiseOnceAfterRW
// starts it after the movies); the frames are drawn by the game thread.

#include "common.h"
#include "ps3_platform.h"

#if PS3_STAGE != 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>
#include <audio/audio.h>

#include "main.h"
#include "RwHelper.h"
#include "Sprite2d.h"
#include "skeleton.h"
#include "ps3_glfw.h"

#define PL_MPEG_IMPLEMENTATION
#include "../../../vendor/pl_mpeg/pl_mpeg.h"

namespace {

const unsigned MOVIE_MAX_WIDTH = 720;
const unsigned MOVIE_MAX_HEIGHT = 576;
const unsigned MOVIE_SLOT_COUNT = 4;
const unsigned MOVIE_QUEUE_LIMIT = 3;
const unsigned MOVIE_AUDIO_CAPACITY = 32768;	// stereo frames, at 48 kHz
const unsigned MOVIE_AUDIO_BLOCK_SAMPLES = 256;
const unsigned MOVIE_AUDIO_AHEAD = 4;
const double PORT_RATE = 48000.0;

struct MovieFrame {
	unsigned width, height;
	unsigned yWidth, yHeight, cWidth, cHeight;
	double time;
	unsigned serial;
	bool used;
	unsigned char y[MOVIE_MAX_WIDTH * MOVIE_MAX_HEIGHT];
	unsigned char cr[(MOVIE_MAX_WIDTH/2) * (MOVIE_MAX_HEIGHT/2)];
	unsigned char cb[(MOVIE_MAX_WIDTH/2) * (MOVIE_MAX_HEIGHT/2)];
};

MovieFrame *frames;	// MOVIE_SLOT_COUNT, only while the movies play
int frameQueue[MOVIE_SLOT_COUNT];
unsigned queueHead, queueTail, queueCount;
int currentFrame = -1;
unsigned nextFrameSerial, uploadedFrameSerial;
sys_mutex_t movieMutex;
bool movieMutexReady;
bool workerStarted;
volatile uint32 workerQuit;
uint32 movieGeneration;
uint32 finishedGeneration;
uint32 clockGeneration;
uint64 movieClockStartUS;
char requestPath[512];
volatile uint32 decodedCallbackCount;
volatile uint32 decodedVideoCount;

sys_mutex_t audioMutex;
bool audioMutexReady;
bool audioOpen;
volatile uint32 audioStopRequested;
uint32 audioGeneration;
uint32 audioHead, audioTail;
float audioRing[MOVIE_AUDIO_CAPACITY * 2];
// the resampler (the movie's rate to the port's 48 kHz)
double rsStep = 1.0, rsPhase;
float rsLastL, rsLastR;
u32 audioPort;
audioPortConfig audioConfig;
sys_event_queue_t audioQueue;
sys_ipc_key_t audioKey;
sys_ppu_thread_t audioThread;
sys_ppu_thread_t movieThreadId;
CSprite2d movieSprite;
RwCamera *movieCamera;
unsigned audioNextBlock = 1;
unsigned skipPrev = ~0u;	// the buttons held at start don't skip

uint64
nowUS(void)
{
	return sysGetSystemTime();
}

bool
initMutex(sys_mutex_t *mutex, bool *ready, const char *name)
{
	if(*ready) return true;
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	snprintf(attr.name, sizeof(attr.name), "%s", name);
	if(sysMutexCreate(mutex, &attr) != 0)
		return false;
	*ready = true;
	return true;
}

void
audioReset(uint32 generation)
{
	if(!audioMutexReady) return;
	sysMutexLock(audioMutex, 0);
	audioGeneration = generation;
	audioHead = audioTail = 0;
	rsPhase = 0.0;
	rsLastL = rsLastR = 0.0f;
	sysMutexUnlock(audioMutex);
}

inline void
ringPut(float l, float r)
{
	if(audioHead - audioTail >= MOVIE_AUDIO_CAPACITY)
		audioTail++;	// full: the oldest goes
	uint32 at = audioHead % MOVIE_AUDIO_CAPACITY;
	audioRing[at*2] = l;
	audioRing[at*2+1] = r;
	audioHead++;
}

void
audioPush(uint32 generation, const float *samples, unsigned count)
{
	sysMutexLock(audioMutex, 0);
	if(generation != audioGeneration || !audioOpen){
		sysMutexUnlock(audioMutex);
		return;
	}
	// linear interpolation between the last input sample and this one
	for(unsigned i = 0; i < count; i++){
		float l = samples[i*2], r = samples[i*2+1];
		while(rsPhase < 1.0){
			float t = (float)rsPhase;
			ringPut(rsLastL + (l - rsLastL)*t, rsLastR + (r - rsLastR)*t);
			rsPhase += rsStep;
		}
		rsPhase -= 1.0;
		rsLastL = l;
		rsLastR = r;
	}
	sysMutexUnlock(audioMutex);
}

void
audioFill(void)
{
	sysMutexLock(audioMutex, 0);
	unsigned blocks = (unsigned)audioConfig.numBlocks;
	unsigned playing = (unsigned)(*(volatile u64*)(u64)audioConfig.readIndex % blocks);
	unsigned distance = (audioNextBlock + blocks - playing) % blocks;
	if(distance == 0 || distance > MOVIE_AUDIO_AHEAD + 1){
		audioNextBlock = (playing + 1) % blocks;
		distance = 1;
	}
	while(distance <= MOVIE_AUDIO_AHEAD){
		float *out = (float*)(u64)audioConfig.audioDataStart +
		             (u64)audioNextBlock * MOVIE_AUDIO_BLOCK_SAMPLES * audioConfig.channelCount;
		for(unsigned i = 0; i < MOVIE_AUDIO_BLOCK_SAMPLES; i++){
			float l = 0.0f, r = 0.0f;
			if(audioHead != audioTail){
				uint32 a = audioTail % MOVIE_AUDIO_CAPACITY;
				l = audioRing[a*2]; r = audioRing[a*2+1];
				audioTail++;
			}
			out[i*2] = l;
			out[i*2+1] = r;
		}
		audioNextBlock = (audioNextBlock + 1) % blocks;
		distance++;
	}
	sysMutexUnlock(audioMutex);
}

void
movieAudioThread(void *arg)
{
	(void)arg;
	while(__sync_fetch_and_add(&audioStopRequested, 0) == 0){
		sys_event_t event;
		sysEventQueueReceive(audioQueue, &event, 20 * 1000);
		if(__sync_fetch_and_add(&audioStopRequested, 0) == 0)
			audioFill();
	}
	sysThreadExit(0);
}

bool
audioOpenPort(void)
{
	if(audioOpen) return true;
	if(!initMutex(&audioMutex, &audioMutexReady, "mvaud"))
		return false;
	audioReset(movieGeneration);
	if(audioInit() != 0){
		PS3_Log("[movie] audioInit failed: no sound");
		return false;
	}
	audioPortParam param;
	memset(&param, 0, sizeof(param));
	param.numChannels = AUDIO_PORT_2CH;
	param.numBlocks = AUDIO_BLOCK_16;
	param.attrib = 0;
	param.level = 1.0f;
	if(audioPortOpen(&param, &audioPort) != 0){
		audioQuit();
		PS3_Log("[movie] audioPortOpen failed: no sound");
		return false;
	}
	audioGetPortConfig(audioPort, &audioConfig);
	if(audioCreateNotifyEventQueue(&audioQueue, &audioKey) != 0){
		audioPortClose(audioPort);
		audioQuit();
		PS3_Log("[movie] no audio event queue: no sound");
		return false;
	}
	if(audioSetNotifyEventQueue(audioKey) != 0){
		sysEventQueueDestroy(audioQueue, 0);
		audioPortClose(audioPort);
		audioQuit();
		PS3_Log("[movie] no audio notification: no sound");
		return false;
	}
	sysEventQueueDrain(audioQueue);
	memset((void*)(u64)audioConfig.audioDataStart, 0, audioConfig.portSize);
	__sync_lock_test_and_set(&audioStopRequested, 0);
	if(sysThreadCreate(&audioThread, movieAudioThread, nil, 200, 64*1024,
	                   THREAD_JOINABLE, (char*)"movieaudio") != 0){
		audioRemoveNotifyEventQueue(audioKey);
		sysEventQueueDestroy(audioQueue, 0);
		audioPortClose(audioPort);
		audioQuit();
		PS3_Log("[movie] no audio thread: no sound");
		return false;
	}
	sysMutexLock(audioMutex, 0);
	audioOpen = true;
	sysMutexUnlock(audioMutex);
	if(audioPortStart(audioPort) != 0){
		PS3_Log("[movie] audioPortStart failed: no sound");
		sysMutexLock(audioMutex, 0);
		audioOpen = false;
		sysMutexUnlock(audioMutex);
		__sync_lock_test_and_set(&audioStopRequested, 1);
		u64 result;
		sysThreadJoin(audioThread, &result);
		audioRemoveNotifyEventQueue(audioKey);
		sysEventQueueDestroy(audioQueue, 0);
		audioPortClose(audioPort);
		audioQuit();
		return false;
	}
	audioNextBlock = 1;
	return true;
}

void
audioClosePort(void)
{
	if(!audioOpen) return;
	__sync_lock_test_and_set(&audioStopRequested, 1);
	u64 result;
	sysThreadJoin(audioThread, &result);
	audioPortStop(audioPort);
	audioRemoveNotifyEventQueue(audioKey);
	audioPortClose(audioPort);
	sysEventQueueDestroy(audioQueue, 0);
	audioQuit();	// the game's sampman calls audioInit again after this
	sysMutexLock(audioMutex, 0);
	audioOpen = false;
	audioHead = audioTail = 0;
	sysMutexUnlock(audioMutex);
}

void
decodedVideo(plm_t *decoder, plm_frame_t *frame, void *user)
{
	(void)decoder;
	uint32 generation = (uint32)(uintptr_t)user;
	if(!frame || frame->width == 0 || frame->height == 0 ||
	   frame->width > MOVIE_MAX_WIDTH || frame->height > MOVIE_MAX_HEIGHT ||
	   frame->y.width > MOVIE_MAX_WIDTH || frame->y.height > MOVIE_MAX_HEIGHT ||
	   frame->cr.width > MOVIE_MAX_WIDTH/2 || frame->cr.height > MOVIE_MAX_HEIGHT/2 ||
	   frame->y.width < frame->width || frame->y.height < frame->height ||
	   frame->cr.width < (frame->width+1)/2 || frame->cr.height < (frame->height+1)/2 ||
	   frame->cb.width != frame->cr.width || frame->cb.height != frame->cr.height)
		return;
	sysMutexLock(movieMutex, 0);
	if(generation != movieGeneration || frames == nil){
		sysMutexUnlock(movieMutex);
		return;
	}
	if(queueCount >= MOVIE_QUEUE_LIMIT){
		int old = frameQueue[queueTail];
		queueTail = (queueTail + 1) % MOVIE_SLOT_COUNT;
		queueCount--;
		frames[old].used = false;
	}
	int slot = -1;
	for(unsigned i = 0; i < MOVIE_SLOT_COUNT; i++)
		if(!frames[i].used){ slot = (int)i; break; }
	if(slot < 0){
		sysMutexUnlock(movieMutex);
		return;
	}
	MovieFrame &dst = frames[slot];
	dst.used = true;
	dst.width = frame->width; dst.height = frame->height;
	dst.yWidth = frame->y.width; dst.yHeight = frame->y.height;
	dst.cWidth = frame->cr.width; dst.cHeight = frame->cr.height;
	dst.time = frame->time;
	dst.serial = ++nextFrameSerial;
	memcpy(dst.y, frame->y.data, (size_t)dst.yWidth * dst.yHeight);
	memcpy(dst.cr, frame->cr.data, (size_t)dst.cWidth * dst.cHeight);
	memcpy(dst.cb, frame->cb.data, (size_t)dst.cWidth * dst.cHeight);
	if(clockGeneration != generation){
		clockGeneration = generation;
		movieClockStartUS = nowUS();
	}
	frameQueue[queueHead] = slot;
	queueHead = (queueHead + 1) % MOVIE_SLOT_COUNT;
	queueCount++;
	__sync_fetch_and_add(&decodedVideoCount, 1);
	__sync_fetch_and_add(&decodedCallbackCount, 1);
	sysMutexUnlock(movieMutex);
}

void
decodedAudio(plm_t *decoder, plm_samples_t *samples, void *user)
{
	(void)decoder;
	if(samples && samples->count)
		audioPush((uint32)(uintptr_t)user, samples->interleaved, samples->count);
	__sync_fetch_and_add(&decodedCallbackCount, 1);
}

void
finishGeneration(uint32 generation)
{
	sysMutexLock(movieMutex, 0);
	if(movieGeneration == generation)
		finishedGeneration = generation;
	sysMutexUnlock(movieMutex);
}

void
movieWorker(void *arg)
{
	(void)arg;
	uint32 seenGeneration = 0;
	plm_t *decoder = nil;
	uint64 lastCallUS = 0;
	uint64 noProgressSince = 0;
	for(;;){
		char path[sizeof(requestPath)];
		sysMutexLock(movieMutex, 0);
		uint32 generation = movieGeneration;
		bool changed = generation != seenGeneration;
		if(changed) strncpy(path, requestPath, sizeof(path)-1);
		path[sizeof(path)-1] = '\0';
		bool quit = workerQuit != 0;
		sysMutexUnlock(movieMutex);
		if(quit) break;
		if(changed){
			if(decoder){ plm_destroy(decoder); decoder = nil; }
			seenGeneration = generation;
			lastCallUS = 0;
			noProgressSince = 0;
			if(path[0]){
				decoder = plm_create_with_filename(path);
				if(decoder){
					plm_set_loop(decoder, FALSE);
					plm_set_audio_lead_time(decoder, 0.08);
					plm_set_video_decode_callback(decoder, decodedVideo, (void*)(uintptr_t)generation);
					plm_set_audio_decode_callback(decoder, decodedAudio, (void*)(uintptr_t)generation);
					int rate = plm_get_samplerate(decoder);
					sysMutexLock(audioMutex, 0);
					rsStep = rate > 0 ? rate / PORT_RATE : 1.0;
					sysMutexUnlock(audioMutex);
					PS3_Logf("[movie] %s: %dx%d, %.2f fps, sound %d Hz", path, plm_get_width(decoder),
					         plm_get_height(decoder), plm_get_framerate(decoder), rate);
				}else{
					PS3_Logf("[movie] couldn't open %s", path);
					finishGeneration(generation);
				}
			}
		}
		if(!decoder){ sysUsleep(2000); continue; }
		uint64 before = nowUS();
		if(lastCallUS == 0) lastCallUS = before - 20000;
		double delta = (double)(before - lastCallUS) / 1000000.0;
		lastCallUS = before;
		if(delta > 0.1) delta = 0.1;
		uint32 callbacksBefore = __sync_fetch_and_add(&decodedCallbackCount, 0);
		plm_decode(decoder, delta);
		uint32 callbacksAfter = __sync_fetch_and_add(&decodedCallbackCount, 0);
		if(plm_has_ended(decoder)){
			plm_destroy(decoder); decoder = nil;
			finishGeneration(generation);
			lastCallUS = 0;
		}else if(callbacksAfter == callbacksBefore){
			// a damaged file mustn't keep the start up here for ever
			if(noProgressSince == 0) noProgressSince = before;
			if(before - noProgressSince > 5000000ULL){
				PS3_Log("[movie] the decoder made no progress for 5 s: movie skipped");
				plm_destroy(decoder); decoder = nil;
				finishGeneration(generation);
				noProgressSince = 0;
				lastCallUS = 0;
			}
		}else
			noProgressSince = 0;
		if(callbacksAfter == callbacksBefore) sysUsleep(1000);
	}
	if(decoder) plm_destroy(decoder);
	sysThreadExit(0);
}

void
ensureMovieCamera(void)
{
	if(movieCamera) return;
	movieCamera = CameraCreate(RsGlobal.maximumWidth, RsGlobal.maximumHeight, FALSE);
	if(!movieCamera) return;
	RwCameraSetNearClipPlane(movieCamera, 0.1f);
	RwCameraSetFarClipPlane(movieCamera, 10.0f);
	CameraSize(movieCamera, nil, DEFAULT_VIEWWINDOW, DEFAULT_ASPECT_RATIO);
}

bool
uploadCurrentFrame(MovieFrame &frame)
{
	if(movieSprite.m_pTexture &&
	   (RwRasterGetWidth(RwTextureGetRaster(movieSprite.m_pTexture)) != (int)frame.width ||
	    RwRasterGetHeight(RwTextureGetRaster(movieSprite.m_pTexture)) != (int)frame.height))
		movieSprite.Delete();
	if(!movieSprite.m_pTexture){
		RwRaster *raster = RwRasterCreate(frame.width, frame.height, 32,
		                                  rwRASTERFORMAT8888 | rwRASTERTYPETEXTURE);
		if(!raster) return false;
		movieSprite.m_pTexture = RwTextureCreate(raster);
		if(!movieSprite.m_pTexture){ RwRasterDestroy(raster); return false; }
		RwTextureSetFilterMode(movieSprite.m_pTexture, rwFILTERLINEAR);
	}
	RwRaster *raster = RwTextureGetRaster(movieSprite.m_pTexture);
	unsigned char *pixels = raster->lock(0, rw::Raster::LOCKWRITE | rw::Raster::LOCKNOFETCH);
	if(!pixels) return false;
	int stride = raster->stride ? raster->stride : (int)frame.width*4;
	for(unsigned y = 0; y < frame.height; y++){
		unsigned char *row = pixels + y * stride;
		const unsigned char *yrow = frame.y + y * frame.yWidth;
		const unsigned char *crrow = frame.cr + (y/2) * frame.cWidth;
		const unsigned char *cbrow = frame.cb + (y/2) * frame.cWidth;
		for(unsigned x = 0; x < frame.width; x++){
			int yy = (int)yrow[x] - 16;
			int cr = (int)crrow[x/2] - 128;
			int cb = (int)cbrow[x/2] - 128;
			if(yy < 0) yy = 0;
			int r = (298*yy + 409*cr + 128) >> 8;
			int g = (298*yy - 100*cb - 208*cr + 128) >> 8;
			int b = (298*yy + 516*cb + 128) >> 8;
			row[x*4+0] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
			row[x*4+1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
			row[x*4+2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
			row[x*4+3] = 255;
		}
	}
	raster->unlock(0);
	return true;
}

} // namespace

int
PS3_MovieTestOn(void)
{
	FILE *f = fopen(PS3_USRDIR "/videointrotest.txt", "rb");
	if(f == nil)
		return 0;
	fclose(f);
	PS3_Log("[movie] videointrotest.txt: the intro movies are played");
	return 1;
}

int
PS3_MovieStart(const char *path)
{
	if(!initMutex(&movieMutex, &movieMutexReady, "mvdec"))
		return 0;
	if(!initMutex(&audioMutex, &audioMutexReady, "mvaud"))
		return 0;
	char resolved[512];
	if(!path || !PS3_ResolvePath(path, resolved, sizeof(resolved))){
		PS3_Logf("[movie] no %s", path ? path : "(nil)");
		return 0;
	}
	if(frames == nil){
		frames = (MovieFrame*)calloc(MOVIE_SLOT_COUNT, sizeof(MovieFrame));
		if(frames == nil){
			PS3_Log("[movie] no memory for the frames");
			return 0;
		}
	}
	if(!workerStarted){
		workerQuit = 0;
		if(sysThreadCreate(&movieThreadId, movieWorker, nil, 1200, 512*1024, THREAD_JOINABLE,
		                   (char*)"movie") != 0)
			return 0;
		workerStarted = true;
	}
	if(!audioOpen) audioOpenPort();
	sysMutexLock(movieMutex, 0);
	uint32 generation = movieGeneration + 1;
	if(generation == 0) generation = 1;
	movieGeneration = generation;
	finishedGeneration = 0;
	clockGeneration = 0;
	movieClockStartUS = 0;
	queueHead = queueTail = queueCount = 0;
	currentFrame = -1;
	for(unsigned i = 0; i < MOVIE_SLOT_COUNT; i++) frames[i].used = false;
	strncpy(requestPath, resolved, sizeof(requestPath)-1);
	requestPath[sizeof(requestPath)-1] = '\0';
	audioReset(generation);
	sysMutexUnlock(movieMutex);
	skipPrev = ~0u;
	return 1;
}

int
PS3_MovieSkipPressed(void)
{
	PS3_PadPoll();	// the game polls it in CPad::UpdatePads, not running yet
	unsigned m = PS3_PadButtons();
	unsigned want = (1u << GLFW_GAMEPAD_BUTTON_A) | (1u << GLFW_GAMEPAD_BUTTON_B) | (1u << GLFW_GAMEPAD_BUTTON_START);
	unsigned down = m & ~skipPrev;
	skipPrev = m;
	return (down & want) != 0;
}

void
PS3_MovieRenderFrame(void)
{
	if(!movieMutexReady || frames == nil) return;
	sysMutexLock(movieMutex, 0);
	uint32 generation = movieGeneration;
	double elapsed = clockGeneration == generation && movieClockStartUS ?
	                 (double)(nowUS() - movieClockStartUS) / 1000000.0 : 0.0;
	int selected = -1;
	while(queueCount){
		int candidate = frameQueue[queueTail];
		if(frames[candidate].time > elapsed + 0.004)
			break;
		queueTail = (queueTail + 1) % MOVIE_SLOT_COUNT;
		queueCount--;
		if(selected >= 0) frames[selected].used = false;
		selected = candidate;
	}
	if(selected >= 0){
		if(currentFrame >= 0 && currentFrame != selected)
			frames[currentFrame].used = false;
		currentFrame = selected;
	}
	MovieFrame *frame = currentFrame >= 0 ? &frames[currentFrame] : nil;
	bool needsUpload = frame && frame->serial != uploadedFrameSerial;
	RwCamera *savedCamera = Scene.camera;
	sysMutexUnlock(movieMutex);
	// the slot stays "used" until another is selected: the decoder doesn't
	// reuse it during the conversion
	if(needsUpload){
		if(uploadCurrentFrame(*frame))
			uploadedFrameSerial = frame->serial;
		else
			PS3_Log("[movie] frame texture upload failed");
	}

	ensureMovieCamera();
	if(!movieCamera) return;
	RwRGBA black = {0, 0, 0, 255};
	RwCameraClear(movieCamera, &black, rwCAMERACLEARIMAGE);
	if(!RwCameraBeginUpdate(movieCamera)) return;
	Scene.camera = movieCamera;
	CSprite2d::InitPerFrame();
	RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	if(frame && movieSprite.m_pTexture){
		movieSprite.SetRenderState();
		float sw = SCREEN_WIDTH, sh = SCREEN_HEIGHT;
		float aspect = (float)frame->width / (float)frame->height;
		float w = sw, h = sh;
		if(sw / sh > aspect) w = sh * aspect;
		else h = sw / aspect;
		movieSprite.Draw(CRect((sw-w)*0.5f, (sh-h)*0.5f, (sw+w)*0.5f, (sh+h)*0.5f), CRGBA(255,255,255,255));
	}
	RwCameraEndUpdate(movieCamera);
	Scene.camera = savedCamera;
	RsCameraShowRaster(movieCamera);
}

int
PS3_MovieFinished(void)
{
	if(!movieMutexReady) return 1;
	static uint32 watchGeneration, watchCallbacks;
	static uint64 watchProgressUS;
	sysMutexLock(movieMutex, 0);
	if(watchGeneration != movieGeneration){
		watchGeneration = movieGeneration;
		watchCallbacks = __sync_fetch_and_add(&decodedVideoCount, 0);
		watchProgressUS = nowUS();
	}
	uint32 callbacks = __sync_fetch_and_add(&decodedVideoCount, 0);
	if(callbacks != watchCallbacks){
		watchCallbacks = callbacks;
		watchProgressUS = nowUS();
	}
	bool done = finishedGeneration == movieGeneration;
	bool pending = queueCount != 0;
	if(done && !pending && currentFrame >= 0 && movieClockStartUS && frames){
		double elapsed = (double)(nowUS() - movieClockStartUS) / 1000000.0;
		done = elapsed > frames[currentFrame].time + 0.05;
	}
	// no new frame for 5 s: the start up goes on
	if(!done && watchProgressUS && nowUS() - watchProgressUS > 5000000ULL)
		done = true;
	sysMutexUnlock(movieMutex);
	return done ? 1 : 0;
}

void
PS3_MovieCancel(void)
{
	if(!movieMutexReady) return;
	sysMutexLock(movieMutex, 0);
	uint32 generation = movieGeneration + 1;
	if(generation == 0) generation = 1;
	movieGeneration = generation;
	finishedGeneration = generation;
	requestPath[0] = '\0';
	queueHead = queueTail = queueCount = 0;
	currentFrame = -1;
	if(frames)
		for(unsigned i = 0; i < MOVIE_SLOT_COUNT; i++) frames[i].used = false;
	sysMutexUnlock(movieMutex);
	audioReset(generation);
}

void
PS3_MovieShutdown(void)
{
	if(!movieMutexReady)
		return;
	PS3_MovieCancel();
	if(workerStarted){
		sysMutexLock(movieMutex, 0);
		workerQuit = 1;
		sysMutexUnlock(movieMutex);
		u64 result;
		sysThreadJoin(movieThreadId, &result);
		workerStarted = false;
	}
	audioClosePort();
	if(movieCamera){ CameraDestroy(movieCamera); movieCamera = nil; }
	movieSprite.Delete();
	free(frames);
	frames = nil;
	PS3_Log("[movie] done");
}

#endif	// PS3_STAGE != 1
