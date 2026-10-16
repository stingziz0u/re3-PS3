// Car spawner: L3 + R3 together on foot opens it, Left/Right go through the
// vehicles in model order (L1/R1 by 10, held = repeat), X puts the player in
// the one shown, O closes. The game keeps running underneath; while the
// spawner is open the pad goes to it, not to the player.
// Same file in re3-PS3 and reVC-PS3, PS3_GAME_VC picks the game.
#define PS3_GAME_VC 0

#include "common.h"
#include "ps3_platform.h"
#include "ps3_glfw.h"
#include "Pad.h"
#include "Timer.h"
#include "Font.h"
#include "Text.h"
#include "ModelInfo.h"
#include "VehicleModelInfo.h"
#include "ModelIndices.h"
#include "Streaming.h"
#include "World.h"
#include "PlayerPed.h"
#include "Automobile.h"
#include "Boat.h"
#if PS3_GAME_VC
#include "Bike.h"
#endif
#include "CarCtrl.h"
#include "Script.h"
#include "WaterLevel.h"
#include "Camera.h"
#include "CutsceneMgr.h"
#include "Replay.h"
#include "Frontend.h"

extern wchar WideErrorString[];

#define BTN(b) (1u << (b))
#define B_CROSS  BTN(GLFW_GAMEPAD_BUTTON_A)
#define B_CIRCLE BTN(GLFW_GAMEPAD_BUTTON_B)
#define B_L1     BTN(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER)
#define B_R1     BTN(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER)
#define B_L3     BTN(GLFW_GAMEPAD_BUTTON_LEFT_THUMB)
#define B_R3     BTN(GLFW_GAMEPAD_BUTTON_RIGHT_THUMB)
#define B_LEFT   BTN(GLFW_GAMEPAD_BUTTON_DPAD_LEFT)
#define B_RIGHT  BTN(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT)

enum { SP_CLOSED, SP_OPEN, SP_RELEASE };	// RELEASE: pad held back until X/O/L3/R3 are up

static int spState;
static int16 spList[256];
static int spCount;
static int spSel;
static unsigned spPrev;
static int spRepeatDir;
static uint32 spRepeatAt;
static wchar spName[80];
static wchar spHint[120];
static wchar spNote[80];
static uint32 spNoteUntil;

static bool
IsRCModel(int mi)
{
#if PS3_GAME_VC
	return mi == MI_RCBANDIT || mi == MI_RCBARON || mi == MI_RCRAIDER || mi == MI_RCGOBLIN;
#else
	return mi == MI_RCBANDIT;
#endif
}

static void
BuildList(void)
{
	spCount = 0;
	for (int i = 0; i < MODELINFOSIZE && spCount < (int)ARRAY_SIZE(spList); i++) {
		CBaseModelInfo *mi = CModelInfo::GetModelInfo(i);
		if (mi == nil || mi->GetModelType() != MITYPE_VEHICLE)
			continue;
		int type = ((CVehicleModelInfo*)mi)->m_vehicleType;
		// trains, the police heli and the dead planes can't be driven
		if (type == VEHICLE_TYPE_TRAIN || type == VEHICLE_TYPE_HELI || type == VEHICLE_TYPE_PLANE)
			continue;
		if (IsRCModel(i))
			continue;
		spList[spCount++] = i;
	}
	PS3_Logf("[spawn] %d vehicles in the list", spCount);
}

static void
Note(const char *msg)
{
	AsciiToUnicode(msg, spNote);
	spNoteUntil = CTimer::GetTimeInMilliseconds() + 2500;
}

static void
MakeName(void)
{
	int mi = spList[spSel];
	CVehicleModelInfo *vmi = (CVehicleModelInfo*)CModelInfo::GetModelInfo(mi);
	char buf[48];
	sprintf(buf, "%d: ", mi);
	AsciiToUnicode(buf, spName);
	int n = strlen(buf);
	wchar *name = TheText.Get(vmi->m_gameName);
	if (name == nil || name == WideErrorString || name[0] == 0) {
		AsciiToUnicode(vmi->GetModelName(), spName + n);
	} else {
		int i;
		for (i = 0; name[i] && n + i < (int)ARRAY_SIZE(spName) - 1; i++)
			spName[n + i] = name[i];
		spName[n + i] = 0;
	}
	sprintf(buf, "(%d/%d)", spSel + 1, spCount);
	char hint[120];
	sprintf(hint, "Left/Right: vehicle   L1/R1: 10   X: spawn   O: close   %s", buf);
	AsciiToUnicode(hint, spHint);
}

