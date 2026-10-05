// ps3.cpp -- re3 on the PS3: entry point, skeleton (ps*) functions, main loop.
//
// Boot steps are numbered in the log ([0]..[8]) so a black screen or a hang
// still says how far it got.
//
// STAGE 1 (make STAGE=1): headless. The whole game with the RSX backend's
// drawing stubbed out: loads the data, starts a new game and runs the game
// loop for a while (the intro, then a walk over the three islands), logging
// memory, streaming and the player position. Validates endianness, 64 bits
// and memory before any video work.
//
// STAGE 2 (make): the game. librw's PS3 backend renders with the RSX at the
// internal resolution of the video mode and scales the picture to the TV;
// the DualShock 3 is a GLFW gamepad (ps3_pad.cpp). No audio yet.

#include "common.h"
#include "crossplatform.h"
#include "Draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <ppu-types.h>
#include <ppu-asm.h>
#include <sysutil/sysutil.h>
#include <sys/thread.h>
#include <sys/process.h>
#include <sys/memory.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"

#include "main.h"
#include "FileMgr.h"
#include "Text.h"
#include "Pad.h"
#include "Timer.h"
#include "DMAudio.h"
#include "ControllerConfig.h"
#include "Frontend.h"
#include "Game.h"
#include "PCSave.h"
#include "MemoryCard.h"
#include "Sprite2d.h"
#include "AnimViewer.h"
#include "Font.h"
#include "MemoryMgr.h"
#include "Streaming.h"
#include "World.h"
#include "PlayerInfo.h"
#include "Pools.h"
#include "Script.h"
#include "Ped.h"
#include "Directory.h"
#include "Clock.h"
#include "Collision.h"
#include "CutsceneMgr.h"
#include "CdStream.h"
#include "frontendoption.h"

#include "ps3_platform.h"
#include "ps3_rev.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define PS3_GAME_STACK (4 * 1024 * 1024)

rw::EngineOpenParams openParams;

static RwBool ForegroundApp = TRUE;
static RwBool RwInitialised = FALSE;

static psGlobalType PsGlobal;
#define PSGLOBAL(var) (((psGlobalType *)(RsGlobal.ps))->var)

size_t _dwMemAvailPhys;
long _dwOperatingSystemVersion;
RwUInt32 gGameState;

// Output size the RSX renders to (stage 2 scales it to the TV): the video
// mode picks the internal resolution
#define PS3_SCREEN_WIDTH 1280
#define PS3_SCREEN_HEIGHT 720

static volatile int ps3_running = 1;

static void
ps3_sysutil_callback(u64 status, u64 param, void *userdata)
{
	(void)param; (void)userdata;
	if (status == SYSUTIL_EXIT_GAME) {
		PS3_Log("[8] XMB exit requested");
		ps3_running = 0;
		RsGlobal.quit = TRUE;
	}
}

extern "C" void
PS3_Pump(void)
{
	sysUtilCheckCallback();
}

extern "C" int
PS3_Running(void)
{
	return ps3_running;
}

extern "C" unsigned
PS3_FreeUserMemMB(void)
{
	unsigned lo = 1, hi = 256, best = 0;

	while (lo <= hi) {
		unsigned mid = (lo + hi) / 2;
		sys_mem_container_t c;

		if (sysMemContainerCreate(&c, (size_t)mid * 1024u * 1024u) == 0) {
			sysMemContainerDestroy(c);
			best = mid;
			lo = mid + 1;
		} else
			hi = mid - 1;
	}
	return best;
}

static void
PS3_LogMem(const char *tag)
{
	struct mallinfo mi = mallinfo();
	PS3_Logf("%s heap %.1f MB (in use %.1f MB), free user mem ~%u MB, "
	         "textures %d (%.1f MB, %d DXT), streaming %.1f MB",
	         tag, mi.arena / 1048576.0, mi.uordblks / 1048576.0, PS3_FreeUserMemMB(),
	         rw::ps3::rasterStats.numTextures, rw::ps3::rasterStats.textureBytes / 1048576.0,
	         rw::ps3::rasterStats.numDXT, CStreaming::ms_memoryUsed / 1048576.0);
}

extern "C" void
PS3_LogMemTag(const char *tag)
{
	PS3_LogMem(tag);
}

// ---- skeleton functions ---------------------------------------------------

void
_psCreateFolder(const char *path)
{
	char abs[1024];
	if (PS3_ResolvePath(path, abs, sizeof(abs)) && !PS3_IsDir(abs))
		mkdir(abs, 0755);
}

