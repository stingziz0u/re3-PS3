#pragma once

#ifdef TIMEBARS
void tbInit();
void tbStartTimer(int32, Const char*);
void tbEndTimer(Const char*);
void tbDisplay();
#elif defined(__PS3__)
// the PS3 port: the same timers measured and logged (ps3_prof.cpp)
void PS3_ProfFrame(void);
void PS3_ProfStart(const char *name);
void PS3_ProfEnd(const char *name);
#define tbInit() PS3_ProfFrame()
#define tbStartTimer(a, b) PS3_ProfStart(b)
#define tbEndTimer(a) PS3_ProfEnd(a)
#define tbDisplay()
#else
#define tbInit()
#define tbStartTimer(a, b)
#define tbEndTimer(a)
#define tbDisplay()
#endif
