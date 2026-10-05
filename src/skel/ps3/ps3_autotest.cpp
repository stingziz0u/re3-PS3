// ps3_autotest.cpp -- re3-PS3 automatic test run.
//
// Put an empty file named autotest.txt in USRDIR (by FTP), load a save in a
// safehouse (no mission running) and leave the pad alone. The test then
// plays by itself and writes "[autotest]" lines to the log:
//
//   1. weather and time of day: sunny / cloudy / rain / fog at 0, 6, 12, 19 h
//   2. a flight over the three islands at car speed (streaming, island
//      changes, LODs), landing at every restart point and garage the
//      script created
//   3. save + load on Staunton and on Shoreside (GTA3sf9.b, not a menu slot)
//   4. side missions: taxi, ambulance, fire truck, vigilante (started with
//      R3 like a player would, then cancelled), Pay 'n' Spray with a wanted
//      level, bomb shops, import/export garages, and the Dodo at the airport
//   5. all of it again "loops" times (autotest.txt: "loops=5" ~ one hour),
//      with memory and VRAM logged at every step: a leak shows up as a
//      number that keeps growing loop after loop
//
// When it ends the player is put back where he was and the file is renamed
// to autotest.done.txt, so the next boot doesn't run it again. The player
// can't die and has no wanted level during the run (except on purpose).
// Not automated: rampages, RC missions (they need their own pickups/vans).
//
// Other modes (a line in autotest.txt): "mode=rain" (rain effects one by one),
// "mode=leak" (the same actions over and over, heap at each step), "mode=gpu"
// (smoke, rain and ZCULL compared where the player stands, no 60 fps cap),
// "mode=full" (every mission 30 s, odd jobs, rampage, one of each
// collectible; "from=N" / "to=N" for a part of the missions). The full test
// changes the game's progress: use a save you don't mind, don't save after.

#include "common.h"
#include "crossplatform.h"

#if PS3_STAGE != 1	// the test needs the renderer (stage 2)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdarg.h>
#include <sys/systime.h>

#include "General.h"
#include "Pad.h"
#include "Timer.h"
#include "Clock.h"
#include "Weather.h"
#include "World.h"
#include "PlayerPed.h"
#include "PlayerInfo.h"
#include "Wanted.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Pools.h"
#include "Streaming.h"
#include "ModelIndices.h"
#include "ModelInfo.h"
#include "Garages.h"
#include "Restart.h"
#include "Zones.h"
#include "Script.h"
#include "CarCtrl.h"
#include "CutsceneMgr.h"
#include "Frontend.h"
#include "Game.h"
#include "GenericGameStorage.h"
#include "PCSave.h"
#include "AudioManager.h"
#include "AnimManager.h"
#include "AnimBlendAssociation.h"
#include "WeaponInfo.h"
#include "Camera.h"
#include "Pickups.h"
#include "FileMgr.h"
#include "Darkel.h"
#include "Particle.h"
#include "main.h"

#include "ps3_platform.h"

namespace rw { namespace ps3 { void vramStats(uint32 *used, uint32 *total, uint32 *blocks); extern bool additiveKill;
	extern bool rsxTiling, zcullOn; extern int32 blendAlphaRef; void setFlipVsync(bool on); } }
extern int32 gPS3FxOff;	// Weather.cpp
extern "C" void PS3_LogMemTag(const char *tag);

#define AUTOTEST_FILE      PS3_USRDIR "/autotest.txt"
#define AUTOTEST_DONE_FILE PS3_USRDIR "/autotest.done.txt"
#define AUTOTEST_SLOT      SLOT_COUNT	// GTA3sf9.b: not one of the 8 menu slots

#define FLY_HEIGHT  50.0f	// above the sea: over most roofs (hills go through)
#define FLY_SPEED    1.0f	// metres per frame: 60 m/s, a fast car's top speed

enum StepType {
	ST_WEATHER,	// a = weather, b = hour
	ST_FLY,		// to (x, y): flight, then landing
	ST_SAVELOAD,	// save here, load it back, check the position
	ST_SIDEMISSION,	// a = vehicle model: get in, R3, wait, R3, get out
	ST_GARAGE,	// a = garage index: drive a car in, wait
	ST_DODO,	// at (x, y): take off from the airport
	ST_LOOPEND,	// memory snapshot, next loop
	ST_MISSION,	// a = mission script index: started (mission switcher), x seconds
	ST_MISSIONEND,	// the running mission fails and cleans up
	ST_PICKUP,	// a = 0 hidden package, 1 rampage: the player on it, x seconds
};

struct Step {
	int type;
	int a, b;
	float x, y;
	char name[48];
};

#define MAX_STEPS 256
static Step steps[MAX_STEPS];
static int numSteps;

static int state;		// 0 off, 1 waiting to start, 2 running, 3 done
static int loops = 1, loop;
static int rainMode;		// autotest.txt "mode=rain": only the rain test, where the player is
static int leakMode;		// "mode=leak": the same few actions over and over, heap logged at each
static int gpuMode;		// "mode=gpu": smoke / rain / ZCULL compared where the player is, no 60 fps cap
static int fullMode;		// "mode=full": every mission script, the odd jobs, one of each collectible
static int fromMission = 0, toMission = -1;	// "from=N" / "to=N": a part of the missions only
static int pulseCross;		// frames: cross pressed on and off (skips cutscenes)
static int packagesBefore;
static int smokeOn;		// the step puts smoke in front of the camera
static uint64 smokeLastUs;
static double lastHeapMB;
static int cur;			// step being run
static int phase;		// inside the step
static uint64 phaseStartUs;
static uint64 stepStartUs, runStartUs;
static CVector startPos;
static float startHeading;
static int wantLoad;		// ps3.cpp asks for it between frames
static int waitingForLoad;
static CVector savedPos;
static int savedLevel;
static int failures;
static CVehicle *testCar;
static int32 testCarHandle;
static CVector flyFrom, flyTo;
static float flyDist, flyDone;
static float dodoMaxZ;
static int pressR3, pressCross, pullUp;
static uint64 lastFrameUs;

// per step frame stats
static uint32 sFrames, sSlow, sVerySlow;
static uint64 sWorstUs, sSumUs;

static uint64 nowUs(void) { return sysGetSystemTime(); }
static float phaseSecs(void) { return (nowUs() - phaseStartUs) / 1000000.0f; }
static void nextPhase(void) { phase++; phaseStartUs = nowUs(); }