const char*
_psGetUserFilesFolder()
{
	static char szUserFiles[256];
	strcpy(szUserFiles, PS3_USRDIR "/userfiles");
	_psCreateFolder(szUserFiles);
	return szUserFiles;
}

RwBool
psCameraBeginUpdate(RwCamera *camera)
{
	if (!RwCameraBeginUpdate(Scene.camera)) {
		ForegroundApp = FALSE;
		RsEventHandler(rsACTIVATE, (void *)FALSE);
		return FALSE;
	}
	return TRUE;
}

void
psCameraShowRaster(RwCamera *camera)
{
#if PS3_STAGE != 1
	// a fixed aspect ratio in the options: the picture keeps that shape on
	// the TV (black bars), instead of being stretched over all of it
	rw::ps3::setPresentAspect(FrontEndMenuManager.m_PrefsUseWideScreen == AR_AUTO ? 0.0f : CDraw::FindAspectRatio());
	{
		extern void PS3_ApplyScreenFit(void);
		PS3_ApplyScreenFit();	// Display menu: SCREEN SIZE (re3.ini [Display] ScreenFit)
		extern int8 gPS3Fps60;
		rw::ps3::setFrameRate(gPS3Fps60 ? 60 : 30);	// Graphics menu: FRAME RATE
	}
#endif
	RwCameraShowRaster(camera, nil, CMenuManager::m_PrefsVsync ? rwRASTERFLIPWAITVSYNC : rwRASTERFLIPDONTWAIT);
}

RwImage*
psGrabScreen(RwCamera *pCamera)
{
	rw::Image *image = RwCameraGetRaster(pCamera)->toImage();
	if (image) {
		image->removeMask();
		return image;
	}
	return nil;
}

double
psTimer(void)
{
	// timebase: 79.8 MHz on retail consoles
	static u64 freq;
	if (freq == 0)
		freq = sysGetTimebaseFrequency();
	u64 tb = __gettime();
	return (double)tb * 1000.0 / (double)freq;
}

void
psMouseSetPos(RwV2d *pos)
{
	PSGLOBAL(lastMousePos.x) = (RwInt32)pos->x;
	PSGLOBAL(lastMousePos.y) = (RwInt32)pos->y;
}

RwMemoryFunctions*
psGetMemoryFunctions(void)
{
	return nil;
}

RwBool
psInstallFileSystem(void)
{
	return TRUE;
}

RwBool
psNativeTextureSupport(void)
{
	return TRUE;
}

RwBool
psInitialize(void)
{
	PsGlobal.window = nil;
	PsGlobal.fullScreen = TRUE;
	PsGlobal.lastMousePos.x = PsGlobal.lastMousePos.y = 0.0f;
	PsGlobal.mouseWheel = 0.0;
	PsGlobal.cursorIsInWindow = FALSE;
	PsGlobal.joy1id = -1;
	PsGlobal.joy2id = -1;
	RsGlobal.ps = &PsGlobal;

	CFileMgr::Initialise();

	PS3_Log("[5]   user files folder");
	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());

	PS3_Log("[5]   language + text (GXT)");
	InitialiseLanguage();

	gGameState = GS_START_UP;
	_dwOperatingSystemVersion = OS_WINXP;

	PS3_Log("[5]   settings (gta3.set, re3.ini)");
	FrontEndMenuManager.LoadSettings();

	_dwMemAvailPhys = (size_t)PS3_FreeUserMemMB() * 1024 * 1024;

	TheText.Unload();
#if PS3_STAGE != 1
	PS3_PadInit();
#endif
	PS3_Log("[5]   psInitialize done");
	return TRUE;
}

void
psTerminate(void)
{
}

static RwChar **_VMList;

RwInt32
_psGetNumVideModes()
{
	return RwEngineGetNumVideoModes();
}

RwBool
_psFreeVideoModeList()
{
	RwInt32 numModes = _psGetNumVideModes();
	if (_VMList == nil)
		return TRUE;
	for (RwInt32 i = 0; i < numModes; i++)
		RwFree(_VMList[i]);
	RwFree(_VMList);
	_VMList = nil;
	return TRUE;
}

