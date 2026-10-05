// sampman_ps3.cpp -- re3's sample manager on the PS3 (stage 2, AUDIO_PS3).
//
// sampman_oal.cpp's logic (sample banks, ped comment slots, streams, the
// MP3 player radio), with OpenAL replaced by a software mixer:
//
//  - one audio port, 2 channels, 16 blocks of 256 float samples at 48000 Hz,
//    fed by a thread woken once per block (Doom64-PS3 / Quake2PS3's scheme,
//    proven on hardware);
//  - 28 + 1 sample channels: 16 bit samples from sfx.raw (little-endian on
//    disc, swapped once when loaded), linear interpolation for the pitch,
//    the 3D ones attenuated like OpenAL's inverse distance clamped model
//    and panned from their position (sampman_oal's OpenAL setup);
//  - 2 streams (radio, cutscene and mission speech): WAV (PCM / IMA ADPCM)
//    and MP3 (minimp3), decoded ahead by their own thread into a ring the
//    mixer reads.
//  - the MP3 player radio: the .mp3 files of USRDIR/mp3, in name order, one
//    long station (a position in it = a file + a position in the file).

#include "common.h"

#ifdef AUDIO_PS3

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/systime.h>

#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>
#include <audio/audio.h>

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "Frontend.h"
#include "Timer.h"
#include "crossplatform.h"

#include "ps3_platform.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

cSampleManager SampleManager;
bool _bSampmanInitialised = false;

uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s;
int defaultProvider = 0;

static char SampleBankDescFilename[] = "audio/sfx.SDT";
static char SampleBankDataFilename[] = "audio/sfx.RAW";

static FILE *fpSampleDescHandle;
static FILE *fpSampleDataHandle;
static bool  bSampleBankLoaded[MAX_SFX_BANKS];
static int32 nSampleBankDiscStartOffset[MAX_SFX_BANKS];
static int32 nSampleBankSize[MAX_SFX_BANKS];
static uintptr nSampleBankMemoryStartAddress[MAX_SFX_BANKS];

static int32 nPedSlotSfx[MAX_PEDSFX];
static uint8 nCurrentPedSlot;

static uint8 nChannelVolume[MAXCHANNELS+MAX2DCHANNELS];
static uint32 nStreamLength[TOTAL_STREAMED_SOUNDS];
static uint8 nStreamPan[MAX_STREAMS];
static uint8 nStreamVolume[MAX_STREAMS];

static char ProviderName[] = "PS3 STEREO";

#define OUT_RATE	48000
#define OUT_CHANNELS	2
#define AUDIO_AHEAD	4	// blocks (~21 ms)

// ============================================================================
// Locks
// ============================================================================

static sys_mutex_t mixMutex;	// channels, stream rings (mixer vs game)
static sys_mutex_t streamMutex;	// stream decoders (decode thread vs game)

static void
CreateMutex(sys_mutex_t *m, const char *name)
{
	sys_mutex_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	attr.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	strncpy(attr.name, name, sizeof(attr.name)-1);
	sysMutexCreate(m, &attr);
}

struct MixLock {
	MixLock() { sysMutexLock(mixMutex, 0); }
	~MixLock() { sysMutexUnlock(mixMutex); }
};
struct StreamLock {
	StreamLock() { sysMutexLock(streamMutex, 0); }
	~StreamLock() { sysMutexUnlock(streamMutex); }
};

// ============================================================================
// Sample channels
// ============================================================================

struct Channel
{
	const int16 *data;
	uint32 length;		// samples
	uint32 baseFreq;
	uint32 freq;		// playback rate, Hz
	double pos;		// in samples
	int32 loopStart, loopEnd;	// samples, loopEnd -1 = the end
	int32 loopCount;	// 0 forever, n: n times
	int32 timesPlayed;
	bool playing;

	// volume 0..127 (with the master volumes)
	int32 volume;
	// 3D: listener space (OpenAL's: x right), distances
	bool is3D;
	float x, y, z;
	float maxDist, minDist;
	// 2D
	int32 pan;		// 0..127
};

static Channel channels[MAXCHANNELS+MAX2DCHANNELS];

// ============================================================================
// Stream decoders
// ============================================================================

class Decoder
{
public:
	virtual ~Decoder() {}
	virtual bool IsOpened() = 0;
	virtual uint32 GetRate() = 0;
	virtual uint32 GetLengthMS() = 0;
	virtual void Seek(uint32 ms) = 0;
	// interleaved stereo frames; 0 at the end
	virtual uint32 Decode(int16 *out, uint32 maxFrames) = 0;
};

static inline uint16 rd16(const uint8 *p) { return p[0] | p[1]<<8; }
static inline uint32 rd32(const uint8 *p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32)p[3]<<24; }

// RIFF WAVE: 16 bit PCM or IMA ADPCM (sampman_oal's CWavFile, any endian)
class WavDecoder : public Decoder
{
	enum { FMT_PCM = 1, FMT_IMA_ADPCM = 0x11, FMT_XBOX_ADPCM = 0x69 };

	FILE *f;
	bool opened;
	uint16 format, numChannels, blockAlign, bits;
	uint32 rate;
	uint32 dataStart, dataSize;
	uint32 samplesPerBlock;
	uint32 sampleCount;	// per channel
	uint32 curSample;	// per channel, next to decode

	uint8 *block;
	int16 *blockPcm;	// decoded block, interleaved
	uint32 blockPcmFrames, blockPcmPos;

	static int16 ImaStep(int16 &sample, int16 &index, uint8 nib)
	{
		static const uint16 StepTable[89] = {
			7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
			34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
			157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
			724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
			3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
			15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
		};
		uint16 step = StepTable[index];
		if(nib & 4) index += ((nib & 3) + 1) * 2;
		else index--;
		if(index < 0) index = 0;
		if(index > 88) index = 88;
		int delta = step >> 3;
		if(nib & 1) delta += step >> 2;
		if(nib & 2) delta += step >> 1;
		if(nib & 4) delta += step;
		if(nib & 8) delta = -delta;
		int s = sample + delta;
		if(s < -32768) s = -32768;
		if(s > 32767) s = 32767;
		sample = s;
		return sample;
	}