static const char *levelName(int l)
{
	switch (l) {
	case LEVEL_INDUSTRIAL: return "Portland";
	case LEVEL_COMMERCIAL: return "Staunton";
	case LEVEL_SUBURBAN: return "Shoreside";
	default: return "?";
	}
}

static int levelAt(float x, float y)
{
	CVector p(x, y, 20.0f);
	return CTheZones::GetLevelFromPosition(&p);
}

static void addStep(int type, int a, int b, float x, float y, const char *fmt, ...)
	__attribute__((format(printf, 6, 7)));
static void addStep(int type, int a, int b, float x, float y, const char *fmt, ...)
{
	if (numSteps >= MAX_STEPS)
		return;
	Step &s = steps[numSteps++];
	s.type = type; s.a = a; s.b = b; s.x = x; s.y = y;
	va_list va;
	va_start(va, fmt);
	vsnprintf(s.name, sizeof(s.name), fmt, va);
	va_end(va);
}

// ---- the player ---------------------------------------------------------------

static void keepPlayerSafe(void)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil)
		return;
	p->m_fHealth = 100.0f;
	p->bBulletProof = p->bFireProof = p->bExplosionProof = p->bCollisionProof = p->bMeleeProof = true;
	CWorld::Players[CWorld::PlayerInFocus].m_nMoney = Max(CWorld::Players[CWorld::PlayerInFocus].m_nMoney, 20000);
}

static void releasePlayer(void)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil)
		return;
	p->bBulletProof = p->bFireProof = p->bExplosionProof = p->bCollisionProof = p->bMeleeProof = false;
	p->bUsesCollision = true;
	p->bAffectedByGravity = true;
}

static void clearWanted(void)
{
	CPlayerPed *p = FindPlayerPed();
	if (p && p->m_pWanted->m_nWantedLevel > 0)
		p->SetWantedLevel(0);
}

static void warpOutOfCar(CVector pos)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil)
		return;
	if (p->bInVehicle && p->m_pMyVehicle) {
		CVehicle *v = p->m_pMyVehicle;
		if (v->pDriver == p) {
			v->RemoveDriver();
			v->SetStatus(STATUS_ABANDONED);
			v->bEngineOn = false;
			v->AutoPilot.m_nCruiseSpeed = 0;
		} else
			v->RemovePassenger(p);
		p->bInVehicle = false;
		p->m_pMyVehicle = nil;
		p->SetPedState(PED_IDLE);
		p->bUsesCollision = true;
		p->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		p->AddWeaponModel(CWeaponInfo::GetWeaponInfo(p->GetWeapon()->m_eWeaponType)->m_nModelId);
		p->RemoveInCarAnims();
		if (p->m_pVehicleAnim)
			p->m_pVehicleAnim->blendDelta = -1000.0f;
		p->m_pVehicleAnim = nil;
		p->SetMoveState(PEDMOVE_NONE);
		CAnimManager::BlendAnimation(p->GetClump(), p->m_animGroup, ANIM_STD_IDLE, 100.0f);
		p->RestartNonPartialAnims();
		AudioManager.PlayerJustLeftCar();
	}
	pos.z += p->GetDistanceFromCentreOfMassToBaseOfModel();
	p->Teleport(pos);
}

static void deleteTestCar(void)
{
	if (testCar == nil)
		return;
	// only if it still is the one we made (a load wipes the pool)
	if (CPools::GetVehiclePool()->GetAt(testCarHandle) == testCar) {
		CPlayerPed *p = FindPlayerPed();
		if (p && p->bInVehicle && p->m_pMyVehicle == testCar)
			warpOutOfCar(testCar->GetPosition() + CVector(3.0f, 0.0f, 0.0f));
		CWorld::Remove(testCar);
		CWorld::RemoveReferencesToDeletedObject(testCar);
		delete testCar;
	}
	testCar = nil;
}

// Ground under (x, y) after loading the place; false: water or nothing there
static bool groundAt(float x, float y, float *z)
{
	CStreaming::LoadScene(CVector(x, y, 20.0f));
	bool found = false;
	float gz = CWorld::FindGroundZFor3DCoord(x, y, 1000.0f, &found);
	if (!found || gz < 1.0f || gz > 500.0f)
		return false;
	*z = gz;
	return true;
}

static CVehicle *makeCar(int model, CVector pos, float heading)
{
	CStreaming::RequestModel(model, STREAMFLAGS_DEPENDENCY);
	CStreaming::LoadAllRequestedModels(false);
	if (!CStreaming::HasModelLoaded(model))
		return nil;
	CAutomobile *car = new CAutomobile(model, MISSION_VEHICLE);
	pos.z += car->GetDistanceFromCentreOfMassToBaseOfModel();
	car->SetPosition(pos);
	car->SetHeading(heading);
	CTheScripts::ClearSpaceForMissionEntity(pos, car);
	car->SetStatus(STATUS_ABANDONED);
	car->bIsLocked = false;
	CCarCtrl::JoinCarWithRoadSystem(car);
	car->AutoPilot.m_nCarMission = MISSION_NONE;
	car->AutoPilot.m_nTempAction = TEMPACT_NONE;
	car->bEngineOn = false;
	car->m_nZoneLevel = CTheZones::GetLevelFromPosition(&pos);
	car->bHasBeenOwnedByPlayer = true;
	CWorld::Add(car);
	testCarHandle = CPools::GetVehiclePool()->GetIndex(car);
	return car;
}

static void warpIntoCar(CVehicle *car)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil || car == nil)
		return;
	p->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, car);
	p->WarpPedIntoCar(car);
}

// ---- logging ------------------------------------------------------------------

static void statsReset(void)
{
	sFrames = sSlow = sVerySlow = 0;
	sWorstUs = sSumUs = 0;
}

static void statsFrame(void)
{
	uint64 n = nowUs();
	if (lastFrameUs) {
		uint64 dt = n - lastFrameUs;
		sFrames++;
		sSumUs += dt;
		if (dt > sWorstUs) sWorstUs = dt;
		if (dt > 20000) sSlow++;
		if (dt > 50000) sVerySlow++;
	}
	lastFrameUs = n;
}