RwChar**
_psGetVideoModeList()
{
	if (_VMList != nil)
		return _VMList;
	RwInt32 numModes = RwEngineGetNumVideoModes();
	_VMList = (RwChar **)RwCalloc(numModes, sizeof(RwChar*));
	for (RwInt32 i = 0; i < numModes; i++) {
		RwVideoMode vm;
		RwEngineGetVideoModeInfo(&vm, i);
		_VMList[i] = (RwChar*)RwCalloc(100, sizeof(RwChar));
		rwsprintf(_VMList[i], "%d X %d X %d", vm.width, vm.height, vm.depth);
	}
	return _VMList;
}

// The video modes are internal resolutions: a change only resizes the
// camera's rasters (the RSX scales the picture to the TV anyway).
static RwBool
PS3_SetVideoMode(RwInt32 videoMode)
{
	RwVideoMode vm;
	if (videoMode < 0 || videoMode >= RwEngineGetNumVideoModes())
		videoMode = 0;
	if (!RwEngineSetVideoMode(videoMode))
		return FALSE;
	RwEngineGetVideoModeInfo(&vm, videoMode);
	RsGlobal.maximumWidth = vm.width;
	RsGlobal.maximumHeight = vm.height;
	RsGlobal.width = vm.width;
	RsGlobal.height = vm.height;
	FrontEndMenuManager.m_nPrefsWidth = vm.width;
	FrontEndMenuManager.m_nPrefsHeight = vm.height;
	FrontEndMenuManager.m_nPrefsDepth = vm.depth;
	PS3_Logf("[video] internal resolution %dx%d (mode %d)", vm.width, vm.height, videoMode);
	return TRUE;
}

void
_psSelectScreenVM(RwInt32 videoMode)
{
	RwTexDictionarySetCurrent(nil);
	FrontEndMenuManager.UnloadTextures();
	if (PS3_SetVideoMode(videoMode)) {
		RwRect r;
		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;
		RsEventHandler(rsCAMERASIZE, &r);
	}
	FrontEndMenuManager.LoadAllTextures();
}

RwBool
IsForegroundApp()
{
	return !!ForegroundApp;
}

RwBool
psSelectDevice()
{
	if (!RwEngineSetSubSystem(0))
		return FALSE;

	// the mode saved in gta3.set (the internal resolution), else 1280x720
	RwInt32 mode = FrontEndMenuManager.m_nDisplayVideoMode;
#if PS3_STAGE == 1
	mode = 0;
#endif
	if (mode < 0 || mode >= RwEngineGetNumVideoModes())
		mode = 0;
	if (!PS3_SetVideoMode(mode))
		return FALSE;

	FrontEndMenuManager.m_nPrefsSubsystem = 0;
	FrontEndMenuManager.m_nPrefsWindowed = 0;
	FrontEndMenuManager.m_nDisplayVideoMode = mode;
	FrontEndMenuManager.m_nPrefsVideoMode = mode;
	FrontEndMenuManager.m_nSelectedScreenMode = 0;
	FrontEndMenuManager.m_nCurrOption = 0;
	PSGLOBAL(fullScreen) = TRUE;
	return TRUE;
}

void
_InputInitialiseJoys()
{
	PSGLOBAL(joy1id) = -1;
	PSGLOBAL(joy2id) = -1;
	if (glfwJoystickPresent(GLFW_JOYSTICK_1)) {
		int count;
		PSGLOBAL(joy1id) = GLFW_JOYSTICK_1;
		glfwGetJoystickButtons(GLFW_JOYSTICK_1, &count);
		ControlsManager.InitDefaultControlConfigJoyPad(count);
	}
}

long
_InputInitialiseMouse()
{
	return 0;
}

void
_InputTranslateShiftKeyUpDown(RsKeyCodes *rs)
{
	(void)rs;
}

void
psPostRWinit(void)
{
	_InputInitialiseJoys();
	_InputInitialiseMouse();

	CPad::GetPad(0)->Clear(true);
	CPad::GetPad(1)->Clear(true);
}

RwBool
_psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	(void)subSystem; (void)videoMode;
	return TRUE;
}

void
InitialiseLanguage()
{
	// The XMB language could pick the GXT here later; American for now.
	CGame::nastyGame = true;
	CMenuManager::m_PrefsAllowNastyGame = true;
	CGame::noProstitutes = false;
	CMenuManager::OS_Language = LANG_ENGLISH;
	CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;

	TheText.Unload();
	TheText.Load();
}

// Called during the long loads (loading screens): keeps the XMB responsive
void
HandleExit()
{
	PS3_Pump();
}

#if PS3_STAGE != 1
// Diagnostics (patch 16): librw's offscreen self test, with the font that
// renders as boxes
static void
PS3_SelfTest(const char *tag)
{
	CSprite2d &f = CFont::Sprite[2];	// font1
	rw::ps3::selfTest(tag, f.m_pTexture ? f.m_pTexture->raster : nil);
}
#else
#define PS3_SelfTest(tag)
#endif