	bool DecodeBlock(void)
	{
		blockPcmFrames = blockPcmPos = 0;
		if(fread(block, 1, blockAlign, f) != blockAlign)
			return false;
		int ch = numChannels;
		int16 smp[2], idx[2];
		const uint8 *p = block;
		for(int c = 0; c < ch; c++){
			smp[c] = (int16)rd16(p);
			idx[c] = p[2];
			if(idx[c] > 88) idx[c] = 88;
			p += 4;
			blockPcm[c] = smp[c];
		}
		uint32 frame = 1;
		// then 4 bytes (8 samples) per channel in turn
		while(frame < samplesPerBlock){
			for(int c = 0; c < ch; c++){
				for(int b = 0; b < 4; b++){
					uint8 v = p[b];
					uint32 f0 = frame + b*2;
					int16 s0 = ImaStep(smp[c], idx[c], v & 0xF);
					int16 s1 = ImaStep(smp[c], idx[c], v >> 4);
					if(f0 < samplesPerBlock) blockPcm[f0*ch + c] = s0;
					if(f0+1 < samplesPerBlock) blockPcm[(f0+1)*ch + c] = s1;
				}
				p += 4;
			}
			frame += 8;
		}
		blockPcmFrames = samplesPerBlock;
		return true;
	}

public:
	WavDecoder(const char *path) : f(nil), opened(false), block(nil), blockPcm(nil),
		blockPcmFrames(0), blockPcmPos(0), curSample(0)
	{
		uint8 hdr[12], ck[8], fmt[40];
		f = fopen(path, "rb");
		if(f == nil)
			return;
		if(fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr+8, "WAVE", 4) != 0)
			return;
		bool haveFmt = false;
		for(;;){
			if(fread(ck, 1, 8, f) != 8)
				return;
			uint32 size = rd32(ck+4);
			if(memcmp(ck, "fmt ", 4) == 0){
				uint32 n = size < sizeof(fmt) ? size : sizeof(fmt);
				memset(fmt, 0, sizeof(fmt));
				if(fread(fmt, 1, n, f) != n)
					return;
				if(size > n)
					fseek(f, size - n, SEEK_CUR);
				format = rd16(fmt);
				numChannels = rd16(fmt+2);
				rate = rd32(fmt+4);
				blockAlign = rd16(fmt+12);
				bits = rd16(fmt+14);
				haveFmt = true;
			}else if(memcmp(ck, "data", 4) == 0){
				dataStart = ftell(f);
				dataSize = size;
				break;
			}else
				fseek(f, size + (size & 1), SEEK_CUR);
		}
		if(!haveFmt || numChannels < 1 || numChannels > 2 || rate == 0 || blockAlign == 0)
			return;
		if(format == FMT_XBOX_ADPCM)
			format = FMT_IMA_ADPCM;
		if(format == FMT_PCM){
			if(bits != 16)
				return;
			samplesPerBlock = 1;
		}else if(format == FMT_IMA_ADPCM){
			samplesPerBlock = (blockAlign / numChannels - 4) * 2 + 1;
			block = new uint8[blockAlign];
			blockPcm = new int16[samplesPerBlock * numChannels];
		}else{
			PS3_Logf("[audio] %s: unsupported WAV format 0x%x", path, format);
			return;
		}
		sampleCount = dataSize / blockAlign * samplesPerBlock;
		opened = true;
	}
	~WavDecoder()
	{
		if(f) fclose(f);
		delete[] block;
		delete[] blockPcm;
	}
	bool IsOpened() { return opened; }
	uint32 GetRate() { return rate; }
	uint32 GetLengthMS() { return opened ? (uint32)((uint64)sampleCount * 1000 / rate) : 0; }
	void Seek(uint32 ms)
	{
		if(!opened) return;
		uint32 s = (uint32)((uint64)ms * rate / 1000);
		uint32 b = s / samplesPerBlock;
		fseek(f, dataStart + b * blockAlign, SEEK_SET);
		curSample = b * samplesPerBlock;
		blockPcmFrames = blockPcmPos = 0;
	}
	uint32 Decode(int16 *out, uint32 maxFrames)
	{
		if(!opened) return 0;
		uint32 n = 0;
		if(format == FMT_PCM){
			int16 tmp[2048];
			while(n < maxFrames && curSample < sampleCount){
				uint32 want = maxFrames - n;
				if(want > 2048 / numChannels) want = 2048 / numChannels;
				if(want > sampleCount - curSample) want = sampleCount - curSample;
				uint32 got = fread(tmp, 2*numChannels, want, f);
				if(got == 0) break;
				for(uint32 i = 0; i < got; i++){
					int16 l = (int16)rd16((uint8*)&tmp[i*numChannels]);
					int16 r = numChannels == 2 ? (int16)rd16((uint8*)&tmp[i*numChannels+1]) : l;
					out[(n+i)*2] = l;
					out[(n+i)*2+1] = r;
				}
				n += got;
				curSample += got;
			}
			return n;
		}
		while(n < maxFrames){
			if(blockPcmPos >= blockPcmFrames){
				if(curSample >= sampleCount || !DecodeBlock())
					break;
			}
			while(blockPcmPos < blockPcmFrames && n < maxFrames){
				int16 l = blockPcm[blockPcmPos*numChannels];
				int16 r = numChannels == 2 ? blockPcm[blockPcmPos*numChannels+1] : l;
				out[n*2] = l;
				out[n*2+1] = r;
				n++;
				blockPcmPos++;
				curSample++;
			}
		}
		return n;
	}
};

// MP3 (minimp3)
class Mp3Decoder : public Decoder
{
	FILE *f;
	bool opened;
	mp3dec_t dec;
	uint32 rate;
	uint32 channelsInFile;
	uint32 dataStart, fileSize;
	uint32 lengthMs;
	uint32 bitrate;		// kbps of the first frame (for seeking)

	uint8 *in;
	uint32 inSize, inPos;
	bool eof;
	int16 pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	uint32 pcmFrames, pcmPos, pcmChannels;

	enum { IN_BUF = 16384 };

	void Refill(void)
	{
		if(inPos > 0){
			memmove(in, in + inPos, inSize - inPos);
			inSize -= inPos;
			inPos = 0;
		}
		if(!eof && inSize < IN_BUF){
			uint32 got = fread(in + inSize, 1, IN_BUF - inSize, f);
			if(got == 0)
				eof = true;
			inSize += got;
		}
	}