static void logStep(const char *result)
{
	uint32 used = 0, total = 0, blocks = 0;
	rw::ps3::vramStats(&used, &total, &blocks);
	CVector pos = FindPlayerCoors();
	float fps = sSumUs ? sFrames * 1000000.0f / sSumUs : 0.0f;
	struct mallinfo mi = mallinfo();
	double heapMB = mi.uordblks / 1048576.0;
	double delta = lastHeapMB > 0.0 ? heapMB - lastHeapMB : 0.0;
	lastHeapMB = heapMB;
	PS3_Logf("[autotest] %d/%d %-40s %s | %.1f s, %.1f fps, %u frames >20 ms, %u >50 ms, worst %.0f ms | "
	         "at (%.0f, %.0f, %.0f) %s | VRAM %u MB, streaming %.1f MB, heap in use %.2f MB (%+.2f), peds %d, cars %d",
	         loop + 1, loops, steps[cur].name, result,
	         (nowUs() - stepStartUs) / 1000000.0f, fps, sSlow, sVerySlow, sWorstUs / 1000.0f,
	         pos.x, pos.y, pos.z, levelName(levelAt(pos.x, pos.y)),
	         used >> 20, CStreaming::ms_memoryUsed / 1048576.0f, heapMB, delta,
	         CPools::GetPedPool()->GetNoOfUsedSpaces(), CPools::GetVehiclePool()->GetNoOfUsedSpaces());
}

static void logScripts(const char *tag)
{
	char names[200] = "";
	int n = 0;
	for (CRunningScript *s = CTheScripts::pActiveScripts; s; s = s->GetNext()) {
		if (n++ < 16) {
			char nm[9];
			memcpy(nm, s->m_abScriptName, 8);
			nm[8] = '\0';
			strncat(names, " ", sizeof(names) - strlen(names) - 1);
			strncat(names, nm, sizeof(names) - strlen(names) - 1);
		}
	}
	PS3_Logf("[autotest]     %s: on a mission %d, wanted %d, money %d, scripts %d:%s", tag,
	         CTheScripts::IsPlayerOnAMission(), FindPlayerPed() ? FindPlayerPed()->m_pWanted->m_nWantedLevel : -1,
	         CWorld::Players[CWorld::PlayerInFocus].m_nMoney, n, names);
}

// ---- the plan -----------------------------------------------------------------

struct Waypoint { float x, y; char name[40]; int level; };

static int cmpWaypoint(const void *a, const void *b)
{
	const Waypoint *wa = (const Waypoint*)a, *wb = (const Waypoint*)b;
	// Portland -> Staunton -> Shoreside, and west to east inside each
	static const int order[4] = { 9, 0, 1, 2 };
	int la = order[wa->level & 3], lb = order[wb->level & 3];
	if (la != lb) return la - lb;
	return wa->x < wb->x ? -1 : wa->x > wb->x ? 1 : 0;
}