// Pad -> CPad (glfw.cpp's CapturePad, on the DualShock 3 of ps3_pad.cpp).
// Called by CPad::UpdatePads every frame.
void
CapturePad(RwInt32 padID)
{
	int8 glfwPad = -1;

	if (padID == 0)
		glfwPad = PSGLOBAL(joy1id);
	else if (padID == 1)
		glfwPad = PSGLOBAL(joy2id);
	if (glfwPad == -1)
		return;

	PS3_PadPoll();

	int numButtons, numAxes;
	const uint8 *buttons = glfwGetJoystickButtons(glfwPad, &numButtons);
	const float *axes = glfwGetJoystickAxes(glfwPad, &numAxes);
	GLFWgamepadstate gamepadState;

	if (ControlsManager.m_bFirstCapture == false) {
		memcpy(&ControlsManager.m_OldState, &ControlsManager.m_NewState, sizeof(ControlsManager.m_NewState));
	} else {
		ControlsManager.m_NewState.mappedButtons[15] = ControlsManager.m_NewState.mappedButtons[16] = 0;
	}

	ControlsManager.m_NewState.buttons = (uint8*)buttons;
	ControlsManager.m_NewState.numButtons = numButtons;
	ControlsManager.m_NewState.id = glfwPad;
	ControlsManager.m_NewState.isGamepad = glfwGetGamepadState(glfwPad, &gamepadState);
	if (ControlsManager.m_NewState.isGamepad) {
		memcpy(&ControlsManager.m_NewState.mappedButtons, gamepadState.buttons, sizeof(gamepadState.buttons));
		float lt = gamepadState.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER], rt = gamepadState.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER];
		// L2 and R2: -1 released, 1 pressed
		ControlsManager.m_NewState.mappedButtons[15] = lt > -0.8f;
		ControlsManager.m_NewState.mappedButtons[16] = rt > -0.8f;
	}

	if (ControlsManager.m_bFirstCapture == true) {
		memcpy(&ControlsManager.m_OldState, &ControlsManager.m_NewState, sizeof(ControlsManager.m_NewState));
		ControlsManager.m_bFirstCapture = false;
	}

	RsPadButtonStatus bs;
	bs.padID = padID;

	RsPadEventHandler(rsPADBUTTONUP, (void *)&bs);

	RwV2d leftStickPos, rightStickPos;
	leftStickPos.x = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_LEFT_X] : numAxes >= 1 ? axes[0] : 0.0f;
	leftStickPos.y = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] : numAxes >= 2 ? axes[1] : 0.0f;
	rightStickPos.x = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] : numAxes >= 3 ? axes[2] : 0.0f;
	rightStickPos.y = ControlsManager.m_NewState.isGamepad ? gamepadState.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] : numAxes >= 4 ? axes[3] : 0.0f;

	{
		if (CPad::m_bMapPadOneToPadTwo)
			bs.padID = 1;
		RsPadEventHandler(rsPADBUTTONUP, (void *)&bs);
		RsPadEventHandler(rsPADBUTTONDOWN, (void *)&bs);
	}

	{
		if (CPad::m_bMapPadOneToPadTwo)
			bs.padID = 1;
		CPad *pad = CPad::GetPad(bs.padID);
		if (Abs(leftStickPos.x) > 0.3f)
			pad->PCTempJoyState.LeftStickX = (int32)(leftStickPos.x * 128.0f);
		if (Abs(leftStickPos.y) > 0.3f)
			pad->PCTempJoyState.LeftStickY = (int32)(leftStickPos.y * 128.0f);
		if (Abs(rightStickPos.x) > 0.3f)
			pad->PCTempJoyState.RightStickX = (int32)(rightStickPos.x * 128.0f);
		if (Abs(rightStickPos.y) > 0.3f)
			pad->PCTempJoyState.RightStickY = (int32)(rightStickPos.y * 128.0f);
	}
}

void
joysChangeCB(int jid, int event)
{
	(void)jid; (void)event;
}

// ---- boot checks ----------------------------------------------------------

// [3] USRDIR must be writable: re3.ini, saves and this log live there.
static bool
PS3_CheckUsrdir(void)
{
	FILE *f = fopen(PS3_USRDIR "/write.test", "wb");
	if (!f)
		return false;
	fclose(f);
	remove(PS3_USRDIR "/write.test");
	return true;
}