	bool DecodeFrame(void)
	{
		for(int tries = 0; tries < 64; tries++){
			if(inSize - inPos < IN_BUF/2)
				Refill();
			if(inSize - inPos == 0)
				return false;
			mp3dec_frame_info_t info;
			int samples = mp3dec_decode_frame(&dec, in + inPos, inSize - inPos, pcm, &info);
			if(info.frame_bytes == 0){
				// need more data or nothing left
				if(eof){
					inPos = inSize;
					return false;
				}
				Refill();
				continue;
			}
			inPos += info.frame_bytes;
			if(samples > 0){
				pcmFrames = samples;
				pcmPos = 0;
				pcmChannels = info.channels;
				return true;
			}
		}
		return false;
	}

public:
	Mp3Decoder(const char *path) : f(nil), opened(false), in(nil), inSize(0), inPos(0), eof(false),
		pcmFrames(0), pcmPos(0), pcmChannels(2)
	{
		f = fopen(path, "rb");
		if(f == nil)
			return;
		fseek(f, 0, SEEK_END);
		fileSize = ftell(f);
		fseek(f, 0, SEEK_SET);
		// skip an ID3v2 tag
		uint8 id3[10];
		dataStart = 0;
		if(fread(id3, 1, 10, f) == 10 && memcmp(id3, "ID3", 3) == 0)
			dataStart = 10 + ((id3[6]&0x7F)<<21 | (id3[7]&0x7F)<<14 | (id3[8]&0x7F)<<7 | (id3[9]&0x7F));
		fseek(f, dataStart, SEEK_SET);
		in = new uint8[IN_BUF];
		mp3dec_init(&dec);

		// first frame: rate, channels, bitrate
		Refill();
		mp3dec_frame_info_t info;
		int samples = 0;
		uint32 skipped = 0;
		for(int i = 0; i < 16 && samples == 0; i++){
			samples = mp3dec_decode_frame(&dec, in + inPos, inSize - inPos, pcm, &info);
			if(info.frame_bytes == 0)
				break;
			if(samples == 0){
				inPos += info.frame_bytes;
				skipped += info.frame_bytes;
			}
		}
		if(samples == 0 || info.hz == 0)
			return;
		rate = info.hz;
		channelsInFile = info.channels;
		bitrate = info.bitrate_kbps;
		// Xing/Info header of a VBR file: the frame count
		lengthMs = 0;
		{
			const uint8 *fr = in + inPos;
			uint32 fb = info.frame_bytes;
			for(uint32 o = 4; o + 12 < fb && o < 64; o++){
				if(memcmp(fr+o, "Xing", 4) == 0 || memcmp(fr+o, "Info", 4) == 0){
					uint32 flags = fr[o+4]<<24 | fr[o+5]<<16 | fr[o+6]<<8 | fr[o+7];
					if(flags & 1){
						uint32 frames = fr[o+8]<<24 | fr[o+9]<<16 | fr[o+10]<<8 | fr[o+11];
						uint32 spf = info.layer == 1 ? 384 : (info.hz < 32000 ? 576 : 1152);
						lengthMs = (uint32)((uint64)frames * spf * 1000 / rate);
					}
					break;
				}
			}
		}
		if(lengthMs == 0 && bitrate)
			lengthMs = (uint32)((uint64)(fileSize - dataStart - skipped) * 8 / bitrate);
		// back to the start, decoding from scratch
		Seek(0);
		opened = true;
	}
	~Mp3Decoder()
	{
		if(f) fclose(f);
		delete[] in;
	}
	bool IsOpened() { return opened; }
	uint32 GetRate() { return rate; }
	uint32 GetLengthMS() { return lengthMs; }
	void Seek(uint32 ms)
	{
		// constant bitrate assumed (the game's files): byte position from it
		uint32 pos = dataStart;
		if(ms && bitrate)
			pos += (uint32)((uint64)ms * bitrate / 8);
		if(pos >= fileSize)
			pos = fileSize;
		fseek(f, pos, SEEK_SET);
		mp3dec_init(&dec);
		inSize = inPos = 0;
		eof = false;
		pcmFrames = pcmPos = 0;
	}
	uint32 Decode(int16 *out, uint32 maxFrames)
	{
		uint32 n = 0;
		while(n < maxFrames){
			if(pcmPos >= pcmFrames && !DecodeFrame())
				break;
			while(pcmPos < pcmFrames && n < maxFrames){
				int16 l = pcm[pcmPos*pcmChannels];
				int16 r = pcmChannels == 2 ? pcm[pcmPos*pcmChannels+1] : l;
				out[n*2] = l;
				out[n*2+1] = r;
				n++;
				pcmPos++;
			}
		}
		return n;
	}
};

static Decoder*
OpenDecoder(const char *filename)
{
	size_t len = strlen(filename);
	Decoder *d = nil;
	if(len > 4 && strcasecmp(filename + len - 4, ".wav") == 0)
		d = new WavDecoder(filename);
	else if(len > 4 && strcasecmp(filename + len - 4, ".mp3") == 0)
		d = new Mp3Decoder(filename);
	if(d && !d->IsOpened()){
		delete d;
		d = nil;
	}
	return d;
}

// ============================================================================
// Streams
// ============================================================================

#define RING_FRAMES	65536	// ~1.4 s at 48 kHz, 2 s at 32 kHz

struct Stream
{
	Decoder *dec;
	uint32 rate;
	int16 *ring;		// stereo frames
	volatile uint32 head, tail;	// frames written / read (counters)
	double frac;		// position between ring frames
	bool eof;
	bool playing;
	bool paused;
	uint32 startMs;
	uint64 consumed;	// frames
	uint32 lengthMs;
	int32 volume;		// 0..127, with the master volumes
	int32 pan;		// 0..127
};

static Stream *streams[MAX_STREAMS];

static Stream*
StreamCreate(const char *filename)
{
	Decoder *d = OpenDecoder(filename);
	if(d == nil){
		static int warned;
		if(warned++ < 10)
			PS3_Logf("[audio] can't open stream %s", filename);
		return nil;
	}
	Stream *s = new Stream;
	memset(s, 0, sizeof(*s));
	s->dec = d;
	s->rate = d->GetRate();
	s->lengthMs = d->GetLengthMS();
	s->ring = new int16[RING_FRAMES*2];
	s->volume = 0;
	s->pan = 63;
	return s;
}

static void
StreamDestroy(Stream *s)
{
	if(s == nil) return;
	delete s->dec;
	delete[] s->ring;
	delete s;
}

// ============================================================================
// MP3 player radio (sampman_oal's _FindMP3s & co.)
// ============================================================================

struct Mp3Entry
{
	char path[256];		// absolute
	uint32 lengthMs;
	uint32 streamPos;	// where it starts in the station, ms
};
static Mp3Entry *mp3List;
static uint32 curMp3Index;
static bool mp3Active;		// stream 0 plays one of them (its end: the next one)

static int
CmpMp3(const void *a, const void *b)
{
	return strcasecmp(((const Mp3Entry*)a)->path, ((const Mp3Entry*)b)->path);
}

static void
FindMP3s(void)
{
	nNumMP3s = 0;
	mp3Active = false;
	char *dir = casepath(PS3_USRDIR "/mp3");
	if(dir == nil)
		return;
	DIR *d = opendir(dir);
	if(d == nil){
		free(dir);
		return;
	}
	uint64 t0 = sysGetSystemTime();
	uint32 cap = 0, skipped = 0;
	struct dirent *e;
	while((e = readdir(d)) != nil && nNumMP3s < 2000){
		size_t n = strlen(e->d_name);
		if(n < 5 || strcasecmp(e->d_name + n - 4, ".mp3") != 0)
			continue;
		char path[256];
		if(snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)){
			skipped++;
			continue;
		}
		Mp3Decoder *dec = new Mp3Decoder(path);
		uint32 len = dec->IsOpened() ? dec->GetLengthMS() : 0;
		delete dec;
		if(len == 0){
			if(skipped++ < 10)
				PS3_Logf("[audio] mp3/%s: not a valid MP3, skipped", e->d_name);
			continue;
		}
		if(nNumMP3s >= cap){
			uint32 ncap = cap ? cap*2 : 64;
			Mp3Entry *nl = (Mp3Entry*)realloc(mp3List, ncap*sizeof(Mp3Entry));
			if(nl == nil)
				break;
			mp3List = nl;
			cap = ncap;
		}
		Mp3Entry &m = mp3List[nNumMP3s++];
		strcpy(m.path, path);
		m.lengthMs = len;
	}
	closedir(d);
	free(dir);
	if(nNumMP3s == 0){
		if(skipped)
			PS3_Logf("[audio] MP3 player: no playable file in mp3/ (%u skipped)", skipped);
		return;
	}
	qsort(mp3List, nNumMP3s, sizeof(Mp3Entry), CmpMp3);
	uint32 total = 0;
	for(uint32 i = 0; i < nNumMP3s; i++){
		mp3List[i].streamPos = total;
		total += mp3List[i].lengthMs;
	}
	nStreamLength[STREAMED_SOUND_RADIO_MP3_PLAYER] = total;
	PS3_Logf("[audio] MP3 player: %u files, %u min (%u skipped), scanned in %.1f s",
	         nNumMP3s, total/60000, skipped, (sysGetSystemTime() - t0)/1000000.0);
}

