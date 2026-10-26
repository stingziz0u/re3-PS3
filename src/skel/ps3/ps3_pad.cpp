// ps3_pad.cpp -- the glfw* calls of the game's input code (see ps3_glfw.h),
// on the DualShock 3. No mouse and no keyboard.
//
// The pad code is Doom64-PS3's / Quake2PS3's (survived plugging, unplugging
// and powering pads off and on on hardware):
//  - ioPadGetInfo2 (per port status, bit 1 = "assignment changed");
//  - only the pad on port 1 (index 0) plays, as on the PS2;
//  - the data is NOT skipped when len == 0 (the buffer keeps the previous
//    poll; skipping freezes the sticks);
//  - if the library keeps failing, it is ended and started again.
//
// The game sees an SDL/GLFW style gamepad (Xbox layout, which re3 maps
// back to the PS2 buttons: A = cross, B = circle, X = square, Y = triangle).

#include "common.h"
#include "crossplatform.h"

#include <io/pad.h>
#include <sys/systime.h>

#include "ps3_platform.h"

#define STICK_CENTER 128
#define PLAY_PORT 0	// pad number 1

static padInfo2 info;
static padData data;
static int initialized;
static int connected;
static int port = -1;
static int nopad;

static unsigned char buttons[15];
static float axes[6];
// Pressure of each button, by the game's button id (ControllerConfig.cpp
// MapIdToButtonId: 1 circle, 2 cross, 3 square, 4 triangle, 5 L2, 6 R2,
// 7 L1, 8 R1, 13..16 d-pad up/right/down/left); 0 = no pressure sensor.
static unsigned char pressure[17];

// Gamepad settings > STICK SETTINGS: dead zones, percent / 5, 1..14 = 5%..70%,
// 6 = 30% (the 0.3 the game always had). An axis under it reads as centred;
// the right one also sets where the camera starts turning (Pad.cpp,
// PS3_LookAround: the game itself waited for 85 of 128 there).
int8 gPS3DeadzoneL = 6, gPS3DeadzoneR = 6;

float
PS3_PadDeadzone(bool right)
{
	int v = right ? gPS3DeadzoneR : gPS3DeadzoneL;
	if (v < 1 || v > 14)
		v = 6;
	return v * 0.05f;
}

static void
ReleaseAll(void)
{
	// "at rest", not zero: zero is a stick pushed all the way
	memset(&data, 0, sizeof(data));
	data.ANA_R_H = data.ANA_R_V = STICK_CENTER;
	data.ANA_L_H = data.ANA_L_V = STICK_CENTER;
}

void
PS3_PadInit(void)
{
	if (initialized)
		return;
	s32 ret = ioPadInit(7);
	PS3_Logf("[pad] ioPadInit ret=%d", (int)ret);
	PS3_Logf("[pad] dead zones: left %d%%, right %d%%", (int)(PS3_PadDeadzone(false) * 100.0f + 0.5f),
	         (int)(PS3_PadDeadzone(true) * 100.0f + 0.5f));
	if (ret < 0)
		return;
	initialized = 1;
	// pressure: L2/R2 read as analog triggers
	for (int i = 0; i < MAX_PORT_NUM; i++)
		ioPadSetPressMode(i, PAD_PRESS_MODE_ON);
	memset(&info, 0, sizeof(info));
	ReleaseAll();
	port = -1;
	connected = 0;
}

void
PS3_PadShutdown(void)
{
	if (!initialized)
		return;
	PS3_PadRumble(0);
	ioPadEnd();
	initialized = 0;
	connected = 0;
	port = -1;
}

static padData portData[MAX_PORT_NUM];	// each pad's last poll
static bool portOn[MAX_PORT_NUM];
static bool portWaitInput[MAX_PORT_NUM];	// log its first input
static int portFailures[MAX_PORT_NUM];

static void
CenterData(padData *d)
{
	memset(d, 0, sizeof(*d));
	d->ANA_R_H = d->ANA_R_V = STICK_CENTER;
	d->ANA_L_H = d->ANA_L_V = STICK_CENTER;
}

static void
PadReinit(const char *why)
{
	s32 a = ioPadEnd();
	s32 b = ioPadInit(7);
	for (int i = 0; i < MAX_PORT_NUM; i++) {
		ioPadSetPressMode(i, PAD_PRESS_MODE_ON);
		portOn[i] = false;
		portFailures[i] = 0;
		CenterData(&portData[i]);
	}
	PS3_Logf("[pad] ioPad restarted (%s): end=%d init=%d", why, (int)a, (int)b);
	port = -1;
	nopad = 0;
	connected = 0;
	ReleaseAll();
}

static int
StickOff(unsigned v)
{
	int d = (int)v - STICK_CENTER;
	return d < 0 ? -d : d;
}