// [4] Game data present + endianness: the first entry of models/gta3.dir
// (little-endian offset/size in 2 KB sectors) has to fall inside gta3.img.
static bool
PS3_CheckData(void)
{
	// required = the game can't start without it; the rest is only logged
	static const struct { const char *name; bool required; } files[] = {
		{ "models/gta3.img", true }, { "models/gta3.dir", true },
		{ "models/generic.txd", false }, { "models/coll/generic.col", false },
		{ "data/gta3.dat", true }, { "data/main.scm", true }, { "data/default.ide", false },
		{ "data/timecyc.dat", false }, { "anim/ped.ifp", true }, { "anim/cuts.img", false },
		{ "anim/cuts.dir", false }, { "text/american.gxt", true },
		{ "audio/sfx.raw", false }, { "audio/sfx.sdt", false },
	};
	bool ok = true;
	char abs[1024];

	for (unsigned i = 0; i < ARRAY_SIZE(files); i++) {
		long long size = -1;
		bool found = PS3_ResolvePath(files[i].name, abs, sizeof(abs)) && PS3_FileSize(abs, &size);
		PS3_Logf("[4]   %-24s %s  size=%lld", files[i].name, found ? "found" : "MISSING", size);
		if (!found && files[i].required)
			ok = false;
	}
	if (!ok)
		return false;

	long long imgSize = 0;
	PS3_ResolvePath("models/gta3.img", abs, sizeof(abs));
	PS3_FileSize(abs, &imgSize);
	PS3_ResolvePath("models/gta3.dir", abs, sizeof(abs));
	FILE *f = fopen(abs, "rb");
	if (f) {
		CDirectory::DirectoryInfo di;
		long long n = 0;
		while (fread(&di, sizeof(di), 1, f) == 1)
			n++;
		fseek(f, 0, SEEK_SET);
		if (fread(&di, sizeof(di), 1, f) == 1) {
			uint32 off = LittleU32(di.offset), sz = LittleU32(di.size);
			di.name[23] = '\0';
			bool inside = ((long long)off + sz) * 2048 <= imgSize;
			PS3_Logf("[4]   gta3.dir: %lld entries, first \"%s\" sector %u, %u sectors (%s)",
			         n, di.name, off, sz, inside ? "inside gta3.img: endianness ok" : "OUTSIDE gta3.img: WRONG");
			if (!inside)
				ok = false;
		}
		fclose(f);
	}
	return ok;
}

// ---- stage 1 ---------------------------------------------------------------

extern uint32 gPS3CdReads, gPS3CdErrors;
extern uint64 gPS3CdBytes;

#if PS3_STAGE == 1

static int s1_frame;

static void
PS3_Stage1Status(const char *tag)
{
	CVector pos = FindPlayerCoors();
	int scripts = 0;
	char names[160] = "";
	for (CRunningScript *s = CTheScripts::pActiveScripts; s; s = s->GetNext()) {
		if (scripts < 12) {
			char n[9];
			memcpy(n, s->m_abScriptName, 8);
			n[8] = '\0';
			strncat(names, " ", sizeof(names) - strlen(names) - 1);
			strncat(names, n, sizeof(names) - strlen(names) - 1);
		}
		scripts++;
	}
	int32 onMission = CTheScripts::OnAMissionFlag ?
		*(int32*)&CTheScripts::ScriptSpace[CTheScripts::OnAMissionFlag] : -1;
	PS3_Logf("[s1] %s frame %d  game time %u ms  clock %02d:%02d  player (%.1f, %.1f, %.1f)  "
	         "level %d (collision %d)  cutscene %d  on mission %d  money %d",
	         tag, s1_frame, CTimer::GetTimeInMilliseconds(), CClock::GetHours(), CClock::GetMinutes(),
	         pos.x, pos.y, pos.z, (int)CGame::currLevel, (int)CCollision::ms_collisionInMemory,
	         CCutsceneMgr::IsRunning(), onMission, CWorld::Players[0].m_nMoney);
	PS3_Logf("[s1]   peds %d  vehicles %d  objects %d  scripts %d:%s",
	         CPools::GetPedPool()->GetNoOfUsedSpaces(), CPools::GetVehiclePool()->GetNoOfUsedSpaces(),
	         CPools::GetObjectPool()->GetNoOfUsedSpaces(), scripts, names);
	PS3_LogMem("[s1]  ");
}