static void buildPlan(void)
{
	static const int weathers[4] = { WEATHER_SUNNY, WEATHER_CLOUDY, WEATHER_RAINY, WEATHER_FOGGY };
	static const char *weatherNames[4] = { "sunny", "cloudy", "rain", "fog" };
	static const int hours[4] = { 0, 6, 12, 19 };

	numSteps = 0;

	if (leakMode) {
		// one kind of action at a time, several times in a row: the one
		// whose "heap in use" keeps climbing is what leaks. Streaming makes
		// the number move too, so it is the trend that counts, not one step
		CVector here = startPos;
		for (int i = 0; i < 6; i++)
			addStep(ST_SAVELOAD, 0, 0, 0, 0, "leak test: save + load %d", i + 1);
		for (int i = 0; i < 3; i++) {
			addStep(ST_FLY, 0, 0, 920.0f, -690.0f, "leak test: fly to Chinatown %d", i + 1);
			addStep(ST_FLY, 0, 0, here.x, here.y, "leak test: fly back %d", i + 1);
		}
		for (int i = 0; i < 2; i++) {
			addStep(ST_FLY, 0, 0, 100.0f, -900.0f, "leak test: fly to Staunton %d", i + 1);
			addStep(ST_FLY, 0, 0, here.x, here.y, "leak test: fly back to Portland %d", i + 1);
		}
		for (int i = 0; i < 3; i++)
			addStep(ST_SIDEMISSION, MI_TAXI, 0, 0, 0, "leak test: taxi mission %d", i + 1);
		for (uint32 g = 0; g < CGarages::NumGarages; g++)
			if (CGarages::aGarages[g].m_eGarageType == GARAGE_RESPRAY) {
				for (int i = 0; i < 4; i++)
					addStep(ST_GARAGE, g, 0, 0, 0, "leak test: car in and out of a garage %d", i + 1);
				break;
			}
		for (int i = 0; i < 3; i++)
			addStep(ST_SAVELOAD, 0, 0, 0, 0, "leak test: save + load again %d", i + 1);
		addStep(ST_LOOPEND, 0, 0, 0, 0, "end of loop");
		PS3_Logf("[autotest] plan: leak test, %d steps, %d loops", numSteps, loops);
		return;
	}

	if (fullMode) {
		// 1-2. every mission script of main.scm (story, phone / side
		// missions, RC, odd jobs...), as the debug menu's mission switcher
		// starts them: x seconds each, cutscenes skipped with X. Each start
		// fails the one before (its death/arrest cleanup, as a real fail)
		int n = CTheScripts::NumberOfMissionScripts;
		int hi = toMission >= 0 && toMission < n - 1 ? toMission : n - 1;
		for (int i = fromMission; i <= hi && numSteps < MAX_STEPS - 16; i++)
			addStep(ST_MISSION, i, 0, 30.0f, 0, "mission %d of %d", i, n - 1);
		addStep(ST_MISSIONEND, 0, 0, 0, 0, "end the last mission");
		// 3. odd jobs started the player's way (vehicle + R3), 30 s each
		addStep(ST_SIDEMISSION, MI_TAXI, 0, 30.0f, 0, "odd job: taxi");
		addStep(ST_SIDEMISSION, MI_AMBULAN, 0, 30.0f, 0, "odd job: paramedic");
		addStep(ST_SIDEMISSION, MI_FIRETRUCK, 0, 30.0f, 0, "odd job: firefighter");
		addStep(ST_SIDEMISSION, MI_POLICE, 0, 30.0f, 0, "odd job: vigilante");
		addStep(ST_PICKUP, 1, 0, 30.0f, 0, "rampage");
		// 4. one of each collectible
		addStep(ST_PICKUP, 0, 0, 8.0f, 0, "hidden package");
		for (int t = GARAGE_COLLECTCARS_1; t <= GARAGE_COLLECTCARS_3; t++)
			for (uint32 g = 0; g < CGarages::NumGarages; g++)
				if (CGarages::aGarages[g].m_eGarageType == t) {
					addStep(ST_GARAGE, g, 1, 0, 0, "import/export garage %u (list %d)", g, t - GARAGE_COLLECTCARS_1 + 1);
					break;
				}
		for (uint32 g = 0; g < CGarages::NumGarages; g++)
			if (CGarages::aGarages[g].m_eGarageType == GARAGE_RESPRAY) {
				addStep(ST_GARAGE, g, 0, 0, 0, "Pay 'n' Spray %u", g);
				break;
			}
		for (uint32 g = 0; g < CGarages::NumGarages; g++)
			if (CGarages::aGarages[g].m_eGarageType == GARAGE_BOMBSHOP1) {
				addStep(ST_GARAGE, g, 0, 0, 0, "bomb shop %u", g);
				break;
			}
		addStep(ST_SAVELOAD, 0, 0, 0, 0, "save + load at the end");
		addStep(ST_LOOPEND, 0, 0, 0, 0, "end of loop");
		PS3_Logf("[autotest] plan: full test, missions %d..%d of %d, %d steps", fromMission, hi, n - 1, numSteps);
		return;
	}

	if (gpuMode) {
		// what the RSX costs, where the player is (outdoors): the flips go
		// on hsync for the test, so the fps is what the RSX gives (above 60,
		// with tearing) and every change shows. Smoke: black car-fire smoke,
		// a steady cloud 10 m in front of the camera. ZCULL off = the depth
		// buffer as before patch 29 (the tiling itself stays: notiles.txt
		// compares that, a run with and a run without)
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 8.0f, 0, "gpu test: sunny (baseline)");
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 10.0f, 256, "gpu test: sunny, ZCULL off");
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 12.0f, 128, "gpu test: sunny + smoke");
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 10.0f, 128 | 256, "gpu test: sunny + smoke, ZCULL off");
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 10.0f, 128 | 512, "gpu test: sunny + smoke, faint skipped");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 12.0f, 0, "gpu test: rain");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 12.0f, 128, "gpu test: rain + smoke");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 10.0f, 128 | 256, "gpu test: rain + smoke, ZCULL off");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 10.0f, 128 | 512, "gpu test: rain + smoke, faint skipped");
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 10.0f, 0, "gpu test: sunny again");
		addStep(ST_LOOPEND, 0, 0, 0, 0, "end of loop");
		rw::ps3::setFlipVsync(false);
		PS3_Logf("[autotest] plan: gpu test, %d steps, %d loops; tiling %s", numSteps, loops,
		         rw::ps3::rsxTiling ? "on" : "OFF (notiles.txt)");
		return;
	}

	if (rainMode) {
		// the rain on its own, standing where the player is (outdoors):
		// dry, rain, dry again, twice; the [gpu] lines say what the RSX does
		// each rain effect off in turn: the one whose absence brings the
		// fps back is the culprit. The RSX timers go on only at the end
		// (if they hang the RSX, the rest of the data is in the log already)
		// patch 26 found the rain streaks (a dozen screen-sized additive
		// quads); patch 27 kills their empty fragments. Old way vs new way:
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 10.0f, 0, "rain test: sunny (dry baseline)");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 15.0f, 64, "rain test: rain, as patch 26 (no kill)");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 15.0f, 0, "rain test: rain, empty fragments killed");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 10.0f, 1, "rain test: rain, no rain streaks");
		addStep(ST_WEATHER, WEATHER_RAINY, -1, 10.0f, 4, "rain test: rain, no splashes");
		addStep(ST_WEATHER, WEATHER_SUNNY, -1, 10.0f, 0, "rain test: sunny again");
		addStep(ST_LOOPEND, 0, 0, 0, 0, "end of loop");
		PS3_Logf("[autotest] plan: rain test, %d steps, %d loops", numSteps, loops);
		return;
	}

	// 1. weather and time of day, where the player is
	for (int w = 0; w < 4; w++)
		for (int h = 0; h < 4; h++)
			addStep(ST_WEATHER, weathers[w], hours[h], 0, 0, "weather %s %02d:00", weatherNames[w], hours[h]);

	// 2. the islands: every restart point and garage the script made, plus
	// a few fixed places, so the three islands are always there
	static Waypoint wp[128];
	int n = 0;
	for (int i = 0; i < CRestart::NumberOfHospitalRestarts && n < 120; i++) {
		CVector &v = CRestart::HospitalRestartPoints[i];
		wp[n].x = v.x; wp[n].y = v.y; snprintf(wp[n].name, sizeof(wp[n].name), "hospital %d", i); n++;
	}
	for (int i = 0; i < CRestart::NumberOfPoliceRestarts && n < 120; i++) {
		CVector &v = CRestart::PoliceRestartPoints[i];
		wp[n].x = v.x; wp[n].y = v.y; snprintf(wp[n].name, sizeof(wp[n].name), "police station %d", i); n++;
	}
	for (uint32 i = 0; i < CGarages::NumGarages && n < 120; i++) {
		CGarage &g = CGarages::aGarages[i];
		if (!g.IsUsed())
			continue;
		wp[n].x = g.GetGarageCenterX() + 12.0f;	// in front of it, not inside
		wp[n].y = g.GetGarageCenterY();
		snprintf(wp[n].name, sizeof(wp[n].name), "garage %u (type %d)", i, g.m_eGarageType);
		n++;
	}
	static const struct { float x, y; const char *name; } fixed[] = {
		{ 890.0f, -310.0f, "Portland, Harwood" },
		{ 920.0f, -690.0f, "Portland, Chinatown" },
		{ 100.0f, -900.0f, "Staunton, Commercial" },
		{ -50.0f, 150.0f, "Staunton, Aspatria" },
		{ -1100.0f, -600.0f, "Shoreside, airport" },
		{ -700.0f, 50.0f, "Shoreside, Cedar Grove" },
	};
	for (int i = 0; i < ARRAY_SIZE(fixed); i++) {
		wp[n].x = fixed[i].x; wp[n].y = fixed[i].y;
		snprintf(wp[n].name, sizeof(wp[n].name), "%s", fixed[i].name);
		n++;
	}
	for (int i = 0; i < n; i++)
		wp[i].level = levelAt(wp[i].x, wp[i].y);
	qsort(wp, n, sizeof(wp[0]), cmpWaypoint);

	int lastLevel = -1;
	bool savedStaunton = false, savedShoreside = false;
	for (int i = 0; i < n; i++) {
		addStep(ST_FLY, 0, 0, wp[i].x, wp[i].y, "fly to %s (%s)", wp[i].name, levelName(wp[i].level));
		// 3. save and load once on Staunton and once on Shoreside
		if (wp[i].level == LEVEL_COMMERCIAL && !savedStaunton && lastLevel == LEVEL_COMMERCIAL) {
			addStep(ST_SAVELOAD, 0, 0, 0, 0, "save + load on Staunton");
			savedStaunton = true;
		}
		if (wp[i].level == LEVEL_SUBURBAN && !savedShoreside && lastLevel == LEVEL_SUBURBAN) {
			addStep(ST_SAVELOAD, 0, 0, 0, 0, "save + load on Shoreside");
			savedShoreside = true;
		}
		lastLevel = wp[i].level;
	}
	// the Dodo, at the airport
	addStep(ST_DODO, 0, 0, -1100.0f, -600.0f, "Dodo take-off at the airport");

	// back to Portland: 4. side missions and garages
	addStep(ST_FLY, 0, 0, 890.0f, -310.0f, "fly back to Portland, Harwood");
	addStep(ST_SIDEMISSION, MI_TAXI, 0, 0, 0, "side mission: taxi");
	addStep(ST_SIDEMISSION, MI_AMBULAN, 0, 0, 0, "side mission: ambulance");
	addStep(ST_SIDEMISSION, MI_FIRETRUCK, 0, 0, 0, "side mission: fire truck");
	addStep(ST_SIDEMISSION, MI_POLICE, 0, 0, 0, "side mission: vigilante");
	for (uint32 i = 0; i < CGarages::NumGarages; i++) {
		CGarage &g = CGarages::aGarages[i];
		const char *what = nil;
		switch (g.m_eGarageType) {
		case GARAGE_RESPRAY: what = "Pay 'n' Spray"; break;
		case GARAGE_BOMBSHOP1: case GARAGE_BOMBSHOP2: case GARAGE_BOMBSHOP3: what = "bomb shop"; break;
		case GARAGE_COLLECTCARS_1: case GARAGE_COLLECTCARS_2: case GARAGE_COLLECTCARS_3: what = "import/export"; break;
		case GARAGE_COLLECTSPECIFICCARS: what = "car list garage"; break;
		case GARAGE_CRUSHER: what = "crusher"; break;
		}
		if (what)
			addStep(ST_GARAGE, i, 0, 0, 0, "garage %u: %s (%s)", i, what,
			        levelName(levelAt(g.GetGarageCenterX(), g.GetGarageCenterY())));
	}
	addStep(ST_LOOPEND, 0, 0, 0, 0, "end of loop");
	PS3_Logf("[autotest] plan: %d steps per loop, %d loops, %d waypoints", numSteps, loops, n);
}

