// ps3_prof.cpp -- where the CPU time of a frame goes: main.cpp's timebar
// timers (CGame::Process, CnstrRenderList, RenderScene, Render2dStuff...),
// averaged per frame and logged every 10 seconds next to librw's [rsx] line.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/systime.h>

#include "ps3_platform.h"

// the timers are also crash breadcrumbs (1 = started, 2 = done): main.cpp's
// frame phases after CGame::Process (audio, render list, scene, 2D, present)
// had none. The names are string literals, as PS3_Crumb needs.
extern "C" void PS3_Crumb(const char *tag, int val);

#if PS3_STAGE != 1
// the same timers on the RSX (librw ps3video.cpp)
namespace rw { namespace ps3 {
void gpuMark(int32_t id);
void gpuStatsTake(uint64_t *ns, uint64_t *frameNs, uint32_t *samples);
} }
#define GPU_MARK(id) rw::ps3::gpuMark(id)
#define GPU_TIMERS 30
#else
#define GPU_MARK(id)
#endif

#define MAX_TIMERS	24
#define LOG_EVERY_US	10000000ull

struct ProfTimer
{
	const char *name;
	u64 start;
	u64 total;
};

static ProfTimer timers[MAX_TIMERS];
static int numTimers;
static u64 frameStart, frameTotal, lastLog;
static u32 frames;

static ProfTimer*
FindTimer(const char *name)
{
	for (int i = 0; i < numTimers; i++)
		if (timers[i].name == name || strcmp(timers[i].name, name) == 0)
			return &timers[i];
	if (numTimers >= MAX_TIMERS)
		return NULL;
	timers[numTimers].name = name;
	timers[numTimers].start = 0;
	timers[numTimers].total = 0;
	return &timers[numTimers++];
}

void
PS3_ProfStart(const char *name)
{
	PS3_Crumb(name, 1);
	ProfTimer *t = FindTimer(name);
	if (t) {
		t->start = sysGetSystemTime();
		GPU_MARK(2 * (int)(t - timers));
	}
}

void
PS3_ProfEnd(const char *name)
{
	PS3_Crumb(name, 2);
	ProfTimer *t = FindTimer(name);
	if (t)
		GPU_MARK(2 * (int)(t - timers) + 1);
	if (t && t->start) {
		t->total += sysGetSystemTime() - t->start;
		t->start = 0;
	}
}

// start of a frame (main.cpp's Idle)
void
PS3_ProfFrame(void)
{
	u64 now = sysGetSystemTime();
	if (frameStart)
		frameTotal += now - frameStart;
	frameStart = now;
	if (lastLog == 0) {
		lastLog = now;
		return;
	}
	frames++;
	if (now - lastLog < LOG_EVERY_US || frames == 0)
		return;

	char line[512];
	int n = snprintf(line, sizeof(line), "[prof] ms per frame: total %.1f |", frameTotal / 1000.0 / frames);
	for (int i = 0; i < numTimers && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - n, " %s %.2f", timers[i].name, timers[i].total / 1000.0 / frames);
		timers[i].total = 0;
	}
	PS3_Log(line);
#if PS3_STAGE != 1
	{
		uint64_t ns[GPU_TIMERS], frameNs;
		uint32_t samples;
		rw::ps3::gpuStatsTake(ns, &frameNs, &samples);
		if (samples) {
			n = snprintf(line, sizeof(line), "[gpu]  RSX ms per frame: total %.1f |", frameNs / 1e6 / samples);
			for (int i = 0; i < numTimers && i < GPU_TIMERS && n < (int)sizeof(line) - 40; i++)
				if (ns[i] / samples >= 10000)	// 0.01 ms
					n += snprintf(line + n, sizeof(line) - n, " %s %.2f", timers[i].name, ns[i] / 1e6 / samples);
			PS3_Log(line);
		}
	}
#endif
	frameTotal = 0;
	frames = 0;
	lastLog = now;
}