// Runs the game for gameMs of game time at most 30 fps (like the PC frame
// limiter), so scripts, cutscenes and the clock run as in the real game.
static void
PS3_Stage1Run(uint32 gameMs, const char *tag)
{
	double t0 = psTimer(), worst = 0.0;
	double last = t0;
	uint32 g0 = CTimer::GetTimeInMilliseconds();
	uint32 nextStatus = g0 + 10000;
	int frames = 0;
	while (!RsGlobal.quit && CTimer::GetTimeInMilliseconds() - g0 < gameMs) {
		PS3_Pump();
		RsEventHandler(rsIDLE, (void *)TRUE);
		s1_frame++;
		PS3_crumbFrame = s1_frame;
		frames++;
		double now = psTimer();
		if (now - last > worst)
			worst = now - last;
		double wait = 33.3 - (now - last);
		if (wait > 1.0)
			sysUsleep((u32)(wait * 1000.0));
		last = psTimer();
		if (CTimer::GetTimeInMilliseconds() >= nextStatus) {
			PS3_Stage1Status(tag);
			nextStatus += 10000;
		}
		// the game time stops in some screens: don't wait forever
		if (psTimer() - t0 > gameMs * 3.0 + 20000.0) {
			PS3_Logf("[s1] %s: game time stuck at %u ms, moving on", tag, CTimer::GetTimeInMilliseconds());
			break;
		}
	}
	double t = psTimer() - t0;
	PS3_Logf("[s1] %s: %d frames in %.1f s, worst frame %.1f ms (without the 30 fps wait)",
	         tag, frames, t / 1000.0, worst);
	PS3_Stage1Status(tag);
}

static void
PS3_Stage1Teleport(const char *name, float x, float y, float z)
{
	if (RsGlobal.quit)
		return;
	CPlayerPed *player = FindPlayerPed();
	if (player == nil) {
		PS3_Logf("[s1] teleport %s: no player", name);
		return;
	}
	PS3_Logf("[s1] ---- teleport to %s (%.0f, %.0f) ----", name, x, y);
	double t0 = psTimer();
	CStreaming::LoadScene(CVector(x, y, z));
	float gz = CWorld::FindGroundZForCoord(x, y);
	if (gz > -100.0f && gz < 500.0f && gz != 0.0f)
		z = gz + 1.0f;
	player->Teleport(CVector(x, y, z));
	PS3_Logf("[s1] %s: LoadScene + teleport in %.0f ms, ground z %.1f", name, psTimer() - t0, gz);
	PS3_Stage1Run(25000, name);
}

static void
PS3_Stage1(void)
{
	PS3_Log("[7] stage 1: new game, the intro runs by itself");
	PS3_Stage1Run(150000, "intro");

	// a walk over the three islands: streaming, collision, level changes
	PS3_Stage1Teleport("Portland (Harwood)", 890.0f, -310.0f, 10.0f);
	PS3_Stage1Teleport("Portland (Chinatown)", 920.0f, -690.0f, 15.0f);
	PS3_Stage1Teleport("Staunton (Commercial)", 100.0f, -900.0f, 30.0f);
	PS3_Stage1Teleport("Staunton (Aspatria)", -50.0f, 150.0f, 15.0f);
	PS3_Stage1Teleport("Shoreside (Airport)", -1100.0f, -600.0f, 12.0f);
	PS3_Stage1Teleport("Shoreside (Cedar Grove)", -700.0f, 50.0f, 50.0f);
	PS3_Stage1Teleport("Portland (back)", 890.0f, -310.0f, 10.0f);

	PS3_Logf("[s1] cd reads %u, %u errors, %.1f MB", gPS3CdReads, gPS3CdErrors, gPS3CdBytes / 1048576.0);
	PS3_Log("[7] stage 1 done");
	RsGlobal.quit = TRUE;
}
#endif

// ---- main loop ------------------------------------------------------------

