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
#endif