// ---- the steps ------------------------------------------------------------------

static void startStep(void)
{
	phase = 0;
	phaseStartUs = stepStartUs = nowUs();
	statsReset();
	Step &s = steps[cur];
	PS3_Logf("[autotest] %d/%d step %d/%d: %s", loop + 1, loops, cur + 1, numSteps, s.name);
	PS3_CRUMB("autotest step (loop*1000 + step)", loop * 1000 + cur + 1);
}

static void finishStep(const char *result)
{
	logStep(result);
	if (strncmp(result, "FAIL", 4) == 0)
		failures++;
	cur++;
	if (cur < numSteps)
		startStep();
}

// a step that waits: true when the time is up
static bool waited(float secs) { return phaseSecs() >= secs; }

static void runWeather(Step &s)
{
	if (phase == 0) {
		CWeather::ForceWeatherNow(s.a);
		if (s.b >= 0)
			CClock::SetGameClock(s.b, 0);
		gPS3FxOff = (int)s.y & 31;
		rw::ps3::additiveKill = ((int)s.y & 64) == 0;
		smokeOn = ((int)s.y & 128) != 0;
		rw::ps3::zcullOn = ((int)s.y & 256) == 0;
		rw::ps3::blendAlphaRef = ((int)s.y & 512) ? 8 : 0;
		smokeLastUs = nowUs();
		nextPhase();
	} else if (waited(s.x > 0.0f ? s.x : 4.0f)) {
		smokeOn = 0;
		finishStep("ok");
	} else if (smokeOn) {
		// ~150 puffs a second whatever the frame rate
		uint64 n = nowUs();
		int count = (int)((n - smokeLastUs) / 6666);
		if (count > 0) {
			smokeLastUs += (uint64)count * 6666;
			if (count > 20) count = 20;
			CVector fwd = TheCamera.GetForward();
			fwd.z = 0.0f;
			fwd.Normalise();
			CVector base = TheCamera.GetPosition() + fwd * 10.0f;
			for (int i = 0; i < count; i++) {
				CVector p = base + CVector(CGeneral::GetRandomNumberInRange(-3.0f, 3.0f),
				                           CGeneral::GetRandomNumberInRange(-3.0f, 3.0f),
				                           CGeneral::GetRandomNumberInRange(-2.0f, 1.0f));
				CParticle::AddParticle(PARTICLE_CARFLAME_SMOKE, p, CVector(0.0f, 0.0f, 0.02f));
			}
		}
	}
}

static void runFly(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil) { finishStep("FAIL no player"); return; }
	switch (phase) {
	case 0:
		deleteTestCar();
		if (p->bInVehicle)
			warpOutOfCar(p->GetPosition());
		flyFrom = p->GetPosition();
		flyFrom.z = Max(flyFrom.z, FLY_HEIGHT);
		flyTo = CVector(s.x, s.y, FLY_HEIGHT);
		flyDist = (flyTo - flyFrom).Magnitude2D();
		flyDone = 0.0f;
		p->bUsesCollision = false;
		p->bAffectedByGravity = false;
		nextPhase();
		break;
	case 1: {
		// the flight: streaming has to keep up by itself (no LoadScene)
		flyDone += FLY_SPEED;
		float t = flyDist > 0.0f ? Min(flyDone / flyDist, 1.0f) : 1.0f;
		CVector pos = flyFrom + (flyTo - flyFrom) * t;
		pos.z = FLY_HEIGHT;
		p->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		p->Teleport(pos);
		if (t >= 1.0f)
			nextPhase();
		break;
	}
	case 2: {
		// landing
		p->bUsesCollision = true;
		p->bAffectedByGravity = true;
		float gz;
		if (groundAt(s.x, s.y, &gz)) {
			warpOutOfCar(CVector(s.x, s.y, gz + 0.5f));
			nextPhase();
		} else {
			// water: stay in the air a moment, then go on
			p->bUsesCollision = false;
			p->bAffectedByGravity = false;
			phase = 4;
			phaseStartUs = nowUs();
		}
		break;
	}
	case 3:
		if (waited(3.0f))
			finishStep("ok");
		break;
	case 4:
		if (waited(1.0f))
			finishStep("ok (no ground there: water)");
		break;
	}
}