static void
PS3_GameMain(void)
{
	RwV2d pos;

	// rsINITIALIZE (main.cpp AppEventHandler) step by step, each one in the log
	PS3_Log("[5] CFileMgr::Initialise");
	CFileMgr::Initialise();
	PS3_Log("[5] CdStreamInit");
	CdStreamInit(MAX_CDCHANNELS);
	PS3_Log("[5] ValidateVersion");
	ValidateVersion();
#ifdef CUSTOM_FRONTEND_OPTIONS
	PS3_Log("[5] CustomFrontendOptionsPopulate");
	CustomFrontendOptionsPopulate();
#endif
	PS3_Log("[5] RsInitialize");
	if (!RsInitialize()) {
		PS3_Log("[5] FATAL: RsInitialize failed");
		return;
	}
	PS3_Log("[5] rsINITIALIZE done");
	// 60 fps: the frame limiter's cap (the flips are on vsync anyway)
	RsGlobal.maxFPS = 60;

#if PS3_STAGE != 1
	// patch 29: the camera buffers are tiled (+ compressed depth, ZCULL).
	// If that ever gives a black screen: an empty USRDIR/notiles.txt
	{
		FILE *nt = fopen(PS3_USRDIR "/notiles.txt", "rb");
		if (nt) {
			fclose(nt);
			rw::ps3::rsxTiling = false;
		}
		PS3_Logf("[5] RSX tiling %s", rw::ps3::rsxTiling ? "on (notiles.txt turns it off)" : "OFF (notiles.txt)");
	}
#endif

	openParams.width = PS3_SCREEN_WIDTH;
	openParams.height = PS3_SCREEN_HEIGHT;
	openParams.windowtitle = RsGlobal.appName;

	ControlsManager.MakeControllerActionsBlank();
	ControlsManager.InitDefaultControlConfiguration();

	if (RsEventHandler(rsRWINITIALIZE, &openParams) == rsEVENTERROR) {
		PS3_Log("[6] FATAL: rsRWINITIALIZE failed");
		RsEventHandler(rsTERMINATE, nil);
		return;
	}
	PS3_Logf("[6] RenderWare up: %dx%d", RsGlobal.maximumWidth, RsGlobal.maximumHeight);
#if PS3_STAGE != 1
	// PS3_SelfTest("boot");	// patch 18 and before; the menus are fixed
#endif

	psPostRWinit();

	ControlsManager.InitDefaultControlConfigMouse(MousePointerStateHelper.GetMouseSetUp());

	{
		RwRect r;
		r.x = 0;
		r.y = 0;
		r.w = RsGlobal.maximumWidth;
		r.h = RsGlobal.maximumHeight;
		RsEventHandler(rsCAMERASIZE, &r);
	}

	{
		CFileMgr::SetDirMyDocuments();
		int32 gta3set = CFileMgr::OpenFile("gta3.set", "r");
		if (gta3set) {
			ControlsManager.LoadSettings(gta3set);
			CFileMgr::CloseFile(gta3set);
		}
		CFileMgr::SetDir("");
#ifdef LOAD_INI_SETTINGS
		LoadINIControllerSettings();
		// re3.ini from before the PS2 layout (or none): the DualShock 3
		// bindings, saved so later edits in the menu stick
		if (ControlsManager.ms_padButtonsInited < 16) {
			ControlsManager.ms_padButtonsInited = 0;
			ControlsManager.InitDefaultControlConfigJoyPad(16);
			SaveINIControllerSettings();
			PS3_Log("[6] pad: PS2 Setup 1 bindings");
		}
#endif
	}
	PS3_LogMem("[6]");

	while (TRUE) {
		RwInitialised = TRUE;

		pos.x = RsGlobal.maximumWidth * 0.5f;
		pos.y = RsGlobal.maximumHeight * 0.5f;
		RsMouseSetPos(&pos);

		while (!RsGlobal.quit && !FrontEndMenuManager.m_bWantToRestart) {
			PS3_Pump();
			switch (gGameState) {
			case GS_START_UP:
				gGameState = GS_INIT_ONCE;
				break;

			case GS_INIT_ONCE:
				PS3_Log("[7] CGame::InitialiseOnceAfterRW");
				LoadingScreen(nil, nil, "loadsc0");
				if (!CGame::InitialiseOnceAfterRW())
					RsGlobal.quit = TRUE;
				PS3_LogMem("[7] once after RW:");
#if PS3_STAGE == 1
				gGameState = GS_INIT_PLAYING_GAME;
#else
				gGameState = GS_INIT_FRONTEND;
#endif
				break;

			case GS_INIT_FRONTEND:
				PS3_Log("[7] frontend");
				{
					// diagnostics: a menu string as the font will get it
					wchar *t = TheText.Get("FEM_ON");
					PS3_Logf("[7] text FEM_ON: %04x %04x %04x %04x", t[0], t[0] ? t[1] : 0,
					         t[0] && t[1] ? t[2] : 0, t[0] && t[1] && t[2] ? t[3] : 0);
				}
				LoadingScreen(nil, nil, "loadsc0");
				FrontEndMenuManager.m_bGameNotLoaded = true;
				CMenuManager::m_bStartUpFrontEndRequested = true;
				gGameState = GS_FRONTEND;
				break;

			case GS_FRONTEND:
				RsEventHandler(rsFRONTENDIDLE, nil);
				if (!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad)
					gGameState = GS_INIT_PLAYING_GAME;
				if (FrontEndMenuManager.m_bWantToLoad) {
					InitialiseGame();
					FrontEndMenuManager.m_bGameNotLoaded = false;
					gGameState = GS_PLAYING_GAME;
				}
				break;

			case GS_INIT_PLAYING_GAME:
				PS3_Log("[7] InitialiseGame");
				InitialiseGame();
				FrontEndMenuManager.m_bGameNotLoaded = false;
				PS3_LogMem("[7] game loaded:");
				// PS3_SelfTest("game");
				gGameState = GS_PLAYING_GAME;
				break;

			case GS_PLAYING_GAME:
#if PS3_STAGE == 1
				PS3_Stage1();
#else
			// no frame limiter: the flips are on vsync and the present waits
			// for a free display buffer, which paces the game at 60 fps at
			// most (a limiter on top only made frames miss vsyncs)
			if (PS3_AutotestWantsLoad())
				break;	// the loop ends here and the save gets loaded below
			RsEventHandler(rsIDLE, (void *)TRUE);
#endif
				break;

			default:
				gGameState = GS_INIT_ONCE;
				break;
			}
		}

		RwInitialised = FALSE;
		FrontEndMenuManager.UnloadTextures();
		if (!FrontEndMenuManager.m_bWantToRestart || RsGlobal.quit)
			break;

		CPad::ResetCheats();
		CPad::StopPadsShaking();
		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);
		CTimer::Stop();

		if (FrontEndMenuManager.m_bWantToLoad) {
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
#if PS3_STAGE != 1
			PS3_AutotestLoaded();
#endif
		} else {
			if (gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();
			CTimer::Stop();
			if (FrontEndMenuManager.m_bFirstTime == true)
				gGameState = GS_INIT_FRONTEND;
			else
				gGameState = GS_INIT_PLAYING_GAME;
		}
		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
	}

	PS3_Log("[8] shutdown");
	if (gGameState == GS_PLAYING_GAME)
		CGame::ShutDown();
	DMAudio.Terminate();
	_psFreeVideoModeList();
	RsEventHandler(rsRWTERMINATE, nil);
	RsEventHandler(rsTERMINATE, nil);
	PS3_Log("[8] shutdown done");
}