static void
FreeMP3s(void)
{
	free(mp3List);
	mp3List = nil;
	nNumMP3s = 0;
	mp3Active = false;
}

// ============================================================================
// Audio port and mixer thread
// ============================================================================

static bool audioOpen;
static volatile bool s_audioQuit;
static u32 audioPort;
static audioPortConfig audioCfg;
static sys_event_queue_t audioQueue;
static sys_ipc_key_t audioKey;
static sys_ppu_thread_t audioTid, streamTid;
static u32 audioLate;
static uint8 monoMode;

static float mixBuf[AUDIO_BLOCK_SAMPLES*OUT_CHANNELS];

static void
MixChannel(Channel &c)
{
	if(c.data == nil || c.length == 0 || c.freq == 0)
		return;
	// gains
	float gl, gr;
	float vol = c.volume / 127.0f;
	if(c.is3D){
		float d = sqrtf(c.x*c.x + c.y*c.y + c.z*c.z);
		float ref = c.minDist > 0.0f ? c.minDist : 1.0f;
		float dist = d;
		if(dist < ref) dist = ref;
		if(c.maxDist > 0.0f && dist > c.maxDist) dist = c.maxDist;
		vol *= ref / dist;
		float p = d > 0.0001f ? c.x / d : 0.0f;
		if(p < -1.0f) p = -1.0f;
		if(p > 1.0f) p = 1.0f;
		gl = sqrtf(0.5f*(1.0f - p));
		gr = sqrtf(0.5f*(1.0f + p));
	}else{
		float p = (c.pan - 63) / 64.0f;
		if(p < -1.0f) p = -1.0f;
		if(p > 1.0f) p = 1.0f;
		gl = sqrtf(0.5f*(1.0f - p));
		gr = sqrtf(0.5f*(1.0f + p));
	}
	if(monoMode)
		gl = gr = 0.7071f;
	gl *= vol * (1.0f/32768.0f);
	gr *= vol * (1.0f/32768.0f);

	double step = (double)c.freq / OUT_RATE;
	double pos = c.pos;
	const int16 *data = c.data;
	int32 len = c.length;
	float *out = mixBuf;
	for(int i = 0; i < AUDIO_BLOCK_SAMPLES; i++){
		int32 end = (c.loopEnd < 0 || c.loopEnd > len) ? len : c.loopEnd;
		bool looping = c.loopCount == 0 || c.timesPlayed + 1 < c.loopCount;
		if(!looping)
			end = len;
		if(pos >= end){
			if(looping && end > c.loopStart){
				pos -= end - c.loopStart;
				if(pos < c.loopStart) pos = c.loopStart;
				c.timesPlayed++;
			}else{
				c.playing = false;
				break;
			}
		}
		int32 i0 = (int32)pos;
		float t = (float)(pos - i0);
		int32 i1 = i0 + 1 < len ? i0 + 1 : i0;
		float s = data[i0] + (data[i1] - data[i0]) * t;
		out[i*2] += s * gl;
		out[i*2+1] += s * gr;
		pos += step;
	}
	c.pos = pos;
}

static void
MixStream(Stream &s)
{
	if(!s.playing || s.paused || s.ring == nil)
		return;
	float vol = s.volume / 127.0f * (1.0f/32768.0f);
	float gl = s.pan <= 63 ? 1.0f : (127 - s.pan) / 64.0f;
	float gr = s.pan >= 63 ? 1.0f : s.pan / 63.0f;
	gl *= vol;
	gr *= vol;
	double step = (double)s.rate / OUT_RATE;
	float *out = mixBuf;
	for(int i = 0; i < AUDIO_BLOCK_SAMPLES; i++){
		uint32 avail = s.head - s.tail;
		if(avail < 2)
			break;	// underrun (or the end): silence, no advance
		const int16 *a = &s.ring[(s.tail % RING_FRAMES)*2];
		const int16 *b = &s.ring[((s.tail+1) % RING_FRAMES)*2];
		float t = (float)s.frac;
		float l = a[0] + (b[0] - a[0]) * t;
		float r = a[1] + (b[1] - a[1]) * t;
		if(monoMode)
			l = r = (l + r) * 0.5f;
		out[i*2] += l * gl;
		out[i*2+1] += r * gr;
		s.frac += step;
		uint32 adv = (uint32)s.frac;
		s.frac -= adv;
		s.tail += adv;
		s.consumed += adv;
	}
}

static float*
AudioBlock(u32 index)
{
	return (float*)(u64)audioCfg.audioDataStart +
		(u64)index * AUDIO_BLOCK_SAMPLES * audioCfg.channelCount;
}

static void
AudioFill(float *dst)
{
	memset(mixBuf, 0, sizeof(mixBuf));
	{
		MixLock lock;
		for(int i = 0; i < MAXCHANNELS+MAX2DCHANNELS; i++)
			if(channels[i].playing)
				MixChannel(channels[i]);
		for(int i = 0; i < MAX_STREAMS; i++)
			if(streams[i])
				MixStream(*streams[i]);
	}
	for(int i = 0; i < AUDIO_BLOCK_SAMPLES*OUT_CHANNELS; i++){
		float v = mixBuf[i];
		if(v > 1.0f) v = 1.0f;
		if(v < -1.0f) v = -1.0f;
		dst[i] = v;
	}
}

static void
AudioThread(void *arg)
{
	u32 blocks = (u32)audioCfg.numBlocks;
	u32 next = 1;
	sys_event_t ev;
	(void)arg;
	while(!s_audioQuit){
		u32 playing, dist;
		// one event per block played; the timeout only matters for quitting
		sysEventQueueReceive(audioQueue, &ev, 20 * 1000);
		if(s_audioQuit)
			break;
		playing = (u32)(*(volatile u64*)(u64)audioCfg.readIndex % blocks);
		dist = (next + blocks - playing) % blocks;
		if(dist == 0 || dist > AUDIO_AHEAD + 1){
			if(dist == 0)
				audioLate++;
			next = (playing + 1) % blocks;
			dist = 1;
		}
		while(dist <= AUDIO_AHEAD){
			AudioFill(AudioBlock(next));
			next = (next + 1) % blocks;
			dist++;
		}
	}
	sysThreadExit(0);
}