static void
Move(int d)
{
	spSel = ((spSel + d) % spCount + spCount) % spCount;
	MakeName();
}

// Why it can't open now, nil if it can
static const char *
Blocked(CPlayerPed *p)
{
	if (CCutsceneMgr::IsRunning() || TheCamera.m_WideScreenOn || CReplay::IsPlayingBack())
		return "Car spawner: not now";
	if (p->bInVehicle)
		return "Car spawner: get out of the vehicle first";
	if (CPad::GetPad(0)->ArePlayerControlsDisabled() || !p->IsPedInControl())
		return "Car spawner: not now";
	return nil;
}

// Boats go on the water in front of the player (up to 30 m)
static bool
FindWater(CPlayerPed *p, CVector &pos)
{
	CVector fwd = p->GetForward();
	fwd.z = 0.0f;
	fwd.Normalise();
	for (float d = 4.0f; d <= 30.0f; d += 2.0f) {
		CVector at = p->GetPosition() + fwd * d;
		float water;
		if (!CWaterLevel::GetWaterLevel(at.x, at.y, at.z, &water, true))
			continue;
		bool found;
		float ground = CWorld::FindGroundZFor3DCoord(at.x, at.y, water + 1.0f, &found);
		if (found && ground > water - 1.0f)	// a dock, a beach
			continue;
		pos = CVector(at.x, at.y, water);
		return true;
	}
	return false;
}

static const char *
Spawn(CPlayerPed *p)
{
	int mi = spList[spSel];
	bool boat = CModelInfo::IsBoatModel(mi);
	CVector pos = p->GetPosition();
	if (boat && !FindWater(p, pos))
		return "Car spawner: no water in front";

	// a model a mission or the game already keeps stays kept
	bool kept = !!(CStreaming::ms_aInfoForModel[mi].m_flags & STREAMFLAGS_DONT_REMOVE);
	CStreaming::RequestModel(mi, STREAMFLAGS_DONT_REMOVE);
	CStreaming::LoadAllRequestedModels(false);
	if (CStreaming::ms_aInfoForModel[mi].m_loadState != STREAMSTATE_LOADED) {
		PS3_Logf("[spawn] model %d didn't load", mi);
		return "Car spawner: couldn't load it, try again";
	}

	CVehicle *v;
	if (boat)
		v = new CBoat(mi, RANDOM_VEHICLE);
#if PS3_GAME_VC
	else if (CModelInfo::IsBikeModel(mi)) {
		v = new CBike(mi, RANDOM_VEHICLE);
		if (v)
			((CBike*)v)->bIsStanding = true;
	}
#endif
	else
		v = new CAutomobile(mi, RANDOM_VEHICLE);
	// the vehicle holds the model now
	if (!kept) {
		CStreaming::SetModelIsDeletable(mi);
		CStreaming::SetModelTxdIsDeletable(mi);
	}
	if (v == nil) {
		PS3_Logf("[spawn] model %d: vehicle pool full", mi);
		return "Car spawner: too many vehicles around";
	}

	if (!boat) {
		bool found;
		float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 0.5f, &found);
		pos.z = found ? ground : pos.z - 1.0f;
	}
	pos.z += v->GetDistanceFromCentreOfMassToBaseOfModel();
	v->SetPosition(pos);
	v->SetHeading(p->m_fRotationCur);
	CTheScripts::ClearSpaceForMissionEntity(pos, v);	// random cars and peds in the way
	v->SetStatus(STATUS_ABANDONED);
	v->m_nDoorLock = CARLOCK_UNLOCKED;
	v->bHasBeenOwnedByPlayer = true;
	if (!boat)
		CCarCtrl::JoinCarWithRoadSystem(v);
	CWorld::Add(v);

	p->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, v);
	p->WarpPedIntoCar(v);
	PS3_Logf("[spawn] model %d at %.1f %.1f %.1f", mi, pos.x, pos.y, pos.z);
	return nil;
}

static void
Consume(void)
{
	CPad::GetPad(0)->NewState.Clear();
}