static bool
HasInput(const padData &d)
{
	return d.BTN_CROSS || d.BTN_CIRCLE || d.BTN_SQUARE || d.BTN_TRIANGLE ||
	       d.BTN_L1 || d.BTN_R1 || d.BTN_L2 || d.BTN_R2 || d.BTN_L3 || d.BTN_R3 ||
	       d.BTN_START || d.BTN_SELECT || d.BTN_UP || d.BTN_DOWN || d.BTN_LEFT || d.BTN_RIGHT ||
	       StickOff(d.ANA_L_H) > 48 || StickOff(d.ANA_L_V) > 48 ||
	       StickOff(d.ANA_R_H) > 48 || StickOff(d.ANA_R_V) > 48;
}


static void
ReadPad(void)
{
	static u32 last_status[MAX_PORT_NUM];
	int i, pads = 0;
	s32 r;

	if (!initialized) {
		ReleaseAll();
		return;
	}

	r = ioPadGetInfo2(&info);
	if (r < 0) {
		if (connected)
			PS3_Logf("[pad] ioPadGetInfo2 failed (%d): pad lost", (int)r);
		connected = 0;
		ReleaseAll();
		if (++nopad == 300)
			PadReinit("GetInfo2 failing");
		return;
	}

	for (i = 0; i < MAX_PORT_NUM; i++) {
		if (info.port_status[i] != last_status[i]) {
			PS3_Logf("[pad] port %d: status 0x%x -> 0x%x", i,
			         (unsigned)last_status[i], (unsigned)info.port_status[i]);
			last_status[i] = info.port_status[i];
		}
		// assignment changed: drop that port's stale buffer
		if (info.port_status[i] & 2)
			ioPadClearBuf(i);

		// Ren: only the pad on port 1 plays (another pad, or this one
		// coming back as 2 after a drop-out, is logged and ignored)
		if (i != PLAY_PORT)
			continue;

		bool on = (info.port_status[i] & 1) != 0;
		if (on && !portOn[i]) {
			portOn[i] = true;
			portWaitInput[i] = true;
			portFailures[i] = 0;
			CenterData(&portData[i]);
			ioPadClearBuf(i);
			PS3_Logf("[pad] pad 1 connected: it plays");
		} else if (!on && portOn[i]) {
			portOn[i] = false;
			CenterData(&portData[i]);
			PS3_Log("[pad] pad 1 gone: no control until a pad is number 1 again");
		}
		if (!on)
			continue;
		pads++;

		// NOT skipped when len == 0 on purpose, see the header
		r = ioPadGetData(i, &portData[i]);
		if (r != 0) {
			if (portFailures[i] < 5)
				PS3_Logf("[pad] ioPadGetData(%d) = %d", i, (int)r);
			if (++portFailures[i] == 180) {
				PadReinit("GetData failing");
				return;
			}
			// keep the previous state: don't release buttons
			continue;
		}
		portFailures[i] = 0;
		if (HasInput(portData[i])) {
			if (portWaitInput[i]) {
				portWaitInput[i] = false;
				PS3_Log("[pad] pad 1: first input");
			}
		}
	}

	if (pads == 0) {
		connected = 0;
		port = -1;
		ReleaseAll();
		nopad++;
		return;
	}
	port = PLAY_PORT;
	connected = 1;
	nopad = 0;
	data = portData[PLAY_PORT];
}

static float
Stick(unsigned v)
{
	float f = ((float)v - 128.0f) / 127.0f;
	if (f < -1.0f) f = -1.0f;
	if (f > 1.0f) f = 1.0f;
	return f;
}

static float
Trigger(unsigned digital, unsigned pressure)
{
	// -1 released .. 1 pressed (GLFW)
	float p = pressure / 255.0f;
	if (digital && p < 0.5f)
		p = 1.0f;
	return p * 2.0f - 1.0f;
}

void
PS3_PadPoll(void)
{
	ReadPad();

	buttons[GLFW_GAMEPAD_BUTTON_A] = data.BTN_CROSS;
	buttons[GLFW_GAMEPAD_BUTTON_B] = data.BTN_CIRCLE;
	buttons[GLFW_GAMEPAD_BUTTON_X] = data.BTN_SQUARE;
	buttons[GLFW_GAMEPAD_BUTTON_Y] = data.BTN_TRIANGLE;
	buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = data.BTN_L1;
	buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = data.BTN_R1;
	buttons[GLFW_GAMEPAD_BUTTON_BACK] = data.BTN_SELECT;
	buttons[GLFW_GAMEPAD_BUTTON_START] = data.BTN_START;
	buttons[GLFW_GAMEPAD_BUTTON_GUIDE] = 0;
	buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] = data.BTN_L3;
	buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] = data.BTN_R3;
	buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] = data.BTN_UP;
	buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] = data.BTN_RIGHT;
	buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] = data.BTN_DOWN;
	buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] = data.BTN_LEFT;

	axes[GLFW_GAMEPAD_AXIS_LEFT_X] = Stick(data.ANA_L_H);
	axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = Stick(data.ANA_L_V);
	axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = Stick(data.ANA_R_H);
	axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = Stick(data.ANA_R_V);
	axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = Trigger(data.BTN_L2, data.PRE_L2);
	axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = Trigger(data.BTN_R2, data.PRE_R2);

	pressure[1] = data.PRE_CIRCLE;
	pressure[2] = data.PRE_CROSS;
	pressure[3] = data.PRE_SQUARE;
	pressure[4] = data.PRE_TRIANGLE;
	pressure[5] = data.PRE_L2;
	pressure[6] = data.PRE_R2;
	pressure[7] = data.PRE_L1;
	pressure[8] = data.PRE_R1;
	pressure[13] = data.PRE_UP;
	pressure[14] = data.PRE_RIGHT;
	pressure[15] = data.PRE_DOWN;
	pressure[16] = data.PRE_LEFT;
}