static void runSaveLoad(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	switch (phase) {
	case 0:
		if (p == nil) { finishStep("FAIL no player"); return; }
		if (p->bInVehicle)
			warpOutOfCar(p->GetPosition());
		savedPos = p->GetPosition();
		savedLevel = CGame::currLevel;
		nextPhase();
		break;
	case 1:
		if (!waited(1.0f))
			break;
		if (!PcSaveHelper.SaveSlot(AUTOTEST_SLOT)) {
			PS3_Logf("[autotest]     save failed, error %d", PcSaveHelper.nErrorCode);
			finishStep("FAIL save");
			return;
		}
		PS3_Logf("[autotest]     saved at (%.1f, %.1f, %.1f), level %d", savedPos.x, savedPos.y, savedPos.z, savedLevel);
		nextPhase();
		break;
	case 2:
		if (!waited(1.0f))
			break;
		wantLoad = 1;	// ps3.cpp does it between two frames
		waitingForLoad = 1;
		nextPhase();
		break;
	case 3:
		if (waitingForLoad)
			break;
		// loaded: give the game a moment to fade in
		if (!waited(4.0f))
			break;
		if (p == nil) { finishStep("FAIL no player after the load"); return; }
		{
			float d = (p->GetPosition() - savedPos).Magnitude();
			PS3_Logf("[autotest]     loaded: player at (%.1f, %.1f, %.1f), %.1f m from where it was saved, level %d",
			         p->GetPosition().x, p->GetPosition().y, p->GetPosition().z, d, (int)CGame::currLevel);
			finishStep(d < 10.0f && CGame::currLevel == savedLevel ? "ok" : "FAIL position or level differs");
		}
		break;
	}
}

static void runSideMission(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil) { finishStep("FAIL no player"); return; }
	switch (phase) {
	case 0: {
		deleteTestCar();
		CVector pos = p->GetPosition() + CVector(4.0f, 0.0f, 0.0f);
		float gz;
		if (groundAt(pos.x, pos.y, &gz))
			pos.z = gz;
		testCar = makeCar(s.a, pos, 0.0f);
		if (testCar == nil) { finishStep("FAIL no car"); return; }
		warpIntoCar(testCar);
		nextPhase();
		break;
	}
	case 1:
		if (waited(2.0f)) {
			logScripts("before R3");
			pressR3 = 6;	// frames
			nextPhase();
		}
		break;
	case 2:
		if (waited(s.x > 0.0f ? s.x : 20.0f)) {
			logScripts("after R3");
			pressR3 = 6;	// cancel it, as a player would
			nextPhase();
		}
		break;
	case 3:
		if (waited(5.0f)) {
			logScripts("after the second R3");
			deleteTestCar();
			finishStep("ok (see the scripts lines)");
		}
		break;
	}
}

static void runGarage(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil) { finishStep("FAIL no player"); return; }
	CGarage &g = CGarages::aGarages[s.a];
	switch (phase) {
	case 0: {
		deleteTestCar();
		float cx = g.GetGarageCenterX(), cy = g.GetGarageCenterY();
		float gz;
		if (!groundAt(cx, cy, &gz))
			gz = g.m_fZ1;
		int model = MI_KURUMA;
		if (s.b == 1 && g.m_eGarageType >= GARAGE_COLLECTCARS_1 && g.m_eGarageType <= GARAGE_COLLECTCARS_3) {
			// import/export: a car of its list it doesn't have yet
			int ct = CGarages::GetCarsCollectedIndexForGarageType(g.m_eGarageType);
			for (int i = 0; i < TOTAL_COLLECTCARS_CARS; i++)
				if (!(CGarages::CarTypesCollected[ct] & BIT(i))) {
					extern const int32 gaCarsToCollectInCraigsGarages[TOTAL_COLLECTCARS_GARAGES][TOTAL_COLLECTCARS_CARS];
					model = gaCarsToCollectInCraigsGarages[ct][i];
					break;
				}
			PS3_Logf("[autotest]     import/export: model %d, collected so far 0x%x", model, CGarages::CarTypesCollected[ct]);
		}
		testCar = makeCar(model, CVector(cx, cy, gz), 0.0f);
		if (testCar == nil) { finishStep("FAIL no car"); return; }
		warpIntoCar(testCar);
		if (g.m_eGarageType == GARAGE_RESPRAY)
			p->SetWantedLevel(2);
		PS3_Logf("[autotest]     garage type %d state %d, car inside at (%.0f, %.0f, %.0f)",
		         g.m_eGarageType, g.m_eGarageState, cx, cy, gz);
		nextPhase();
		break;
	}
	case 1:
		if (waited(12.0f)) {
			PS3_Logf("[autotest]     after 12 s: garage state %d, wanted %d, respray %d, money %d",
			         g.m_eGarageState, p->m_pWanted->m_nWantedLevel, g.m_bResprayHappened,
			         CWorld::Players[CWorld::PlayerInFocus].m_nMoney);
			if (g.m_eGarageType >= GARAGE_COLLECTCARS_1 && g.m_eGarageType <= GARAGE_COLLECTCARS_3)
				PS3_Logf("[autotest]     import/export collected now 0x%x",
				         CGarages::CarTypesCollected[CGarages::GetCarsCollectedIndexForGarageType(g.m_eGarageType)]);
			deleteTestCar();
			clearWanted();
			finishStep("ok");
		}
		break;
	}
}

static void runDodo(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil) { finishStep("FAIL no player"); return; }
	switch (phase) {
	case 0: {
		deleteTestCar();
		float gz;
		if (!groundAt(s.x, s.y, &gz)) { finishStep("FAIL no ground at the airport"); return; }
		testCar = makeCar(MI_DODO, CVector(s.x, s.y, gz + 0.5f), 0.0f);
		if (testCar == nil) { finishStep("FAIL no Dodo"); return; }
		warpIntoCar(testCar);
		dodoMaxZ = gz;
		nextPhase();
		break;
	}
	case 1:
		pressCross = 1;		// full throttle
		if (phaseSecs() > 5.0f)
			pullUp = 1;	// stick back
		if (testCar->GetPosition().z > dodoMaxZ)
			dodoMaxZ = testCar->GetPosition().z;
		if (waited(12.0f)) {
			pressCross = pullUp = 0;
			PS3_Logf("[autotest]     Dodo: highest %.1f m, speed %.1f m/s, health %.0f",
			         dodoMaxZ, testCar->GetMoveSpeed().Magnitude() * 50.0f, testCar->m_fHealth);
			deleteTestCar();
			finishStep("ok");
		}
		break;
	}
}

