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
void PS3_PadRumble(int large);	// large motor 0..255 (0 = still)
float PS3_PadDeadzone(bool right);	// stick dead zone, 0.05..0.5 (Gamepad settings)

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

// ps3_carspawn.cpp (L3 + R3)
void PS3_CarSpawnerPad(void);		// CPad::UpdatePads, after pad 0
void PS3_CarSpawnerDraw(void);		// CHud::Draw, after the subtitles
#endif