// How hard a held button is pressed, 1..255, by the game's button id: the
// accelerator and the brake of the PS2 games (ControllerConfig.cpp). Buttons
// without a sensor (select, start, L3, R3) and pads that don't report
// pressure (0 while the button is down) read 255, as before. The DS3's
// sensors rarely reach 255: the top eighth of the travel counts as full.
int
PS3_PadPressure(int button)
{
	if (button < 1 || button > 16 || pressure[button] == 0)
		return 255;
	int p = pressure[button] * 255 / 224;
	return p > 255 ? 255 : p;
}

// Raw DS3 buttons of the last poll, bit n = GLFW_GAMEPAD_BUTTON_n
unsigned
PS3_PadButtons(void)
{
	unsigned m = 0;
	for (int i = 0; i < (int)(sizeof(buttons) / sizeof(buttons[0])); i++)
		if (buttons[i])
			m |= 1u << i;
	return m;
}

// ---- GLFW ---------------------------------------------------------------------

void
glfwGetCursorPos(GLFWwindow *window, double *xpos, double *ypos)
{
	(void)window;
	// 0,0 = "no mouse" for CMousePointerStateHelper::GetMouseSetUp
	if (xpos) *xpos = 0.0;
	if (ypos) *ypos = 0.0;
}

void
glfwSetCursorPos(GLFWwindow *window, double xpos, double ypos)
{
	(void)window; (void)xpos; (void)ypos;
}

int
glfwGetMouseButton(GLFWwindow *window, int button)
{
	(void)window; (void)button;
	return GLFW_RELEASE;
}

int
glfwGetKey(GLFWwindow *window, int key)
{
	(void)window; (void)key;
	return GLFW_RELEASE;
}

void
glfwSetInputMode(GLFWwindow *window, int mode, int value)
{
	(void)window; (void)mode; (void)value;
}

int
glfwJoystickPresent(int jid)
{
	// joystick 1 is always there: a pad plugged in later just works
	return jid == GLFW_JOYSTICK_1 && initialized;
}

int
glfwJoystickIsGamepad(int jid)
{
	return glfwJoystickPresent(jid);
}

const char*
glfwGetJoystickName(int jid)
{
	return glfwJoystickPresent(jid) ? "DUALSHOCK 3" : nil;
}

const unsigned char*
glfwGetJoystickButtons(int jid, int *count)
{
	if (count)
		*count = glfwJoystickPresent(jid) ? 15 : 0;
	return buttons;
}

const float*
glfwGetJoystickAxes(int jid, int *count)
{
	if (count)
		*count = glfwJoystickPresent(jid) ? 6 : 0;
	return axes;
}

int
glfwGetGamepadState(int jid, GLFWgamepadstate *state)
{
	if (!glfwJoystickPresent(jid))
		return 0;
	memcpy(state->buttons, buttons, sizeof(state->buttons));
	memcpy(state->axes, axes, sizeof(state->axes));
	return 1;
}

// ---- vibration ----------------------------------------------------------------

// The PS2 game's scePadSetActDirect: small motor off, large motor = the shake
// frequency (CPad::ShakeFreq, 0 = still). Sent when it changes, and again
// every half second while it runs (a pad plugged in meanwhile gets it too)
void
PS3_PadRumble(int large)
{
	static int last = -1, lastPort = -1;
	static u64 lastSend;
	if (large < 0) large = 0;
	if (large > 255) large = 255;
	if (!initialized || !connected || port < 0) {
		last = -1;
		return;
	}
	u64 now = sysGetSystemTime() / 1000;
	if (large == last && port == lastPort && (large == 0 || now - lastSend < 500))
		return;
	padActParam act;
	memset(&act, 0, sizeof(act));
	act.small_motor = 0;
	act.large_motor = (u8)large;
	s32 r = (s32)ioPadSetActDirect(port, &act);
	static int logged;
	if (((large != 0) != (last > 0) && logged < 40) || (r != 0 && logged < 40)) {
		logged++;
		PS3_Logf("[pad] vibration %s (%d) on port %d: ioPadSetActDirect = 0x%x, pad capability 0x%x (vibration %s)",
		         large ? "on" : "off", large, port, (unsigned)r, (unsigned)info.device_capability[port],
		         info.device_capability[port] & 0x10 ? "yes" : "no");
	}
	last = large;
	lastPort = port;
	lastSend = now;
}