// Keeps every stream's ring filled up (file reading and decoding happen here,
// never in the mixer)
static void
StreamThread(void *arg)
{
	static int16 tmp[4096*2];
	(void)arg;
	while(!s_audioQuit){
		{
			StreamLock slock;
			for(int i = 0; i < MAX_STREAMS; i++){
				Stream *s = streams[i];
				if(s == nil || s->eof)
					continue;
				for(int pass = 0; pass < 8; pass++){
					uint32 used = s->head - s->tail;
					uint32 space = RING_FRAMES - 1 - used;
					if(space < 4096)
						break;
					uint32 n = s->dec->Decode(tmp, 4096);
					if(n == 0){
						s->eof = true;
						break;
					}
					MixLock lock;
					for(uint32 k = 0; k < n; k++){
						uint32 at = ((s->head + k) % RING_FRAMES)*2;
						s->ring[at] = tmp[k*2];
						s->ring[at+1] = tmp[k*2+1];
					}
					__asm__ volatile("lwsync" ::: "memory");
					s->head += n;
				}
			}
		}
		usleep(8000);
	}
	sysThreadExit(0);
}

static bool
AudioPortInit(void)
{
	audioPortParam param;
	s32 r;

	r = audioInit();
	if(r != 0){
		PS3_Logf("[audio] audioInit failed (%d)", (int)r);
		return false;
	}
	memset(&param, 0, sizeof(param));
	param.numChannels = AUDIO_PORT_2CH;
	param.numBlocks = AUDIO_BLOCK_16;
	param.attrib = 0;
	param.level = 1.0f;
	r = audioPortOpen(&param, &audioPort);
	if(r != 0){
		PS3_Logf("[audio] audioPortOpen failed (%d)", (int)r);
		audioQuit();
		return false;
	}
	audioGetPortConfig(audioPort, &audioCfg);
	audioCreateNotifyEventQueue(&audioQueue, &audioKey);
	audioSetNotifyEventQueue(audioKey);
	sysEventQueueDrain(audioQueue);
	memset((void*)(u64)audioCfg.audioDataStart, 0, audioCfg.portSize);

	s_audioQuit = false;
	audioLate = 0;
	audioPortStart(audioPort);

	if(sysThreadCreate(&audioTid, AudioThread, NULL, 200, 64*1024, THREAD_JOINABLE, (char*)"re3audio") != 0){
		PS3_Log("[audio] sysThreadCreate failed (mixer)");
		audioPortStop(audioPort);
		audioRemoveNotifyEventQueue(audioKey);
		audioPortClose(audioPort);
		sysEventQueueDestroy(audioQueue, 0);
		audioQuit();
		return false;
	}
	if(sysThreadCreate(&streamTid, StreamThread, NULL, 900, 128*1024, THREAD_JOINABLE, (char*)"re3stream") != 0){
		PS3_Log("[audio] sysThreadCreate failed (streams)");
	}
	audioOpen = true;
	PS3_Logf("[audio] port %u open: %u blocks, %u channels, %d Hz",
	         (unsigned)audioPort, (unsigned)audioCfg.numBlocks, (unsigned)audioCfg.channelCount, OUT_RATE);
	return true;
}

static void
AudioPortShutdown(void)
{
	u64 rv;
	if(!audioOpen)
		return;
	s_audioQuit = true;
	sysThreadJoin(audioTid, &rv);
	sysThreadJoin(streamTid, &rv);
	audioPortStop(audioPort);
	audioRemoveNotifyEventQueue(audioKey);
	audioPortClose(audioPort);
	sysEventQueueDestroy(audioQueue, 0);
	audioQuit();
	audioOpen = false;
	PS3_Logf("[audio] closed (the mixer fell behind %u times)", (unsigned)audioLate);
}

// ============================================================================
// cSampleManager
// ============================================================================

cSampleManager::cSampleManager(void)
{
}

cSampleManager::~cSampleManager(void)
{
}

void cSampleManager::SetSpeakerConfig(int32 nConfig) { (void)nConfig; }
uint32 cSampleManager::GetMaximumSupportedChannels(void) { return MAXCHANNELS; }
uint32 cSampleManager::GetNum3DProvidersAvailable(void) { return 1; }
void cSampleManager::SetNum3DProvidersAvailable(uint32 num) { (void)num; }
char *cSampleManager::Get3DProviderName(uint8 id) { (void)id; return ProviderName; }
void cSampleManager::Set3DProviderName(uint8 id, char *name) { (void)id; (void)name; }
int8 cSampleManager::GetCurrent3DProviderIndex(void) { return 0; }
int8 cSampleManager::SetCurrent3DProvider(uint8 which) { (void)which; return 0; }
bool cSampleManager::IsMP3RadioChannelAvailable(void) { return nNumMP3s != 0; }
void cSampleManager::ReleaseDigitalHandle(void) { }
void cSampleManager::ReacquireDigitalHandle(void) { }
bool cSampleManager::CheckForAnAudioFileOnCD(void) { return true; }
char cSampleManager::GetCDAudioDriveLetter(void) { return '\0'; }

static bool
IsThisTrackAt16KHz(uint32 track)
{
	return track == STREAMED_SOUND_RADIO_CHAT;
}

// stream lengths: measured once (opening ~200 files), kept in
// audio/sound.cache (native endian: written and read only here)
static void
LoadStreamLengths(void)
{
	FILE *cache = fopen("audio/sound.cache", "rb");
	if(cache){
		uint32 magic = 0;
		if(fread(&magic, 4, 1, cache) == 1 && magic == 0x50533301 &&
		   fread(nStreamLength, sizeof(uint32), TOTAL_STREAMED_SOUNDS, cache) == TOTAL_STREAMED_SOUNDS){
			fclose(cache);
			PS3_Log("[audio] stream lengths from audio/sound.cache");
			return;
		}
		fclose(cache);
	}
	int found = 0;
	for(int32 i = 0; i < TOTAL_STREAMED_SOUNDS; i++){
		Decoder *d = OpenDecoder(StreamedNameTable[i]);
		if(d){
			nStreamLength[i] = d->GetLengthMS();
			delete d;
			found++;
		}else
			nStreamLength[i] = 0;
	}
	PS3_Logf("[audio] %d of %d streamed files found", found, TOTAL_STREAMED_SOUNDS);
	cache = fopen("audio/sound.cache", "wb");
	if(cache){
		uint32 magic = 0x50533301;
		fwrite(&magic, 4, 1, cache);
		fwrite(nStreamLength, sizeof(uint32), TOTAL_STREAMED_SOUNDS, cache);
		fclose(cache);
	}
}