// CPad::UpdatePads, after pad 0 is read
void
PS3_CarSpawnerPad(void)
{
	if (FrontEndMenuManager.m_bMenuActive)
		return;
	CPlayerPed *p = FindPlayerPed();
	unsigned m = PS3_PadButtons();
	unsigned down = m & ~spPrev;
	spPrev = m;
	if (p == nil) {
		spState = SP_CLOSED;
		return;
	}

	switch (spState) {
	case SP_CLOSED:
		if ((m & B_L3) && (m & B_R3) && (down & (B_L3 | B_R3))) {
			const char *why = Blocked(p);
			if (why) {
				Note(why);
			} else {
				if (spCount == 0)
					BuildList();
				if (spCount == 0)
					return;
				spState = SP_OPEN;
				spNoteUntil = 0;
				spRepeatDir = 0;
				MakeName();
				Consume();
				return;
			}
			spState = SP_RELEASE;	// R3 in a car is the submission mission
			Consume();
		}
		return;

	case SP_OPEN: {
		if (Blocked(p)) {	// busted, wasted, a cutscene started
			spState = SP_RELEASE;
			Consume();
			return;
		}
		uint32 now = CTimer::GetTimeInMilliseconds();
		if (down & B_CIRCLE) {
			spState = SP_RELEASE;
		} else if (down & B_CROSS) {
			const char *err = Spawn(p);
			if (err)
				Note(err);
			else
				spState = SP_RELEASE;
		} else {
			int dir = 0;
			if (down & B_LEFT) dir = -1;
			else if (down & B_RIGHT) dir = 1;
			else if (down & B_L1) dir = -10;
			else if (down & B_R1) dir = 10;
			if (dir) {
				Move(dir);
				spRepeatDir = dir;
				spRepeatAt = now + 400;
			} else if (spRepeatDir) {
				unsigned key = spRepeatDir == -1 ? B_LEFT : spRepeatDir == 1 ? B_RIGHT :
				               spRepeatDir < 0 ? B_L1 : B_R1;
				if (!(m & key))
					spRepeatDir = 0;
				else if ((int32)(now - spRepeatAt) >= 0) {
					Move(spRepeatDir);
					spRepeatAt = now + (spRepeatDir == 1 || spRepeatDir == -1 ? 80 : 200);
				}
			}
		}
		Consume();
		return;
	}

	case SP_RELEASE:
		if (m & (B_CROSS | B_CIRCLE | B_L3 | B_R3))
			Consume();
		else
			spState = SP_CLOSED;
		return;
	}
}

// CHud::Draw, after the subtitles
void
PS3_CarSpawnerDraw(void)
{
	bool note = CTimer::GetTimeInMilliseconds() < spNoteUntil;
	if (spState != SP_OPEN && !note)
		return;

	CFont::SetJustifyOff();
	CFont::SetBackgroundOff();
	CFont::SetCentreOn();
	CFont::SetPropOn();
	CFont::SetCentreSize(SCREEN_WIDTH - SCREEN_SCALE_X(60.0f));
	CFont::SetDropShadowPosition(2);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));
#if PS3_GAME_VC
	CFont::SetFontStyle(FONT_LOCALE(FONT_STANDARD));
	const float bigX = 0.75f, bigY = 1.55f, smallX = 0.5f, smallY = 1.05f;
#else
	CFont::SetFontStyle(FONT_LOCALE(FONT_BANK));
	const float bigX = 0.62f, bigY = 1.4f, smallX = 0.42f, smallY = 0.95f;
#endif
	if (spState == SP_OPEN) {
		CFont::SetColor(CRGBA(255, 255, 255, 255));
		CFont::SetScale(SCREEN_SCALE_X(bigX), SCREEN_SCALE_Y(bigY));
		CFont::PrintString(SCREEN_WIDTH / 2.0f, SCREEN_SCALE_FROM_BOTTOM(175.0f), spName);
	}
	CFont::SetColor(note ? CRGBA(255, 200, 80, 255) : CRGBA(200, 200, 200, 255));
	CFont::SetScale(SCREEN_SCALE_X(smallX), SCREEN_SCALE_Y(smallY));
	CFont::PrintString(SCREEN_WIDTH / 2.0f, SCREEN_SCALE_FROM_BOTTOM(145.0f), note ? spNote : spHint);
	CFont::SetDropShadowPosition(0);
	CFont::SetCentreOff();
}