static void
PS3_GameThread(void *arg)
{
	(void)arg;

	PS3_Log("[1] game thread started");

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, ps3_sysutil_callback, NULL);
	PS3_Log("[2] sysutil callback registered");

	if (!PS3_CheckUsrdir()) {
		PS3_Log("[3] FATAL: " PS3_USRDIR " is not writable");
		sysThreadExit(1);
	}
	PS3_Log("[3] USRDIR writable");

	if (!PS3_CheckData()) {
		PS3_Log("[4] FATAL: GTA III data missing or unreadable: copy the PC game "
		        "(data/, models/, anim/, text/, audio/, txd/...) to " PS3_USRDIR);
		sysThreadExit(1);
	}
	PS3_Logf("[4] game data found, free user mem ~%u MB", PS3_FreeUserMemMB());

	PS3_GameMain();

	sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
	sysThreadExit(0);
}

int
main(int argc, char *argv[])
{
	sys_ppu_thread_t tid;
	u64 retval = 0;
	s32 ret;

	(void)argc; (void)argv;

	PS3_LogInit();
	PS3_Logf("[0] main reached: re3-PS3 stage %d, patch " PS3_PATCHLEVEL ", built " __DATE__ " " __TIME__,
	         PS3_STAGE);

	ret = sysThreadCreate(&tid, PS3_GameThread, NULL, 1000, PS3_GAME_STACK,
	                      THREAD_JOINABLE, (char *)"re3");
	if (ret != 0) {
		PS3_Logf("[1] FATAL: sysThreadCreate failed (0x%08x)", ret);
		PS3_LogShutdown();
		return 1;
	}
	ret = sysThreadJoin(tid, &retval);

	// -1 without an [8] line = the game thread died (crash) right after the
	// last line above
	PS3_Logf("exit code %d (join 0x%08x)", (int)retval, (unsigned)ret);
	if ((int)retval != 0)
		PS3_DumpCrumbs();
	PS3_LogShutdown();
	return (int)retval;
}