bool
cSampleManager::Initialise(void)
{
	if(_bSampmanInitialised)
		return true;

	CreateMutex(&mixMutex, "re3mix");
	CreateMutex(&streamMutex, "re3strm");

	for(int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++){
		m_aSamples[i].nOffset = 0;
		m_aSamples[i].nSize = 0;
		m_aSamples[i].nFrequency = MAX_FREQ;
		m_aSamples[i].nLoopStart = 0;
		m_aSamples[i].nLoopEnd = -1;
	}
	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = 0;
	monoMode = 0;

	for(int32 i = 0; i < MAX_SFX_BANKS; i++){
		bSampleBankLoaded[i] = false;
		nSampleBankDiscStartOffset[i] = 0;
		nSampleBankSize[i] = 0;
		nSampleBankMemoryStartAddress[i] = 0;
	}
	for(int32 i = 0; i < MAX_PEDSFX; i++)
		nPedSlotSfx[i] = NO_SAMPLE;
	nCurrentPedSlot = 0;
	for(int32 i = 0; i < MAXCHANNELS+MAX2DCHANNELS; i++){
		nChannelVolume[i] = 0;
		memset(&channels[i], 0, sizeof(Channel));
	}
	for(int32 i = 0; i < MAX_STREAMS; i++){
		streams[i] = nil;
		nStreamVolume[i] = 100;
		nStreamPan[i] = 63;
	}

	LoadStreamLengths();

	if(!InitialiseSampleBanks()){
		PS3_Log("[audio] FATAL: audio/sfx.sdt or audio/sfx.raw missing");
		Terminate();
		return false;
	}

	nSampleBankMemoryStartAddress[SFX_BANK_0] = (uintptr)malloc(nSampleBankSize[SFX_BANK_0]);
	if(nSampleBankMemoryStartAddress[SFX_BANK_0] == 0){
		PS3_Logf("[audio] FATAL: no memory for the sound bank (%d KB)", nSampleBankSize[SFX_BANK_0]/1024);
		Terminate();
		return false;
	}
	nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = (uintptr)malloc(PED_BLOCKSIZE*MAX_PEDSFX);

	if(!LoadSampleBank(SFX_BANK_0))
		PS3_Log("[audio] WARNING: the sound bank didn't load");
	PS3_Logf("[audio] sound bank %d KB, ped comments %d KB", nSampleBankSize[SFX_BANK_0]/1024,
	         nSampleBankSize[SFX_BANK_PED_COMMENTS]/1024);

	if(!AudioPortInit()){
		Terminate();
		return false;
	}

	_bSampmanInitialised = true;
	FindMP3s();
	return true;
}

void
cSampleManager::Terminate(void)
{
	AudioPortShutdown();
	{
		StreamLock slock;
		MixLock lock;
		for(int32 i = 0; i < MAX_STREAMS; i++){
			StreamDestroy(streams[i]);
			streams[i] = nil;
		}
		for(int32 i = 0; i < MAXCHANNELS+MAX2DCHANNELS; i++)
			channels[i].playing = false;
	}
	if(fpSampleDataHandle){
		fclose(fpSampleDataHandle);
		fpSampleDataHandle = nil;
	}
	if(nSampleBankMemoryStartAddress[SFX_BANK_0]){
		free((void*)nSampleBankMemoryStartAddress[SFX_BANK_0]);
		nSampleBankMemoryStartAddress[SFX_BANK_0] = 0;
	}
	if(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS]){
		free((void*)nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS]);
		nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] = 0;
	}
	FreeMP3s();
	_bSampmanInitialised = false;
}

static inline int32
MasterVolume(uint8 fade, uint32 vol, uint8 master)
{
	return fade*vol*master >> 14;
}

void
cSampleManager::UpdateEffectsVolume(void)
{
	if(!_bSampmanInitialised)
		return;
	MixLock lock;
	for(int32 i = 0; i < MAXCHANNELS+MAX2DCHANNELS; i++)
		if(channels[i].playing && nChannelVolume[i] != 0)
			channels[i].volume = MasterVolume(m_nEffectsFadeVolume, nChannelVolume[i], m_nEffectsVolume);
}

void cSampleManager::SetEffectsMasterVolume(uint8 nVolume) { m_nEffectsVolume = nVolume; UpdateEffectsVolume(); }
void cSampleManager::SetMusicMasterVolume(uint8 nVolume) { m_nMusicVolume = nVolume; }
void cSampleManager::SetEffectsFadeVolume(uint8 nVolume) { m_nEffectsFadeVolume = nVolume; UpdateEffectsVolume(); }
void cSampleManager::SetMusicFadeVolume(uint8 nVolume) { m_nMusicFadeVolume = nVolume; }
void cSampleManager::SetMonoMode(uint8 nMode) { m_nMonoMode = nMode; monoMode = nMode; }

bool
cSampleManager::LoadSampleBank(uint8 nBank)
{
	if(CTimer::GetIsCodePaused())
		return false;
	if(MusicManager.IsInitialised() && MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE && nBank != SFX_BANK_0)
		return false;
	if(fpSampleDataHandle == nil || nSampleBankMemoryStartAddress[nBank] == 0)
		return false;
	if(fseek(fpSampleDataHandle, nSampleBankDiscStartOffset[nBank], SEEK_SET) != 0)
		return false;
	if(fread((void*)nSampleBankMemoryStartAddress[nBank], 1, nSampleBankSize[nBank], fpSampleDataHandle) != (size_t)nSampleBankSize[nBank])
		return false;
	// sfx.raw is little-endian
	LittleSwap16((void*)nSampleBankMemoryStartAddress[nBank], nSampleBankSize[nBank]/2);
	bSampleBankLoaded[nBank] = true;
	return true;
}

void cSampleManager::UnloadSampleBank(uint8 nBank) { bSampleBankLoaded[nBank] = false; }
bool cSampleManager::IsSampleBankLoaded(uint8 nBank) { return bSampleBankLoaded[nBank]; }

bool
cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	return _GetPedCommentSlot(nComment) >= 0;
}

int32
cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	int8 slot;
	for(int32 i = 0; i < 3; i++){
		slot = nCurrentPedSlot - i - 1;
		if(slot < 0)
			slot += ARRAY_SIZE(nPedSlotSfx);
		if((int32)nComment == nPedSlotSfx[slot])
			return slot;
	}
	return -1;
}