static const char *missionName(char *buf)
{
	for (CRunningScript *sc = CTheScripts::pActiveScripts; sc; sc = sc->GetNext())
		if (sc->m_bIsMissionScript) {
			memcpy(buf, sc->m_abScriptName, 8);
			buf[8] = '\0';
			return buf;
		}
	strcpy(buf, "(none)");
	return buf;
}

static void runMission(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil) { finishStep("FAIL no player"); return; }
	char name[16];
	switch (phase) {
	case 0: {
		deleteTestCar();
		if (p->bInVehicle)
			warpOutOfCar(p->GetPosition() + CVector(0.0f, 0.0f, 1.0f));
		// the odd jobs start with the player in their vehicle (their main
		// thread launches them so): their name is the first command
		char mname[9] = "";
		{
			uint8 head[16];
			CFileMgr::ChangeDir("\\");
			int fh = CFileMgr::OpenFile("data\\main.scm", "rb");
			if (fh) {
				CFileMgr::Seek(fh, CTheScripts::MultiScriptArray[s.a], 0);
				if (CFileMgr::Read(fh, (char*)head, 16) == 16 && head[0] == 0xA4 && head[1] == 0x03) {
					memcpy(mname, head + 2, 8);
					mname[8] = '\0';
				}
				CFileMgr::CloseFile(fh);
			}
		}
		static const struct { const char *name; int model; } jobs[] = {
			{ "rc1", MI_TOYZ }, { "rc2", MI_TOYZ }, { "rc3", MI_TOYZ }, { "rc4", MI_TOYZ },
			{ "t4x4_1", MI_PATRIOT }, { "t4x4_2", MI_LANDSTAL }, { "t4x4_3", MI_BOBCAT },
			{ "mayhem", MI_STALLION }, { "ambulae", MI_AMBULAN }, { "firetru", MI_FIRETRUCK },
			{ "copcar", MI_POLICE }, { "taxi", MI_TAXI },
		};
		int model = -1;
		for (int i = 0; i < ARRAY_SIZE(jobs); i++)
			if (strcasecmp(mname, jobs[i].name) == 0)
				model = jobs[i].model;
		if (model >= 0) {
			CVector pos = p->GetPosition() + CVector(4.0f, 0.0f, 0.0f);
			float gz;
			if (groundAt(pos.x, pos.y, &gz))
				pos.z = gz;
			testCar = makeCar(model, pos, 0.0f);
			if (testCar)
				warpIntoCar(testCar);
		}
		PS3_Logf("[autotest]     mission %d is \"%s\"%s", s.a, mname, model >= 0 ? ", started in its vehicle" : "");
		CTheScripts::SwitchToMission(s.a);
		testCar = nil;	// the mission's now (its cleanup may delete it)
		nextPhase();
		break;
	}
	case 1:
		if (CCutsceneMgr::IsRunning() && phaseSecs() > 2.0f && pulseCross == 0)
			pulseCross = 20;
		if (waited(s.x)) {
			if (CCutsceneMgr::IsRunning())
				nextPhase();	// let it end first
			else
				phase = 3;
		}
		break;
	case 2:
		if (CCutsceneMgr::IsRunning() && pulseCross == 0)
			pulseCross = 20;
		if (!CCutsceneMgr::IsRunning() || waited(20.0f))
			phase = 3;
		break;
	case 3:
		PS3_Logf("[autotest]     script %s, on a mission %d, cutscene %d, player at (%.0f, %.0f, %.0f) %s",
		         missionName(name), CTheScripts::IsPlayerOnAMission(), CCutsceneMgr::IsRunning(),
		         p->GetPosition().x, p->GetPosition().y, p->GetPosition().z, p->bInVehicle ? "in a vehicle" : "on foot");
		finishStep(CCutsceneMgr::IsRunning() ? "ok (a cutscene still running)" : "ok");
		break;
	}
}

static void runMissionEnd(Step &s)
{
	(void)s;
	switch (phase) {
	case 0:
		if (CCutsceneMgr::IsRunning()) {
			if (pulseCross == 0) pulseCross = 20;
			if (!waited(20.0f)) break;
		}
		CTheScripts::EndMissionScripts();
		nextPhase();
		break;
	case 1:
		if (waited(5.0f)) {
			logScripts("after the end");
			finishStep(CTheScripts::IsPlayerOnAMission() ? "FAIL still on a mission" : "ok");
		}
		break;
	}
}

static void runPickup(Step &s)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil) { finishStep("FAIL no player"); return; }
	switch (phase) {
	case 0: {
		deleteTestCar();
		int found = -1;
		for (int i = 0; i < NUMPICKUPS && found < 0; i++) {
			CPickup &pk = CPickups::aPickUps[i];
			if (pk.m_eType == PICKUP_NONE || pk.m_bRemoved)
				continue;
			if (s.a == 0 ? pk.m_eType == PICKUP_COLLECTABLE1 : pk.m_eModelIndex == MI_PICKUP_KILLFRENZY)
				found = i;
		}
		if (found < 0) { finishStep(s.a == 0 ? "skipped: no hidden package left" : "skipped: no rampage pickup"); return; }
		CVector pos = CPickups::aPickUps[found].m_vecPos;
		packagesBefore = CWorld::Players[CWorld::PlayerInFocus].m_nCollectedPackages;
		PS3_Logf("[autotest]     pickup %d at (%.0f, %.0f, %.0f)", found, pos.x, pos.y, pos.z);
		CStreaming::LoadScene(pos);
		warpOutOfCar(pos);
		nextPhase();
		break;
	}
	case 1:
		if (waited(s.x)) {
			if (s.a == 0) {
				int now = CWorld::Players[CWorld::PlayerInFocus].m_nCollectedPackages;
				PS3_Logf("[autotest]     hidden packages %d -> %d", packagesBefore, now);
				finishStep(now > packagesBefore ? "ok" : "FAIL not collected");
			} else {
				bool on = CDarkel::FrenzyOnGoing();
				PS3_Logf("[autotest]     rampage going on %d, status %d", on, CDarkel::ReadStatus());
				if (on)
					CDarkel::ResetOnPlayerDeath();	// over, as if the player had died
				finishStep(on ? "ok" : "FAIL no rampage started");
			}
		}
		break;
	}
}

