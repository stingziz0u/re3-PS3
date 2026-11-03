// spumix.h -- from res-PS3 (patch 49): the audio mixer job shared by the PPU
// (src/audio/sampman_ps3.cpp) and the SPU program (ps3/spu/spumix.c).
//
// The PPU fills one SpuMixJob per audio block (256 frames): every playing
// sample channel and stream with its gains already worked out, positions in
// 32.32 fixed point. The SPU mixes them into 256 stereo floats (not clamped),
// puts them at outEa and writes the job back with the new positions, then
// sends an event. Both sides are big-endian; the layout is fixed by the
// types and the explicit padding (sizes checked on the PPU).

#ifndef SPUMIX_H
#define SPUMIX_H

#include <stdint.h>

#define SPUMIX_BLOCK		256	// frames per block (AUDIO_BLOCK_SAMPLES)
#define SPUMIX_MAX_CHANNELS	48
#define SPUMIX_MAX_STREAMS	4	// re3: 2 streams, reVC: 3 (res-PS3 had 2)

#define SPUMIX_ACTIVE		1	// in: mix it
#define SPUMIX_STOPPED		2	// out: the channel ended (or went outside its sample)

#define SPUMIX_SPUP		12	// SPU thread user event port
#define SPUMIX_QUIT		0	// the signal that ends the SPU program

typedef struct SpuMixChannel
{
	uint64_t ea;		// the int16 samples (2-byte aligned)
	int32_t len;		// samples
	int32_t ipos;		// position: ipos + fpos / 2^32
	uint32_t fpos;
	uint32_t istep;		// step per output frame: istep + fstep / 2^32
	uint32_t fstep;
	int32_t loopStart, loopEnd;	// loopEnd -1: the end
	int32_t loopCount;	// 0 forever
	int32_t timesPlayed;
	float gl, gr;		// gains, with the 1/32768
	uint32_t flags;
	uint32_t pad[2];
} __attribute__((aligned(16))) SpuMixChannel;	// 64 bytes

typedef struct SpuMixStream
{
	uint64_t ringEa;	// stereo int16 frames, 16-byte aligned
	uint32_t ringFrames;
	uint32_t head, tail;	// frame counters (wrap)
	uint32_t ffrac;		// position between tail and tail+1, / 2^32
	uint32_t istep, fstep;
	float gl, gr;
	uint32_t flags;
	uint32_t pad[1];
} __attribute__((aligned(16))) SpuMixStream;	// 48 bytes

typedef struct SpuMixJob
{
	uint64_t outEa;		// float[SPUMIX_BLOCK*2], 128-byte aligned
	uint32_t numChannels, numStreams;
	uint32_t mono;		// streams mixed to mono (the channels' gains already are)
	uint32_t seq;		// block number, given back in the event
	uint32_t pad[2];
	SpuMixChannel ch[SPUMIX_MAX_CHANNELS];
	SpuMixStream st[SPUMIX_MAX_STREAMS];
} __attribute__((aligned(128))) SpuMixJob;	// 3328 bytes (one DMA)

#endif
