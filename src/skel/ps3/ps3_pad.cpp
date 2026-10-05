// ps3_pad.cpp -- the glfw* calls of the game's input code (see ps3_glfw.h),
// on the DualShock 3. No mouse and no keyboard.
//
// The pad code is Doom64-PS3's / Quake2PS3's (survived plugging, unplugging
// and powering pads off and on on hardware):
//  - ioPadGetInfo2 (per port status, bit 1 = "assignment changed");
//  - the active port is resolved again every frame;
//  - the data is NOT skipped when len == 0 (the buffer keeps the previous
//    poll; skipping freezes the sticks);
//  - if the library keeps failing, it is ended and started again.
//
// The game sees an SDL/GLFW style gamepad (Xbox layout, which re3 maps
// back to the PS2 buttons: A = cross, B = circle, X = square, Y = triangle).

#include "common.h"
#include "crossplatform.h"

#include <io/pad.h>

#include "ps3_platform.h"

#define STICK_CENTER 128

static padInfo2 info;
static padData data;
static int initialized;
static int connected;
static int port = -1;
static int failures;
static int nopad;

static unsigned char buttons[15];
static float axes[6];

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
	ioPadEnd();
	initialized = 0;
	connected = 0;
	port = -1;
}

static void
PadReinit(const char *why)
{
	s32 a = ioPadEnd();
	s32 b = ioPadInit(7);
	for (int i = 0; i < MAX_PORT_NUM; i++)
		ioPadSetPressMode(i, PAD_PRESS_MODE_ON);
	PS3_Logf("[pad] ioPad restarted (%s): end=%d init=%d", why, (int)a, (int)b);
	port = -1;
	failures = 0;
	nopad = 0;
	connected = 0;
	ReleaseAll();
}

static void
ReadPad(void)
{
	static u32 last_status[MAX_PORT_NUM];
	int i, p = -1;
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
	}

	// keep the current port while it stays connected, otherwise the first
	// connected one
	if (port >= 0 && (info.port_status[port] & 1))
		p = port;
	else
		for (i = 0; i < MAX_PORT_NUM; i++)
			if (info.port_status[i] & 1) {
				p = i;
				break;
			}

	if (p < 0) {
		if (connected)
			PS3_Log("[pad] no pad connected");
		connected = 0;
		port = -1;
		ReleaseAll();
		nopad++;
		return;
	}

	if (!connected || p != port) {
		PS3_Logf("[pad] active pad on port %d (was %d)", p, port);
		ioPadClearBuf(p);
		ReleaseAll();
		failures = 0;
	}
	port = p;
	connected = 1;
	nopad = 0;

	r = ioPadGetData(port, &data);
	if (r != 0) {
		if (failures < 5)
			PS3_Logf("[pad] ioPadGetData(%d) = %d", port, (int)r);
		if (++failures == 180)
			PadReinit("GetData failing");
		// keep the previous state: don't release buttons
		return;
	}
	failures = 0;
	// NOT bailing out on len == 0 on purpose, see the header
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