static void runLoopEnd(void)
{
	PS3_Logf("[autotest] ---- loop %d of %d done in %.1f min, %d failures so far ----",
	         loop + 1, loops, (nowUs() - runStartUs) / 60000000.0f, failures);
	PS3_LogMemTag("[autotest]    ");
	cur++;
}

// ---- start / end ----------------------------------------------------------------

static void finishRun(void)
{
	deleteTestCar();
	gPS3FxOff = 0;
	rw::ps3::additiveKill = true;
	rw::ps3::zcullOn = true;
	rw::ps3::blendAlphaRef = 0;
	rw::ps3::setFlipVsync(true);
	smokeOn = 0;
	CWeather::ReleaseWeather();
	releasePlayer();
	clearWanted();
	CPlayerPed *p = FindPlayerPed();
	if (p) {
		float gz;
		if (groundAt(startPos.x, startPos.y, &gz))
			startPos.z = gz + 0.5f;
		warpOutOfCar(startPos);
		p->m_fRotationCur = p->m_fRotationDest = startHeading;
	}
	PS3_Logf("[autotest] ==== DONE: %d loops in %.1f min, %d failures ====",
	         loops, (nowUs() - runStartUs) / 60000000.0f, failures);
	PS3_LogMemTag("[autotest]    ");
	rename(AUTOTEST_FILE, AUTOTEST_DONE_FILE);
	state = 3;
}

static bool readConfig(void)
{
	FILE *f = fopen(AUTOTEST_FILE, "rb");
	if (f == nil)
		return false;
	char line[128];
	loops = 1;
	while (fgets(line, sizeof(line), f)) {
		int v;
		if (sscanf(line, "loops=%d", &v) == 1 && v >= 1 && v <= 50)
			loops = v;
		if (strncmp(line, "mode=rain", 9) == 0)
			rainMode = 1;
		if (strncmp(line, "mode=leak", 9) == 0)
			leakMode = 1;
		if (strncmp(line, "mode=gpu", 8) == 0)
			gpuMode = 1;
		if (strncmp(line, "mode=full", 9) == 0)
			fullMode = 1;
		if (sscanf(line, "from=%d", &v) == 1 && v >= 0)
			fromMission = v;
		if (sscanf(line, "to=%d", &v) == 1 && v >= 0)
			toMission = v;
	}
	fclose(f);
	return true;
}

static bool canStart(void)
{
	CPlayerPed *p = FindPlayerPed();
	if (p == nil || FrontEndMenuManager.m_bMenuActive || CCutsceneMgr::IsRunning())
		return false;
	if (CTimer::GetTimeInMilliseconds() < 8000)
		return false;
	return true;
}

// ---- hooks ------------------------------------------------------------------------

// From CPad::UpdatePads (start of CGame::Process): runs the plan and puts
// the buttons it wants into the pad, before the scripts read it.
void
PS3_AutotestFrame(void)
{
	if (state == 0) {
		state = readConfig() ? 1 : 3;
		if (state == 1)
			PS3_Logf("[autotest] %s found: the test starts once a game is running", AUTOTEST_FILE);
	}
	if (state != 1 && state != 2)
		return;
	if (gGameState != GS_PLAYING_GAME || FrontEndMenuManager.m_bGameNotLoaded)
		return;

	if (state == 1) {
		if (!canStart())
			return;
		if (CTheScripts::IsPlayerOnAMission()) {
			static int warned;
			if (!warned++)
				PS3_Log("[autotest] waiting: a mission is running (load a save in a safehouse)");
			return;
		}
		CPlayerPed *p = FindPlayerPed();
		startPos = p->GetPosition();
		startHeading = p->m_fRotationCur;
		runStartUs = nowUs();
		loop = 0;
		failures = 0;
		buildPlan();
		PS3_LogMemTag("[autotest] start:");
		state = 2;
		cur = 0;
		startStep();
	}

	if (waitingForLoad || FrontEndMenuManager.m_bMenuActive)
		return;

	statsFrame();
	keepPlayerSafe();

	if (cur >= numSteps) {
		loop++;
		if (loop >= loops) {
			finishRun();
			return;
		}
		buildPlan();	// garages/restarts may have changed
		cur = 0;
		startStep();
	}

	Step &s = steps[cur];
	if (s.type != ST_GARAGE && s.type != ST_MISSION)
		clearWanted();
	switch (s.type) {
	case ST_WEATHER: runWeather(s); break;
	case ST_FLY: runFly(s); break;
	case ST_SAVELOAD: runSaveLoad(s); break;
	case ST_SIDEMISSION: runSideMission(s); break;
	case ST_GARAGE: runGarage(s); break;
	case ST_DODO: runDodo(s); break;
	case ST_LOOPEND: runLoopEnd(); if (cur < numSteps) startStep(); break;
	case ST_MISSION: runMission(s); break;
	case ST_MISSIONEND: runMissionEnd(s); break;
	case ST_PICKUP: runPickup(s); break;
	}

	// the buttons, over what the real pad says
	CPad *pad = CPad::GetPad(0);
	if (pressR3 > 0) {
		// pressed for a few frames, then released (scripts look for a press)
		pad->NewState.RightShock = pressR3 > 3 ? 255 : 0;
		pressR3--;
	}
	if (pressCross)
		pad->NewState.Cross = 255;
	if (pulseCross > 0) {
		// on for 3 frames, off for the rest: a fresh press each time
		pad->NewState.Cross = pulseCross > 17 ? 255 : 0;
		pulseCross--;
	}
	if (pullUp)
		pad->NewState.LeftStickY = 100;
}

// From ps3.cpp, between frames: a load the test asked for
int
PS3_AutotestWantsLoad(void)
{
	if (!wantLoad)
		return 0;
	wantLoad = 0;
	testCar = nil;	// the load throws all the cars away
	if (!CheckSlotDataValid(AUTOTEST_SLOT)) {
		PS3_Logf("[autotest]     the save doesn't check out (error %d)", PcSaveHelper.nErrorCode);
		waitingForLoad = 0;
		return 0;
	}
	FrontEndMenuManager.m_bWantToRestart = true;
	FrontEndMenuManager.m_bWantToLoad = true;
	b_FoundRecentSavedGameWantToLoad = true;
	return 1;
}

// From ps3.cpp, after CGame::InitialiseWhenRestarting
void
PS3_AutotestLoaded(void)
{
	if (waitingForLoad) {
		waitingForLoad = 0;
		phaseStartUs = nowUs();
		lastFrameUs = 0;
	}
}

#endif // PS3_STAGE != 1
