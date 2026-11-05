// ps3_platform.h -- re3-PS3 platform layer: paths, log, file system helpers.
#pragma once

#ifndef PS3_APPID
#define PS3_APPID "RE3PS3000"
#endif

// Everything lives in the game's USRDIR: the GTA III data (copied by the
// user), re3.ini, gta3.set, the saves (userfiles/) and the log.
#define PS3_USRDIR      "/dev_hdd0/game/" PS3_APPID "/USRDIR"
#define PS3_LOG_PATH     PS3_USRDIR "/re3-ps3.log"
#define PS3_LOG_OLD_PATH PS3_USRDIR "/re3-ps3.old.log"

#ifndef PS3_STAGE
#define PS3_STAGE 2
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ps3_log.cpp
void PS3_LogInit(void);
void PS3_LogShutdown(void);
void PS3_LogRaw(const char *msg);	// as-is
void PS3_Log(const char *msg);		// one line
void PS3_Logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void PS3_DumpCrumbs(void);
void PS3_StartWatchdog(void);	// [hang] dump when no frame for 10 s
void PS3_LogSync(void);	// log onto the disk (fsync)
void PS3_StopWatchdog(void);
void PS3_InstallCrashHandler(void);	// liblv2dbg PPU exception handler (if lv2 allows it)
extern unsigned PS3_crumbFrame;
extern int PS3_logsOn;	// USRDIR/logson.txt (else no log file, no reports; from res-PS3)

// ps3_fs.cpp
// Resolves a game path (relative to the emulated working directory, '\\'
// or '/', any letter case) to the real absolute path. Returns 0 if out
// is too small. The file does not have to exist (the case of the parts
// that do exist is fixed, the rest is kept).
int PS3_ResolvePath(const char *path, char *out, int outSize);
int PS3_chdir(const char *path);
char *PS3_getcwd(char *buf, int size);
int PS3_IsDir(const char *abspath);
int PS3_FileSize(const char *abspath, long long *size);

// ps3_mods.cpp (USRDIR/mods, like a PC mod loader)
const char *PS3_ModsOpenPath(const char *abspath);	// a loose file opened for reading: the mod file, or 0

// ps3_pad.cpp
unsigned PS3_PadButtons(void);	// raw DS3 buttons, bit n = GLFW_GAMEPAD_BUTTON_n

// ps3_movie.cpp: the PC intro movies, only with USRDIR/videointrotest.txt
// (from reVC-PS3-Optimised). Decoded on their own thread, drawn by the game's
int  PS3_MovieTestOn(void);
int  PS3_MovieStart(const char *path);
int  PS3_MovieSkipPressed(void);	// X, O or Start
void PS3_MovieRenderFrame(void);
int  PS3_MovieFinished(void);
void PS3_MovieCancel(void);
void PS3_MovieShutdown(void);
void PS3_PadRumble(int large);	// large motor 0..255 (0 = still)
float PS3_PadDeadzone(bool right);	// stick dead zone, 0.05..0.5 (Gamepad settings)
int PS3_PadPressure(int button);	// how hard a held button is pressed, 1..255 (game button id 1..16)

// ps3.cpp
unsigned PS3_FreeUserMemMB(void);
int PS3_Running(void);
void PS3_Pump(void);

void PS3_LogMemTag(const char *tag);	// heap, free memory, textures, streaming

#ifdef __cplusplus
}

// ps3_autotest.cpp (USRDIR/autotest.txt)
void PS3_AutotestFrame(void);		// CPad::UpdatePads, every frame
int  PS3_AutotestWantsLoad(void);	// main loop, between frames
void PS3_AutotestLoaded(void);		// after a load it asked for

// ps3_mods.cpp: .img entries replaced by mods/ files live in a virtual image
#define PS3_MODS_IMAGE (MAX_CDIMAGES - 1)
bool PS3_ModsImgEntry(const char *name, unsigned int *offset, unsigned int *size);	// .dir entry (with extension)
bool PS3_ModsRead(unsigned int sector, void *buffer, unsigned int sectors);	// CdStreamRead on PS3_MODS_IMAGE

// ps3_spu.cpp (from res-PS3, patch 50): an SPU program as a job server
// (signal 1 = job address, event back with the job's sequence number)
#include <stdint.h>
struct PS3SpuTask
{
	bool ready;
	uint8_t port;
	const char *name;
	uint32_t image[4];	// sysSpuImage
	uint32_t group, thread;
	uint64_t queue;
};
bool PS3_SpuTaskStart(PS3SpuTask *t, const void *elf, const char *name, uint8_t port);
bool PS3_SpuTaskKick(PS3SpuTask *t, uint32_t jobEa);
bool PS3_SpuTaskWait(PS3SpuTask *t, uint32_t seq, uint32_t timeoutUs, uint64_t *waitUs);	// false: gave up on it
void PS3_SpuTaskStop(PS3SpuTask *t);

// ps3_carspawn.cpp (L3 + R3)
void PS3_CarSpawnerPad(void);		// CPad::UpdatePads, after pad 0
void PS3_CarSpawnerDraw(void);		// CHud::Draw, after the subtitles
#endif