bool
cSampleManager::LoadPedComment(uint32 nComment)
{
	if(CTimer::GetIsCodePaused())
		return false;
	if(MusicManager.IsInitialised()){
		switch(MusicManager.GetMusicMode()){
		case MUSICMODE_CUTSCENE:
			return false;
		case MUSICMODE_FRONTEND:
			if(MusicManager.GetNextTrack() == STREAMED_SOUND_GAME_COMPLETED)
				return false;
			break;
		}
	}
	if(fpSampleDataHandle == nil || nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] == 0)
		return false;
	uint32 size = m_aSamples[nComment].nSize;
	if(size > PED_BLOCKSIZE)
		size = PED_BLOCKSIZE;
	{
		// the slot may be playing on a channel: stop those first
		MixLock lock;
		uint8 *slotStart = (uint8*)(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE*nCurrentPedSlot);
		for(int32 i = 0; i < MAXCHANNELS+MAX2DCHANNELS; i++)
			if(channels[i].playing && (const uint8*)channels[i].data >= slotStart &&
			   (const uint8*)channels[i].data < slotStart + PED_BLOCKSIZE)
				channels[i].playing = false;
	}
	if(fseek(fpSampleDataHandle, m_aSamples[nComment].nOffset, SEEK_SET) != 0)
		return false;
	void *dst = (void*)(nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE*nCurrentPedSlot);
	if(fread(dst, 1, size, fpSampleDataHandle) != size)
		return false;
	LittleSwap16(dst, size/2);
	nPedSlotSfx[nCurrentPedSlot] = nComment;
	if(++nCurrentPedSlot >= MAX_PEDSFX)
		nCurrentPedSlot = 0;
	return true;
}

int32
cSampleManager::GetBankContainingSound(uint32 offset)
{
	if(offset >= BankStartOffset[SFX_BANK_PED_COMMENTS])
		return SFX_BANK_PED_COMMENTS;
	if(offset >= BankStartOffset[SFX_BANK_0])
		return SFX_BANK_0;
	return INVALID_SFX_BANK;
}

int32 cSampleManager::GetSampleBaseFrequency(uint32 nSample) { return m_aSamples[nSample].nFrequency; }
int32 cSampleManager::GetSampleLoopStartOffset(uint32 nSample) { return m_aSamples[nSample].nLoopStart; }
int32 cSampleManager::GetSampleLoopEndOffset(uint32 nSample) { return m_aSamples[nSample].nLoopEnd; }
uint32 cSampleManager::GetSampleLength(uint32 nSample) { return m_aSamples[nSample].nSize / sizeof(uint16); }

bool cSampleManager::UpdateReverb(void) { return false; }
void cSampleManager::SetChannelReverbFlag(uint32 nChannel, uint8 nReverbFlag) { (void)nChannel; (void)nReverbFlag; }

bool
cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	uintptr addr;
	if(nChannel >= MAXCHANNELS+MAX2DCHANNELS || nSfx >= TOTAL_AUDIO_SAMPLES)
		return false;
	if(nSfx < SAMPLEBANK_MAX){
		if(!IsSampleBankLoaded(nBank))
			return false;
		addr = nSampleBankMemoryStartAddress[nBank] + m_aSamples[nSfx].nOffset - m_aSamples[BankStartOffset[nBank]].nOffset;
	}else{
		int32 slot = _GetPedCommentSlot(nSfx);
		if(slot < 0)
			return false;
		addr = nSampleBankMemoryStartAddress[SFX_BANK_PED_COMMENTS] + PED_BLOCKSIZE * slot;
	}
	MixLock lock;
	Channel &c = channels[nChannel];
	c.playing = false;
	c.data = (const int16*)addr;
	c.length = m_aSamples[nSfx].nSize / 2;
	if(nSfx >= SAMPLEBANK_MAX && c.length > PED_BLOCKSIZE/2)
		c.length = PED_BLOCKSIZE/2;
	c.baseFreq = m_aSamples[nSfx].nFrequency;
	c.freq = c.baseFreq;
	c.pos = 0.0;
	c.loopStart = 0;
	c.loopEnd = -1;
	c.loopCount = 1;
	c.timesPlayed = 0;
	c.is3D = nChannel != CHANNEL2D;
	c.x = c.y = c.z = 0.0f;
	c.maxDist = c.minDist = 0.0f;
	c.pan = 63;
	return true;
}

static bool
CutsceneVolumeReduction(void)
{
	// sampman_oal: quieter effects while JB.MP3 or S4_BDBD.MP3 play
	return MusicManager.GetMusicMode() == MUSICMODE_CUTSCENE &&
	       MusicManager.GetNextTrack() != STREAMED_SOUND_NEWS_INTRO &&
	       MusicManager.GetNextTrack() != STREAMED_SOUND_CUTSCENE_SAL4_BDBD;
}

void
cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	uint32 vol = nVolume > MAX_VOLUME ? MAX_VOLUME : nVolume;
	nChannelVolume[nChannel] = vol;
	if(CutsceneVolumeReduction())
		nChannelVolume[nChannel] = vol / 4;
	MixLock lock;
	channels[nChannel].volume = MasterVolume(m_nEffectsFadeVolume, nChannelVolume[nChannel], m_nEffectsVolume);
}

void
cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	MixLock lock;
	// as sampman_oal hands it to OpenAL: x mirrored, then x = right
	channels[nChannel].x = -fX;
	channels[nChannel].y = fY;
	channels[nChannel].z = fZ;
}

void
cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	MixLock lock;
	channels[nChannel].maxDist = fMax;
	channels[nChannel].minDist = fMin;
}

void
cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	if(nChannel != CHANNEL2D)
		return;
	uint32 vol = nVolume > MAX_VOLUME ? MAX_VOLUME : nVolume;
	nChannelVolume[nChannel] = vol;
	if(CutsceneVolumeReduction())
		nChannelVolume[nChannel] = vol / 4;
	MixLock lock;
	channels[nChannel].volume = MasterVolume(m_nEffectsFadeVolume, vol, m_nEffectsVolume);
}

void
cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	if(nChannel != CHANNEL2D)
		return;
	MixLock lock;
	channels[nChannel].pan = nPan > 127 ? 127 : nPan;
}

void
cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	MixLock lock;
	channels[nChannel].freq = nFreq;
}

void
cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	MixLock lock;
	channels[nChannel].loopStart = nLoopStart / (DIGITALBITS / 8);
	channels[nChannel].loopEnd = nLoopEnd < 0 ? -1 : nLoopEnd / (DIGITALBITS / 8);
}

void
cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	MixLock lock;
	channels[nChannel].loopCount = nLoopCount;
}

bool
cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	return channels[nChannel].playing;
}

void
cSampleManager::StartChannel(uint32 nChannel)
{
	MixLock lock;
	Channel &c = channels[nChannel];
	if(c.data == nil)
		return;
	c.pos = 0.0;
	c.timesPlayed = 0;
	c.playing = true;
}

void
cSampleManager::StopChannel(uint32 nChannel)
{
	MixLock lock;
	channels[nChannel].playing = false;
}

// ---- streams --------------------------------------------------------------------

static void
ReplaceStream(uint8 nStream, Stream *s)
{
	Stream *old;
	{
		StreamLock slock;
		MixLock lock;
		old = streams[nStream];
		streams[nStream] = s;
	}
	StreamDestroy(old);
}

static void
ApplyStreamVolume(cSampleManager *sm, uint8 nStream, uint8 nEffectFlag, uint8 fxFade, uint8 fxVol, uint8 musFade, uint8 musVol)
{
	Stream *s = streams[nStream];
	if(s == nil)
		return;
	if(nEffectFlag)
		s->volume = MasterVolume(fxFade, nStreamVolume[nStream], fxVol);
	else
		s->volume = MasterVolume(musFade, nStreamVolume[nStream], musVol);
	s->pan = nStreamPan[nStream];
}

void
cSampleManager::PreloadStreamedFile(uint8 nFile, uint8 nStream)
{
	if(nFile >= TOTAL_STREAMED_SOUNDS || nStream >= MAX_STREAMS)
		return;
	Stream *s = StreamCreate(StreamedNameTable[nFile]);
	if(s){
		s->volume = 0;
		s->pan = 63;
	}
	if(nStream == 0)
		mp3Active = false;
	ReplaceStream(nStream, s);
}

void
cSampleManager::PauseStream(uint8 nPauseFlag, uint8 nStream)
{
	MixLock lock;
	if(nStream < MAX_STREAMS && streams[nStream])
		streams[nStream]->paused = nPauseFlag != 0;
}

void
cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	MixLock lock;
	if(nStream < MAX_STREAMS && streams[nStream])
		streams[nStream]->playing = true;
}

bool
cSampleManager::StartStreamedFile(uint8 nFile, uint32 nPos, uint8 nStream)
{
	if(nFile >= TOTAL_STREAMED_SOUNDS || nStream >= MAX_STREAMS)
		return false;
	if(nFile == STREAMED_SOUND_RADIO_MP3_PLAYER){
		// sampman_oal: started again while one plays (it ended) = the next
		// file; else the file and position of nPos in the station
		for(uint32 tries = 0; tries < nNumMP3s; tries++){
			uint32 pos = 0;
			if(mp3Active || tries > 0)
				curMp3Index = (curMp3Index + 1) % nNumMP3s;
			else{
				uint32 p = nPos < nStreamLength[STREAMED_SOUND_RADIO_MP3_PLAYER] ? nPos : 0;
				uint32 i = 0;
				while(i + 1 < nNumMP3s && p >= mp3List[i+1].streamPos)
					i++;
				curMp3Index = i;
				pos = p - mp3List[i].streamPos;
			}
			Stream *s = StreamCreate(mp3List[curMp3Index].path);
			if(s == nil){
				mp3Active = false;
				continue;
			}
			if(pos){
				if(s->lengthMs && pos >= s->lengthMs)
					pos = 0;
				s->dec->Seek(pos);
				s->startMs = pos;
			}
			s->volume = 0;
			s->pan = nStreamPan[nStream];
			s->playing = true;
			ReplaceStream(nStream, s);
			mp3Active = nStream == 0;
			return true;
		}
		// none plays: the first station, as sampman_oal
		mp3Active = false;
		nFile = 0;
		nPos = 0;
	}
	if(nStream == 0)
		mp3Active = false;
	Stream *s = StreamCreate(StreamedNameTable[nFile]);
	if(s == nil){
		ReplaceStream(nStream, nil);
		return false;
	}
	if(nPos){
		if(s->lengthMs && nPos >= s->lengthMs)
			nPos = 0;
		s->dec->Seek(nPos);
		s->startMs = nPos;
	}
	s->volume = 0;
	s->pan = nStreamPan[nStream];
	s->playing = true;
	ReplaceStream(nStream, s);
	return true;
}

void
cSampleManager::StopStreamedFile(uint8 nStream)
{
	if(nStream < MAX_STREAMS)
		ReplaceStream(nStream, nil);
	if(nStream == 0)
		mp3Active = false;
}

int32
cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	MixLock lock;
	Stream *s = nStream < MAX_STREAMS ? streams[nStream] : nil;
	if(s == nil || s->rate == 0)
		return 0;
	uint32 pos = s->startMs + (uint32)(s->consumed * 1000 / s->rate);
	if(nStream == 0 && mp3Active && curMp3Index < nNumMP3s)
		pos += mp3List[curMp3Index].streamPos;	// the station's position
	return pos;
}

void
cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, uint8 nEffectFlag, uint8 nStream)
{
	if(nStream >= MAX_STREAMS)
		return;
	if(nVolume > MAX_VOLUME) nVolume = MAX_VOLUME;
	if(nPan > MAX_VOLUME) nPan = MAX_VOLUME;
	nStreamVolume[nStream] = nVolume;
	nStreamPan[nStream] = nPan;
	MixLock lock;
	ApplyStreamVolume(this, nStream, nEffectFlag, m_nEffectsFadeVolume, m_nEffectsVolume,
	                  m_nMusicFadeVolume, m_nMusicVolume);
}

int32
cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	return nStream < TOTAL_STREAMED_SOUNDS ? nStreamLength[nStream] : 0;
}

bool
cSampleManager::IsStreamPlaying(uint8 nStream)
{
	MixLock lock;
	Stream *s = nStream < MAX_STREAMS ? streams[nStream] : nil;
	if(s == nil || !s->playing)
		return false;
	if(s->eof && s->head - s->tail < 2)
		return false;
	return true;
}

void
cSampleManager::Service(void)
{
	// the streams have their own thread
}

bool
cSampleManager::InitialiseSampleBanks(void)
{
	int32 nBank = SFX_BANK_0;

	fpSampleDescHandle = fopen(SampleBankDescFilename, "rb");
	if(fpSampleDescHandle == nil)
		return false;
	fpSampleDataHandle = fopen(SampleBankDataFilename, "rb");
	if(fpSampleDataHandle == nil){
		fclose(fpSampleDescHandle);
		fpSampleDescHandle = nil;
		return false;
	}
	fseek(fpSampleDataHandle, 0, SEEK_END);
	int32 _nSampleDataEndOffset = ftell(fpSampleDataHandle);
	rewind(fpSampleDataHandle);

	fread(m_aSamples, sizeof(tSample), TOTAL_AUDIO_SAMPLES, fpSampleDescHandle);
	fclose(fpSampleDescHandle);
	fpSampleDescHandle = nil;
	// sfx.sdt is little-endian: 5 x 32 bit per sample
	LittleSwap32(m_aSamples, TOTAL_AUDIO_SAMPLES * sizeof(tSample) / 4);

	for(int32 i = 0; i < TOTAL_AUDIO_SAMPLES; i++){
		if(nBank >= MAX_SFX_BANKS)
			break;
		if(BankStartOffset[nBank] == BankStartOffset[SFX_BANK_0] + i){
			nSampleBankDiscStartOffset[nBank] = m_aSamples[i].nOffset;
			nBank++;
		}
	}

	nSampleBankSize[SFX_BANK_0] = nSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS] - nSampleBankDiscStartOffset[SFX_BANK_0];
	nSampleBankSize[SFX_BANK_PED_COMMENTS] = _nSampleDataEndOffset - nSampleBankDiscStartOffset[SFX_BANK_PED_COMMENTS];
	return true;
}

#endif
